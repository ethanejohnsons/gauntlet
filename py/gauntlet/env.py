"""The training environment: many players racing one map in the sim at once.

The AI makes one decision every FRAMES_PER_ACTION frames (4 frames = 32 ms) for
every player at the same time. A decision is four choices, like a keyboard and a
mouse that only turns left and right:

    forward:  back / none / forward
    side:     left / none / right
    jump:     no / yes (VQ3 has no auto-hop, so jump must be let go before the next one)
    turn:     one of TURN_RATES, degrees per frame, held for the whole decision

Rewards come from the route (sim/src/route.c). A run has two legs: to the start
zone (which starts the timer), then to the finish. On each leg, getting closer earns
points; starting the timer and finishing earn a bonus. Dying ends the episode and
takes back every point earned in it, so an episode is worth how far the player got
if it survived, and nothing if it died; no map can make "rush ahead, then die" pay.
Episodes also end after max_seconds. Every frame's input is kept, so any episode can be exported as a .run
file and replayed in the real game.
"""

import ctypes as C
import os

import numpy as np

from . import bots, runfile, sim

FRAMES_PER_ACTION = 4
TURN_RATES = np.array([-6, -3, -1.5, -0.75, -0.25, 0, 0.25, 0.75, 1.5, 3, 6], dtype=np.float32)
ACTION_SIZES = (3, 3, 2, len(TURN_RATES))   # forward, side, jump, turn
SETTLE_FRAMES = 125                           # standing still after spawning, as in .run files

OBS_SIZE = 46                                 # GS_OBS_SIZE in gauntlet.h

# numpy views of gs_cmd_t and gs_state_t (sim/include/gauntlet.h)
CMD_DTYPE = np.dtype([("angles", np.int32, 3), ("forward", np.int8), ("right", np.int8),
                      ("up", np.int8), ("buttons", np.uint8), ("weapon", np.uint8), ("unused", np.uint8, 3)])
STATE_DTYPE = np.dtype([(name, np.float32 if t._type_ is C.c_float else np.int32, t._length_)
                        if hasattr(t, "_length_") else (name, np.int32)
                        for name, t in sim.State._fields_])
assert CMD_DTYPE.itemsize == C.sizeof(sim.Cmd) and STATE_DTYPE.itemsize == C.sizeof(sim.State)

# rewards
PROGRESS_SCALE = 1 / 100    # per unit closer to the current target
START_BONUS = 0.5           # the timer started. Keep it below the death penalty: on maps
                            # whose start line hangs over a pit, a bigger bonus pays for
                            # jumping in, starting the timer and dying.
FINISH_BONUS = 10.0
DEATH_PENALTY = -1.0        # on top of losing the episode's points so far
# No per-decision time penalty: summed over an episode it would outweigh the death
# penalty and teach the AI to die early. Discounting (gamma in train/ppo.py) already
# makes sooner rewards worth more, which is what rewards speed.


def _ptr(array):
    return array.ctypes.data_as(C.c_void_p)


class VecEnv:
    def __init__(self, mapname, num_envs, max_seconds=30, threads=None):
        self.map = mapname
        self.n = num_envs
        self.threads = threads or os.cpu_count()
        self.max_frames = int(max_seconds * 1000 / sim.FRAME_MSEC)

        self.info = sim.load_map(mapname)
        sim.build_route()
        L = self.lib = sim.lib()
        L.gs_step_batch.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_int]
        L.gs_observe_batch.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_void_p, C.c_void_p, C.c_void_p]
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

        # Route lengths, for reporting progress as a fraction of the whole run:
        # spawn -> start zone, then start zone -> finish (measured from the middle of
        # the start trigger).
        self.leg1 = sim.route_distance(tuple(s.origin), sim.ROUTE_START)
        zone = self._start_zone_center()
        self.leg2 = sim.route_distance(zone, sim.ROUTE_FINISH) if zone else -1
        if self.leg1 < 0 or self.leg2 < 0:
            raise RuntimeError(f"{mapname}: no route from the spawn through the start zone to the finish")
        self.start_route = self.leg1 + self.leg2

        self.envs = [L.gs_env_new() for _ in range(self.n)]
        self.env_ptrs = (C.c_void_p * self.n)(*self.envs)
        self.cmds = np.zeros(self.n, CMD_DTYPE)
        self.obs = np.zeros((self.n, OBS_SIZE), np.float32)
        self.states = np.zeros(self.n, STATE_DTYPE)
        self.route = np.zeros(self.n, np.float32)
        # every frame's input since each env's episode started, for exporting runs
        self.history = np.zeros((self.n, self.max_frames + FRAMES_PER_ACTION, 8), np.int32)
        self.frames = np.zeros(self.n, np.int64)
        self.returns = np.zeros(self.n, np.float32)    # reward so far this episode

    # -- helpers ---------------------------------------------------------------

    @staticmethod
    def _start_zone_center():
        """The middle of the first trigger that starts the timer."""
        ents = sim.entities()
        starts = {e["targetname"] for e in ents if e["classname"] == "target_startTimer"}
        for e in ents:
            if e["classname"] == "trigger_multiple" and e["target"] in starts:
                return tuple((a + b) / 2 for a, b in zip(e["absmin"], e["absmax"]))
        return None

    def _progress(self, route, timer_state):
        """How far along the whole run each player is: 0 at the spawn, 1 at the finish."""
        done_so_far = np.where(timer_state == 0, self.leg1 - route, self.leg1 + self.leg2 - route)
        return np.clip(done_so_far / self.start_route, 0, 1)

    def _observe(self):
        self.lib.gs_observe_batch(self.env_ptrs, self.n, self.threads, _ptr(self.obs),
                                  _ptr(self.states), _ptr(self.route))

    def _restart(self, indices):
        for i in indices:
            self.lib.gs_env_restore(self.envs[i], self.snapshot)
        self.frames[indices] = 0
        self.returns[indices] = 0

    def reset(self):
        self._restart(np.arange(self.n))
        self._observe()
        return self.obs.copy()

    # -- stepping --------------------------------------------------------------

    def step(self, actions):
        """actions: (n, 4) ints, one row per player. Returns obs, reward, done, info."""
        forward = (actions[:, 0] - 1) * 127
        side = (actions[:, 1] - 1) * 127
        jump = actions[:, 2] * 127
        turn = TURN_RATES[actions[:, 3]]

        prev_route = self.route.copy()
        prev_timer = self.states["timer_state"].copy()
        yaw = self.states["viewangles"][:, 1].astype(np.float64)
        delta = self.states["delta_angles"][:, 1]
        self.cmds["forward"] = forward
        self.cmds["right"] = side
        self.cmds["up"] = jump
        self.cmds["angles"][:, 0] = (-self.states["delta_angles"][:, 0]) & 65535   # level view
        everyone = np.arange(self.n)
        for _ in range(FRAMES_PER_ACTION):
            yaw += turn
            self.cmds["angles"][:, 1] = ((yaw * 65536 / 360).astype(np.int64) - delta) & 65535
            # remember this frame's input (episodes end before the history is full)
            self.history[everyone, self.frames, 0:3] = self.cmds["angles"]
            self.history[everyone, self.frames, 3] = forward
            self.history[everyone, self.frames, 4] = side
            self.history[everyone, self.frames, 5] = jump
            self.frames += 1
            self.lib.gs_step_batch(self.env_ptrs, _ptr(self.cmds), self.n, self.threads)
        self._observe()

        timer = self.states["timer_state"]
        dead = self.states["dead"] != 0
        # Where the route has no value (for a frame after a teleport the player can be
        # half inside the floor), keep the last known distance: no progress, no penalty.
        self.route = np.where(self.route < 0, prev_route, self.route)
        finished = timer == 2
        started = (prev_timer == 0) & (timer != 0)
        timeout = self.frames >= self.max_frames
        # progress on the current leg; none on the step that switches legs (the two
        # distances are to different targets) or dies
        progress = np.where(dead | started | finished, 0, prev_route - self.route)
        reward = progress * PROGRESS_SCALE
        reward += np.where(started, START_BONUS, 0)
        reward += np.where(finished, FINISH_BONUS, 0)
        reward += np.where(dead, DEATH_PENALTY - self.returns - reward, 0)
        self.returns += reward
        done = dead | finished | timeout

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
                # how far along the run each got (for dead players, where they were last alive)
                "progress": np.where(finished[ended], 1.0,
                                     self._progress(np.where(dead, prev_route, self.route)[ended],
                                                    np.where(dead, prev_timer, timer)[ended])),
            }
            info["runs"] = {i: self.history[i, :self.frames[i]].copy() for i in ended}
            self._restart(ended)
            self._observe()
        return self.obs.copy(), reward.astype(np.float32), done, info

    def make_run(self, frames):
        """A .run of one episode's inputs, with the settle frames in front."""
        run = runfile.Run(map=self.map, delta=self.start_delta, settle=SETTLE_FRAMES)
        run.frames = list(self.settle_frames) + [tuple(int(v) for v in f) for f in frames]
        return run
