"""The training environment: many players racing one map in the sim at once.

The AI makes one decision every FRAMES_PER_ACTION frames (4 frames = 32 ms) for
every player at the same time. A decision is six choices, like a keyboard and mouse:

    forward:  back / none / forward
    side:     left / none / right
    jump:     no / yes (VQ3 has no auto-hop, so jump must be let go before the next one)
    turn:     one of TURN_RATES, degrees per frame, held for the whole decision
    fire:     no / yes (only the rocket launcher fires in the sim; it's held whenever owned)
    aim:      one of PITCHES, the up/down view angle for the decision (down is positive)

Rewards come from the route (sim/src/route.c). A run is a list of legs, one per zone:
the start zone (which starts the timer), each checkpoint (nearest next, along the
route), then the finish. On each leg, getting closer earns points; reaching a zone
earns a small bonus and finishing a big one; each rocket fired costs a little. Dying
ends the episode and takes back the points of the last DEATH_WINDOW seconds, but
none from before the last zone reached (as if respawning where the player was a few
seconds ago): "rush ahead, then die" never pays, but what came before is safe, so
trying a risky jump isn't a gamble with the whole run. Taking back the whole
episode's points made the AI stand still in front of the first dangerous jump
rather than risk it; taking back the whole leg's did the same on maps with a long
last leg (tatmt-s2's runs from the last checkpoint to the finish).
Episodes also end after max_seconds, or after STUCK_SECONDS without getting closer to
the current zone (players stuck in front of a jump make way for new episodes instead
of waiting out the clock). Every frame's input is kept, so any episode can be
exported as a .run file and replayed in the real game. So is every decision, with the
network version that made it (set `version`), so the network's exact thinking during
any episode can be rebuilt afterwards (tools/think_from_run.py).

Restart points (as in Go-Explore): the env remembers a snapshot of the places
players reach, one per cell (a leg, a slice of the run's progress, a band of speed,
and what the player carries: the rocket launcher, rockets). With restart_fraction > 0, that share
of episodes starts from a remembered cell instead of the spawn, cells few episodes
have reached first. This matters on maps where a mistake shows only much later: on
tatmt-s2 the map hands out one rocket, which the last jump needs, and a player who
fires it on the way can't finish. Restarting near the jump with the rocket still in
hand lets the AI learn the jump, and then that the rocket is worth keeping. A
snapshot keeps the inputs that led to it, so a restarted episode is still a whole
run from the spawn, timer included, and exports like any other.

Scouts help find such places: the last `scouts` players never fire (they keep their
ammo for later). They restart too, but only from cells where they still carry
rockets, so they push on from the furthest such cell, step by step. They aren't
trained on (train/ppo.py leaves them out) and their visits count separately, so a
cell only scouts have reached, like "at the rocket jump, rocket still in hand", is
where the learners' restarts go first.

Ammo restarts go further: when a learner restarts from a place it reached after
firing, AMMO_RESTARTS of the time it gets back the rockets it had picked up, as if it
had kept them. That lets the AI practise a jump that needs a rocket at the end of a
map before it has learned to keep one that long. Such a practice episode never
happened in a real run, so it is marked synthetic: it isn't exported and doesn't
count for the best runs or averages. Its visits count separately (so it doesn't hide
the real restart points it passes), and the places it reaches are remembered as
practice-only restart points: restarting from one is practice too. That way practice
can carry on past a rocket jump to the end of the map.
"""

import collections

import ctypes as C
import os

import numpy as np

from . import bots, runfile, sim

FRAMES_PER_ACTION = 4
TURN_RATES = np.array([-6, -3, -1.5, -0.75, -0.25, 0, 0.25, 0.75, 1.5, 3, 6], dtype=np.float32)
PITCHES = np.array([-15, 0, 20, 45, 70, 89], dtype=np.float32)
ACTION_SIZES = (3, 3, 2, len(TURN_RATES), 2, len(PITCHES))   # forward, side, jump, turn, fire, aim
SETTLE_FRAMES = 125                           # standing still after spawning, as in .run files

OBS_SIZE = 52                                 # GS_OBS_SIZE in gauntlet.h

# numpy views of gs_cmd_t, gs_decision_t and gs_state_t (sim/include/gauntlet.h)
CMD_DTYPE = np.dtype([("angles", np.int32, 3), ("forward", np.int8), ("right", np.int8),
                      ("up", np.int8), ("buttons", np.uint8), ("weapon", np.uint8), ("unused", np.uint8, 3)])
DECISION_DTYPE = np.dtype([("forward", np.int8), ("right", np.int8), ("up", np.int8), ("buttons", np.uint8),
                           ("weapon", np.uint8), ("unused", np.uint8, 3), ("turn", np.float32), ("pitch", np.float32)])
STATE_DTYPE = np.dtype([(name, np.float32 if t._type_ is C.c_float else np.int32, t._length_)
                        if hasattr(t, "_length_") else (name, np.int32)
                        for name, t in sim.State._fields_])
assert CMD_DTYPE.itemsize == C.sizeof(sim.Cmd) and STATE_DTYPE.itemsize == C.sizeof(sim.State)

# rewards
PROGRESS_SCALE = 1 / 100    # per unit closer to the current zone
ZONE_BONUS = 0.5            # the start zone or a checkpoint reached. Keep it below the death
                            # penalty: on maps whose start line hangs over a pit, a bigger
                            # bonus pays for jumping in, starting the timer and dying.
FINISH_BONUS = 10.0
DEATH_PENALTY = -1.0        # on top of losing the last few seconds' points
DEATH_WINDOW = 5.0          # seconds of points a death takes back
ROCKET_COST = 0.25          # per rocket fired, so the AI fires only when the shot pays for
                            # itself (a rocket jump gains far more). Ammo can be scarce: on
                            # tatmt-s2 a rocket wasted on the way leaves no way up later.
# No per-decision time penalty: summed over an episode it would outweigh the death
# penalty and teach the AI to die early. Discounting (gamma in train/ppo.py) already
# makes sooner rewards worth more, which is what rewards speed.
CHECKPOINT_RADIUS = 8       # route distance at which a checkpoint zone counts as reached
STUCK_SECONDS = 10          # an episode ends after this long without getting a route cell
                            # (16 units) closer to the current zone

PROGRESS_CELLS = 100        # restart cells: slices of the run's progress per leg,
SPEED_STEP, SPEED_CELLS = 200, 6   # and bands of speed (ups; the last is 1000 and up)
AMMO_RESTARTS = 0.5         # share of learner restarts that get back fired rockets

# a remembered place to restart from: the env, and the episode so far (held: the most
# rockets the player carried on the way there; decisions and versions: what was
# chosen at each decision, and by which network version)
Restart = collections.namedtuple("Restart", "snapshot leg frames history returns recent held decisions versions")


def _ptr(array):
    return array.ctypes.data_as(C.c_void_p)


class VecEnv:
    def __init__(self, mapname, num_envs, max_seconds=30, threads=None, restart_fraction=0.0, scouts=0,
                 stuck_seconds=STUCK_SECONDS):
        self.map = mapname
        self.stuck_frames = stuck_seconds * 1000 / sim.FRAME_MSEC if stuck_seconds else np.inf
        self.n = num_envs
        self.threads = threads or os.cpu_count()
        self.max_frames = int(max_seconds * 1000 / sim.FRAME_MSEC)

        self.info = sim.load_map(mapname)
        sim.build_route()
        L = self.lib = sim.lib()
        L.gs_step_batch.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_int]
        L.gs_observe_batch.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_void_p, C.c_void_p, C.c_void_p]
        L.gs_decide_batch.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_int, C.c_int,
                                      C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p]
        L.gs_env_snapshot_size.restype = C.c_size_t
        L.gs_env_save.argtypes = [C.c_void_p, C.c_void_p]
        L.gs_env_restore.argtypes = [C.c_void_p, C.c_void_p]

        # The start state: spawned, then settled for a second. Every episode starts
        # from a copy of it, and exported runs begin with the same settle frames.
        start = sim.Env()
        s = start.spawn()
        self.start_delta = tuple(s.delta_angles)
        idle = bots.Idle()
        self.settle_frames = []
        for _ in range(SETTLE_FRAMES):
            cmd = idle(s)
            self.settle_frames.append((cmd.angles[0] & 65535, cmd.angles[1] & 65535, cmd.angles[2] & 65535,
                                       cmd.forward, cmd.right, cmd.up, cmd.buttons, cmd.weapon))
            s = start.step(cmd)
        self.snapshot = C.create_string_buffer(L.gs_env_snapshot_size())
        L.gs_env_save(start._env, self.snapshot)

        # The legs of a run, and how long each is along the route (for reporting
        # progress as a fraction of the whole run).
        self.legs = sim.route_legs(tuple(s.origin))
        if any(length < 0 for _, length in self.legs):
            raise RuntimeError(f"{mapname}: no route from the spawn through the start zone to the finish")
        self.leg_targets = np.array([t for t, _ in self.legs])
        self.leg_starts = np.cumsum([0] + [length for _, length in self.legs])   # route done before each leg
        self.start_route = float(self.leg_starts[-1])

        self.envs = [L.gs_env_new() for _ in range(self.n)]
        self.env_ptrs = (C.c_void_p * self.n)(*self.envs)
        self.choices = np.zeros(self.n, DECISION_DTYPE)                   # this decision, for the sim
        self.frame_cmds = np.zeros((self.n, FRAMES_PER_ACTION), CMD_DTYPE)  # the frames it played
        self.obs = np.zeros((self.n, OBS_SIZE), np.float32)
        self.states = np.zeros(self.n, STATE_DTYPE)
        self.route = np.zeros(self.n, np.float32)       # distance to the current leg's zone
        self.leg = np.zeros(self.n, np.int64)
        self.closest = np.full(self.n, np.inf, np.float32)   # closest to the current zone so far
        self.closer_frame = np.zeros(self.n, np.int64)       # the frame it last got closer
        # every frame's input since each env's episode started, for exporting runs
        self.history = np.zeros((self.n, self.max_frames + FRAMES_PER_ACTION, 8), np.int32)
        self.frames = np.zeros(self.n, np.int64)
        # every decision since each env's episode started (the actions as played), and
        # the network version that made it
        self.version = 0
        self.decisions = np.zeros((self.n, self.max_frames // FRAMES_PER_ACTION + 1, len(ACTION_SIZES)), np.int8)
        self.decision_versions = np.zeros(self.decisions.shape[:2], np.int32)
        self.returns = np.zeros(self.n, np.float32)    # reward so far this episode
        # the reward of each of the last DEATH_WINDOW seconds' decisions since the last
        # zone, a ring indexed by self.tick (the slot of tick % size is the oldest)
        self.recent = np.zeros((self.n, int(DEATH_WINDOW * 1000 / (FRAMES_PER_ACTION * sim.FRAME_MSEC))), np.float32)
        self.tick = 0

        # restart points: cell -> Restart (the fastest arrival there so far), how many
        # episodes (of players that aren't scouts) have reached each cell, and the cells
        # each player's episode has been in
        self.restart_fraction = restart_fraction
        self.seen = [set() for _ in range(self.n)]
        self.cell = np.full(self.n, -1, np.int64)      # each player's cell at its last decision
        self.scout = np.arange(self.n) >= self.n - scouts
        self.restarts = {}
        self.practice_restarts = {}                    # remembered by practice episodes
        self.visits = collections.Counter()
        self.scout_visits = collections.Counter()
        self.practice_visits = collections.Counter()
        self.from_spawn = np.ones(self.n, bool)        # this episode started at the spawn
        self.synthetic = np.zeros(self.n, bool)        # it started from an ammo restart
        self.held = np.zeros(self.n, np.int32)         # most rockets carried this episode
        self.rng = np.random.default_rng(0)

    # -- helpers ---------------------------------------------------------------

    def _progress(self, route, leg):
        """How far along the whole run each player is: 0 at the spawn, 1 at the finish."""
        done_so_far = self.leg_starts[leg + 1] - route
        return np.clip(done_so_far / self.start_route, 0, 1)

    def _leg_route(self, route, leg):
        """Distances left on each leg, counting one without a value as the whole leg."""
        return np.where(route < 0, self.leg_starts[leg + 1] - self.leg_starts[leg], route)

    def _observe(self, indices=None):
        """Fill obs, states and route for every player, or just for some."""
        if indices is None:
            self.lib.gs_observe_batch(self.env_ptrs, self.n, self.threads, _ptr(self.obs),
                                      _ptr(self.states), _ptr(self.route))
            return
        ptrs = (C.c_void_p * len(indices))(*[self.envs[i] for i in indices])
        obs = np.zeros((len(indices), OBS_SIZE), np.float32)
        states = np.zeros(len(indices), STATE_DTYPE)
        route = np.zeros(len(indices), np.float32)
        self.lib.gs_observe_batch(ptrs, len(indices), self.threads, _ptr(obs), _ptr(states), _ptr(route))
        self.obs[indices], self.states[indices], self.route[indices] = obs, states, route

    def _set_leg(self, i, leg):
        self.leg[i] = leg
        self.lib.gs_env_set_route_target(self.envs[i], int(self.leg_targets[leg]))

    def _recent_in_order(self, i):
        """Player i's recent rewards, oldest first (np.roll by self.tick puts them back)."""
        return np.roll(self.recent[i], -self.tick)

    def _decisions_of(self, i):
        """Player i's decisions this episode, and the network version of each."""
        d = self.frames[i] // FRAMES_PER_ACTION
        return self.decisions[i, :d].copy(), self.decision_versions[i, :d].copy()

    def versions_in_use(self):
        """The network versions that made a decision of a running episode or of a
        remembered restart point (the trainer can forget the others)."""
        used = [self.decision_versions[i, :self.frames[i] // FRAMES_PER_ACTION] for i in range(self.n)]
        used += [kept.versions for kept in self.restarts.values()]
        return set(np.unique(np.concatenate(used)).tolist()) if used else set()

    def _cells(self, indices):
        """The restart cell each player is in: leg, progress slice, speed band, and what it
        carries (the last 3 bits: rockets, up to 3, and the rocket launcher)."""
        s = self.states[indices]
        progress = (self._progress(self._leg_route(self.route[indices], self.leg[indices]), self.leg[indices])
                    * PROGRESS_CELLS).astype(np.int64)
        has_rl = (s["weapons"] >> sim.WP_ROCKET_LAUNCHER) & 1
        carry = np.minimum(s["rockets"], 3) * 2 + has_rl
        speed = np.minimum(np.hypot(s["velocity"][:, 0], s["velocity"][:, 1]) // SPEED_STEP, SPEED_CELLS - 1)
        return ((self.leg[indices] * (PROGRESS_CELLS + 1) + progress) * SPEED_CELLS + speed.astype(np.int64)) * 8 + carry

    def _remember(self, indices):
        """Count each player's cell if its episode is new there, and remember the cell
        if this is the fastest anyone has got there. Only players that just entered a
        cell can change anything: staying in one, a player only gets later."""
        cells = self._cells(indices)
        entered = cells != self.cell[indices]
        self.cell[indices] = cells
        for i, cell in zip(indices[entered], cells[entered]):
            visits = self.scout_visits if self.scout[i] else self.practice_visits if self.synthetic[i] else self.visits
            if cell not in self.seen[i]:
                self.seen[i].add(cell)
                visits[cell] += 1
            restarts = self.practice_restarts if self.synthetic[i] else self.restarts
            kept = restarts.get(cell)
            if kept is None or self.frames[i] < kept.frames:
                restarts[cell] = self.moment(i)

    def _restart(self, indices):
        """Start new episodes: from the spawn, or (restart_fraction of them) from a
        remembered cell, picked with weight 1 / sqrt(episodes that reached it + 1).
        Learners pick from the real and the practice-only cells (each counted by its
        own kind of episode); scouts only from real cells where they'd carry rockets."""
        # (restart point, is it practice-only) for each choice
        learner_cells = [(c, False) for c in self.restarts] + [(c, True) for c in self.practice_restarts]
        scout_cells = [(c, False) for c in self.restarts if (c % 8) // 2 > 0]
        restart = self.rng.random(len(indices)) < self.restart_fraction
        restart &= np.where(self.scout[indices], len(scout_cells) > 0, len(learner_cells) > 0)
        picks = {}
        for scouts, cells in ((False, learner_cells), (True, scout_cells)):
            group = restart & (self.scout[indices] == scouts)
            if group.any():
                visits = [(self.practice_visits if practice else self.scout_visits if scouts else self.visits)[c]
                          for c, practice in cells]
                weight = 1 / np.sqrt(np.array(visits) + 1)
                chosen = self.rng.choice(len(cells), size=group.sum(), p=weight / weight.sum())
                picks.update(zip(indices[group], (cells[c] for c in chosen)))
        self.synthetic[indices] = False
        self.cell[indices] = -1
        for k, i in enumerate(indices):
            self.seen[i] = set()
            if restart[k]:
                cell, practice = picks[i]
                kept = (self.practice_restarts if practice else self.restarts)[cell]
                self.synthetic[i] = practice
                self._resume(i, kept)
                rockets = (cell % 8) // 2
                if not practice and not self.scout[i] and rockets < kept.held and self.rng.random() < AMMO_RESTARTS:
                    self.lib.gs_env_set_ammo(self.envs[i], sim.WP_ROCKET_LAUNCHER, kept.held)
                    self.synthetic[i] = True
            else:
                self.lib.gs_env_restore(self.envs[i], self.snapshot)
                self._set_leg(i, 0)
                self.frames[i] = self.returns[i] = self.recent[i] = self.held[i] = 0
        self.from_spawn[indices] = ~restart
        self.closest[indices] = np.inf
        self.closer_frame[indices] = self.frames[indices]

    def moment(self, i):
        """Player i's env and episode so far, as a Restart to start other episodes from."""
        snapshot = C.create_string_buffer(len(self.snapshot))
        self.lib.gs_env_save(self.envs[i], snapshot)
        return Restart(snapshot, int(self.leg[i]), int(self.frames[i]), self.history[i, :self.frames[i]].copy(),
                       float(self.returns[i]), self._recent_in_order(i), int(self.held[i]), *self._decisions_of(i))

    def _resume(self, i, kept):
        """Put player i where a Restart was (the env and the episode so far)."""
        self.lib.gs_env_restore(self.envs[i], kept.snapshot)
        self._set_leg(i, kept.leg)
        self.frames[i] = kept.frames
        self.history[i, :kept.frames] = kept.history
        self.returns[i] = kept.returns
        self.recent[i] = np.roll(kept.recent, self.tick)
        self.held[i] = kept.held
        self.decisions[i, :len(kept.decisions)] = kept.decisions
        self.decision_versions[i, :len(kept.versions)] = kept.versions

    def start_all_from(self, kept):
        """Start every player's episode from one Restart (instead of the spawn), for
        searching or testing from a moment of a run. Returns the observations."""
        for i in range(self.n):
            self._resume(i, kept)
            self.seen[i] = set()
        self.from_spawn[:] = self.synthetic[:] = False
        self.cell[:] = -1
        self.closest[:] = np.inf
        self.closer_frame[:] = self.frames
        self._observe()
        return self.obs.copy()

    def seed_restarts(self, runs):
        """Remember restart points along saved runs (whole runs from the spawn, like an
        earlier training's best), as if a player had just played them: so training can
        restart from places those runs reached, at the speed they had there. Their
        decisions are recovered from the inputs; the network that made them is unknown
        (version -1)."""
        player = VecEnv(self.map, 1, max_seconds=self.max_frames * sim.FRAME_MSEC / 1000, threads=1,
                        restart_fraction=1e-9, stuck_seconds=None)
        player.version = -1
        for run in runs:
            player.reset()
            frames = np.array(run.frames[SETTLE_FRAMES:], np.int64)
            for k in range(len(frames) // FRAMES_PER_ACTION):
                action = player._action_for(frames[k * FRAMES_PER_ACTION:(k + 1) * FRAMES_PER_ACTION])
                if action is None:
                    raise ValueError(f"decision {k} of a seed run isn't one of this env's actions")
                if player.step(action[None])[2][0]:
                    break
        for cell, kept in player.restarts.items():
            if cell not in self.restarts or kept.frames < self.restarts[cell].frames:
                self.restarts[cell] = kept
        return len(player.restarts)

    def _action_for(self, frames):
        """The action (a row for step) that makes player 0 play these 4 frames of input, or None."""
        f = frames[0]
        delta = self.states["delta_angles"][0]
        pitch = np.nonzero((((PITCHES * 65536 / 360).astype(np.int64) - delta[0]) & 65535) == f[0])[0]
        for turn, rate in enumerate(TURN_RATES):
            # the yaw as step() works it out: added up frame by frame
            yaw, angles = np.float64(self.states["viewangles"][0, 1]), []
            for _ in range(FRAMES_PER_ACTION):
                yaw += rate
                angles.append((int(yaw * 65536 / 360) - int(delta[1])) & 65535)
            if angles == list(frames[:, 1]) and len(pitch):
                return np.array([f[3] // 127 + 1, f[4] // 127 + 1, f[5] // 127, turn, int(f[6] != 0), pitch[0]])
        return None

    def reset(self):
        self._restart(np.arange(self.n))
        self._observe()
        return self.obs.copy()

    # -- stepping --------------------------------------------------------------

    def step(self, actions):
        """actions: (n, 6) ints, one row per player. Returns obs, reward, done, info."""
        forward = (actions[:, 0] - 1) * 127
        side = (actions[:, 1] - 1) * 127
        jump = actions[:, 2] * 127
        turn = TURN_RATES[actions[:, 3]]
        fire = np.where(self.scout, 0, actions[:, 4]) * sim.BUTTON_ATTACK
        pitch = PITCHES[actions[:, 5]]

        prev_route = self.route.copy()
        prev_timer = self.states["timer_state"].copy()
        prev_rockets = self.states["rockets"].copy()
        has_rl = (self.states["weapons"] & (1 << sim.WP_ROCKET_LAUNCHER)) != 0
        everyone = np.arange(self.n)
        played = actions.copy()
        played[self.scout, 4] = 0
        self.decisions[everyone, self.frames // FRAMES_PER_ACTION] = played
        self.decision_versions[everyone, self.frames // FRAMES_PER_ACTION] = self.version

        # play the decision's frames and observe, all in the sim: each frame turns the
        # view by `turn` (the yaw added up in double precision) and looks at `pitch`
        c = self.choices
        c["forward"], c["right"], c["up"], c["buttons"] = forward, side, jump, fire
        c["weapon"] = np.where(has_rl, sim.WP_ROCKET_LAUNCHER, 0)
        c["turn"], c["pitch"] = turn, pitch
        self.lib.gs_decide_batch(self.env_ptrs, _ptr(c), self.n, FRAMES_PER_ACTION, self.threads,
                                 _ptr(self.frame_cmds), _ptr(self.obs), _ptr(self.states), _ptr(self.route))
        # remember the frames' input (episodes end before the history is full)
        rows = self.frames[:, None] + np.arange(FRAMES_PER_ACTION)
        f = self.frame_cmds
        self.history[everyone[:, None], rows] = np.stack(
            [f["angles"][..., 0], f["angles"][..., 1], f["angles"][..., 2], f["forward"], f["right"],
             f["up"], f["buttons"], f["weapon"]], axis=-1)
        self.frames += FRAMES_PER_ACTION
        self.held = np.maximum(self.held, self.states["rockets"])

        timer = self.states["timer_state"]
        dead = self.states["dead"] != 0
        # Where the route has no value (for a frame after a teleport the player can be
        # half inside the floor), keep the last known distance: no progress, no penalty.
        self.route = np.where(self.route < 0, prev_route, self.route)
        finished = timer == 2
        timeout = self.frames >= self.max_frames
        closer = self.route < self.closest - 16
        self.closest = np.where(closer, self.route, self.closest)
        self.closer_frame = np.where(closer, self.frames, self.closer_frame)
        timeout |= self.frames - self.closer_frame > self.stuck_frames

        # reaching the current leg's zone: the start zone starts the timer, a
        # checkpoint zone is reached when the route to it is (about) zero
        target = self.leg_targets[self.leg]
        on_last_leg = self.leg == len(self.legs) - 1
        known = (prev_route >= 0) & (self.route >= 0)    # distances with a value, before and now
        reached = np.where(target == sim.ROUTE_START, (prev_timer == 0) & (timer != 0),
                           (target >= sim.ROUTE_CHECKPOINT0) & (self.route >= 0) & (self.route <= CHECKPOINT_RADIUS))
        reached &= ~on_last_leg & ~dead

        # progress on the current leg; none on a step that reaches a zone (the next
        # distance is to a different zone), finishes or dies, or where a distance has
        # no value (right after a zone, the player can be somewhere the route can't see)
        progress = np.where(dead | reached | finished | ~known, 0, prev_route - self.route)
        reward = progress * PROGRESS_SCALE
        reward += np.where(reached, ZONE_BONUS, 0)
        reward += np.where(finished, FINISH_BONUS, 0)
        reward -= np.maximum(prev_rockets - self.states["rockets"], 0) * ROCKET_COST
        reward += np.where(dead, DEATH_PENALTY - self.recent.sum(1) - reward, 0)
        self.returns += reward
        self.recent[:, self.tick % self.recent.shape[1]] = reward
        self.recent[reached] = 0        # points from before a zone are safe
        self.tick += 1
        done = dead | finished | timeout

        # next leg for whoever reached their zone
        for i in np.nonzero(reached & ~done)[0]:
            self._set_leg(i, self.leg[i] + 1)
            self.route[i] = sim.route_distance(tuple(self.states["origin"][i]), int(self.leg_targets[self.leg[i]]))
            self.closest[i], self.closer_frame[i] = self.route[i], self.frames[i]
        if self.restart_fraction > 0:
            self._remember(np.nonzero(~done)[0])

        info = {}
        if done.any():
            ended = np.nonzero(done)[0]
            info = {
                "ended": ended,
                "finished": finished[ended],
                "dead": dead[ended],
                "timer_ms": self.states["timer_ms"][ended].copy(),
                "frames": self.frames[ended].copy(),
                "returns": self.returns[ended].copy(),
                "from_spawn": self.from_spawn[ended].copy(),
                "scout": self.scout[ended].copy(),
                "synthetic": self.synthetic[ended].copy(),
                # how far along the run each got (for dead players, where they were last alive)
                "progress": np.where(finished[ended], 1.0, self._progress(self._leg_route(
                    np.where(dead, prev_route, self.route)[ended], self.leg[ended]), self.leg[ended])),
            }
            info["runs"] = {i: self.history[i, :self.frames[i]].copy() for i in ended}
            info["decisions"] = {i: self._decisions_of(i) for i in ended}
            self._restart(ended)
            self._observe(ended)
        return self.obs.copy(), reward.astype(np.float32), done, info

    def make_run(self, frames):
        """A .run of one episode's inputs, with the settle frames in front."""
        run = runfile.Run(map=self.map, delta=self.start_delta, settle=SETTLE_FRAMES)
        run.frames = list(self.settle_frames) + [tuple(int(v) for v in f) for f in frames]
        return run
