#!/usr/bin/env python3
"""Train a player for one map with PPO (proximal policy optimization).

How it works, in short:

1. Play. 1024 copies of the AI race the map in the sim at once. Every 32 ms each
   one looks at its observation (52 numbers: its motion, distances to walls, the
   route ahead, health and weapon; see gauntlet.h) and the network picks keys, a
   turn rate, whether to fire, and where to aim.
2. Score. Each decision earns a reward: getting closer to the finish is good,
   finishing is very good, dying is bad (py/gauntlet/env.py).
3. Learn. After ROLLOUT decisions per player, the network is nudged so that
   decisions which led to more reward than expected become more likely, and
   worse ones less likely. "Proximal" means each nudge is kept small, so one
   unlucky batch can't wreck what was learned.
4. Repeat. Progress is logged to TensorBoard, and the best runs so far are saved
   as .run files you can watch in the real game.

    .venv/bin/python train/ppo.py vanilla_01
    .venv/bin/tensorboard --logdir runs/      # then open http://localhost:6006

Outputs go to runs/<map>/<name>/: TensorBoard logs, agent.pt (the network), and
best_progress.run / best_time.run. Each best run also gets a .decisions.npz (what was
chosen at every decision, and by which version of the network) and a .weights.pt
(those versions), so tools/think_from_run.py can rebuild exactly what the network
was thinking during it, for tools/overlay.py.
"""

import argparse
import contextlib
import copy
import sys
import threading
import time
from pathlib import Path

import numpy as np
import torch
from torch import nn
from torch.utils.tensorboard import SummaryWriter

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "py"))

from gauntlet import runfile  # noqa: E402
from gauntlet.env import ACTION_SIZES, OBS_SIZE, VecEnv  # noqa: E402


IS_CAP = 2.0    # the most a decision's weight can be, for decisions the actor made unlikely

# ---------------------------------------------------------------------------
# The network: observation in, a probability for every choice out, plus a guess
# at how much reward is still to come (the "value", used to judge decisions).

class Agent(nn.Module):
    def __init__(self, hidden=256):
        super().__init__()
        self.body = nn.Sequential(
            nn.Linear(OBS_SIZE, hidden), nn.Tanh(),
            nn.Linear(hidden, hidden), nn.Tanh(),
        )
        self.policy = nn.Linear(hidden, sum(ACTION_SIZES))   # scores for each choice
        self.value = nn.Linear(hidden, 1)
        # start out undecided: all choices about equally likely
        nn.init.orthogonal_(self.policy.weight, 0.01)
        nn.init.zeros_(self.policy.bias)
        # where each policy output goes in a (choices, options) grid; unused slots are -inf
        self.register_buffer("rows", torch.tensor([c for c, n in enumerate(ACTION_SIZES) for _ in range(n)]), False)
        self.register_buffer("cols", torch.tensor([k for n in ACTION_SIZES for k in range(n)]), False)

    def act(self, obs, actions=None):
        """Sample actions (or score given ones). Returns actions, their log-probability,
        the entropy (how undecided the policy is) and the value estimate.

        Each choice (forward, side, jump, turn, fire, aim) is a categorical
        distribution over its options. All six are worked out at once, as rows of one
        grid padded with -inf, and sampled with the Gumbel-max trick: a handful of GPU
        operations instead of a few per choice (deciding is on the critical path of
        every step)."""
        h = self.body(obs)
        grid = torch.full((obs.shape[0], len(ACTION_SIZES), max(ACTION_SIZES)), float("-inf"), device=obs.device)
        grid[:, self.rows, self.cols] = self.policy(h)
        log_probs = torch.log_softmax(grid, dim=-1)
        if actions is None:
            gumbel = -torch.log(-torch.log(torch.rand_like(log_probs)))
            actions = torch.argmax(log_probs + gumbel, dim=-1)
        logp = log_probs.gather(-1, actions[..., None]).squeeze(-1).sum(-1)
        # (the padding's log-probabilities are -inf: zero them first, as 0 * -inf would
        # make the gradients NaN)
        entropy = -(log_probs.exp() * log_probs.masked_fill(grid == float("-inf"), 0)).sum((-2, -1))
        return actions, logp, entropy, self.value(h).squeeze(-1)


# ---------------------------------------------------------------------------

def parse_args():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("map")
    ap.add_argument("--name", default=time.strftime("%Y%m%d-%H%M%S"), help="run name (default: date and time)")
    ap.add_argument("--envs", type=int, default=1024, help="players racing at once")
    ap.add_argument("--rollout", type=int, default=64, help="decisions per player between updates")
    ap.add_argument("--minutes", type=float, default=30, help="stop after this many minutes")
    ap.add_argument("--max-seconds", type=float, default=30, help="episode length limit, in game seconds")
    ap.add_argument("--threads", type=int, default=None, help="CPU threads for the sim (default: all)")
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--gamma", type=float, default=0.995, help="how much future reward counts (per decision)")
    ap.add_argument("--gae-lambda", type=float, default=0.95)
    ap.add_argument("--epochs", type=int, default=4, help="passes over each batch")
    ap.add_argument("--minibatches", type=int, default=8)
    ap.add_argument("--clip", type=float, default=0.2, help="how far one update may move the policy")
    ap.add_argument("--entropy", type=float, default=0.01, help="bonus for staying curious")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--init", help="start from this agent.pt instead of a fresh network")
    ap.add_argument("--restarts", type=float, default=0.25,
                    help="share of episodes that start from a remembered place instead of the spawn")
    ap.add_argument("--seed-runs", nargs="*", default=[], metavar="RUN",
                    help=".run files (whole runs from the spawn, e.g. an earlier training's best) to "
                         "seed restart points from")
    ap.add_argument("--scouts", type=int,
                    help="players that never fire and aren't trained on; they find places to restart "
                         "from (default: 1 in 16 players)")
    args = ap.parse_args()
    if args.scouts is None:
        args.scouts = args.envs // 16
    return args


def save_run(path, run, decisions, versions):
    """A best run: its .run, its decisions and the network versions that made them."""
    actions, used = decisions
    runfile.write(path.with_suffix(".run"), run)
    np.savez_compressed(path.with_suffix(".decisions.npz"), action=actions, version=used)
    # (decisions seeded from an earlier training's runs have version -1: no network here)
    torch.save({int(v): versions[int(v)] for v in np.unique(used) if int(v) in versions},
               path.with_suffix(".weights.pt"))


def main():
    args = parse_args()
    torch.manual_seed(args.seed)
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    out = ROOT / "runs" / args.map / args.name
    out.mkdir(parents=True, exist_ok=True)
    writer = SummaryWriter(out)
    writer.add_text("args", str(vars(args)))

    env = VecEnv(args.map, args.envs, max_seconds=args.max_seconds, threads=args.threads,
                 restart_fraction=args.restarts, scouts=args.scouts)
    if args.seed_runs:
        cells = env.seed_restarts([runfile.read(p) for p in args.seed_runs])
        print(f"seeded {cells} restart points from {len(args.seed_runs)} runs")
    agent = Agent().to(device)
    if args.init:
        agent.load_state_dict(torch.load(args.init, map_location=device))
    optimizer = torch.optim.Adam(agent.parameters(), lr=args.lr, eps=1e-5)
    print(f"{args.map}: route from spawn to finish is {env.start_route:.0f} units; "
          f"{args.envs} players on {device}; writing to {out.relative_to(ROOT)}")

    # Playing and learning overlap: while the sim plays the next rollout with `actor`
    # (a copy of the network), `agent` learns from the last one in a background thread,
    # on its own GPU stream. So the CPU and the GPU don't take turns waiting for each
    # other. After each rollout the actor takes the agent's latest weights: a rollout
    # is played by the network from one update back. Learning allows for that as
    # "decoupled" PPO does: the clipping keeps the agent near where it was when it
    # started on the rollout, and each decision is weighted by how much more (or less)
    # likely that network was to make it than the actor (capped at IS_CAP).
    actor = copy.deepcopy(agent)
    learn_stream = torch.cuda.Stream() if device.type == "cuda" else None
    learning, stats = None, {}

    # rollout storage, two sets (one being played into, one being learned from): one
    # row per decision, one column per player
    T, N = args.rollout, args.envs
    def rollout_buffers():
        return {"obs": torch.zeros((T, N, OBS_SIZE), device=device),
                "act": torch.zeros((T, N, len(ACTION_SIZES)), dtype=torch.long, device=device),
                **{k: torch.zeros((T, N), device=device) for k in ("logp", "rew", "done", "val")}}
    buffers = [rollout_buffers(), rollout_buffers()]

    def learn(buf, next_value):
        """Score a rollout's decisions and learn from them (runs in the background)."""
        with torch.cuda.stream(learn_stream) if learn_stream else contextlib.nullcontext():
            # --- 2. score: how much better than expected was each decision? --
            # GAE (generalized advantage estimation): for each decision, the rewards
            # that followed it, discounted, minus what the value head expected.
            with torch.no_grad():
                advantages = torch.zeros_like(buf["rew"])
                running = torch.zeros(N, device=device)
                for t in reversed(range(T)):
                    not_done = 1 - buf["done"][t]
                    following = next_value if t == T - 1 else buf["val"][t + 1]
                    delta = buf["rew"][t] + args.gamma * following * not_done - buf["val"][t]
                    running = delta + args.gamma * args.gae_lambda * not_done * running
                    advantages[t] = running
                returns = advantages + buf["val"]

            # --- 3. learn: a few passes of small, clipped updates ------------
            # (on the learners only: scouts, the last columns, don't play by the policy)
            L = N - args.scouts
            b_obs, b_act = buf["obs"][:, :L].reshape(-1, OBS_SIZE), buf["act"][:, :L].reshape(-1, len(ACTION_SIZES))
            b_logp, b_adv, b_ret = buf["logp"][:, :L].reshape(-1), advantages[:, :L].reshape(-1), returns[:, :L].reshape(-1)
            batch = T * L
            mb = batch // args.minibatches
            # the agent as it starts on this rollout ("proximal"), and how much more
            # likely it was than the actor to make each decision
            with torch.no_grad():
                b_prox = torch.cat([agent.act(b_obs[s:s + mb], b_act[s:s + mb])[1] for s in range(0, batch, mb)])
                b_weight = (b_prox - b_logp).exp().clamp(max=IS_CAP)
            for _ in range(args.epochs):
                order = torch.randperm(batch, device=device)
                for s in range(0, batch, mb):
                    idx = order[s:s + mb]
                    _, logp, entropy, value = agent.act(b_obs[idx], b_act[idx])
                    ratio = (logp - b_prox[idx]).exp()             # new probability / proximal
                    adv = (b_adv[idx] - b_adv[idx].mean()) / (b_adv[idx].std() + 1e-8) * b_weight[idx]
                    # the clipped PPO objective: follow the advantage, but not too far
                    policy_loss = -torch.min(ratio * adv, ratio.clamp(1 - args.clip, 1 + args.clip) * adv).mean()
                    value_loss = 0.5 * (value - b_ret[idx]).pow(2).mean()
                    loss = policy_loss + 0.5 * value_loss - args.entropy * entropy.mean()
                    optimizer.zero_grad()
                    loss.backward()
                    nn.utils.clip_grad_norm_(agent.parameters(), 0.5)
                    optimizer.step()
            stats.update(policy=policy_loss.item(), value=value_loss.item(), entropy=entropy.mean().item())

    obs = torch.tensor(env.reset(), device=device)
    best_progress, best_time = 0.0, None
    versions = {}               # network version (update number) -> its weights
    practice_finishes = 0       # finishes from ammo restarts (practice, not real runs)
    episodes, decisions, update = 0, 0, 0
    start = time.time()

    while time.time() - start < args.minutes * 60:
        update += 1
        # this rollout's network version: kept while a running episode or a restart
        # point has a decision it made, so best runs can be saved with their weights
        versions[update] = {k: v.detach().cpu().clone() for k, v in actor.state_dict().items()}
        env.version = update
        if update % 20 == 0:
            in_use = env.versions_in_use() | {update}
            versions = {v: w for v, w in versions.items() if v in in_use}

        # --- 1. play: collect a rollout -------------------------------------
        buf = buffers[update % 2]
        ended_progress, ended_returns, ended_finished = [], [], []
        for t in range(T):
            with torch.no_grad():
                actions, logp, _, value = actor.act(obs)
            next_obs, reward, done, info = env.step(actions.cpu().numpy())
            buf["obs"][t], buf["act"][t], buf["logp"][t], buf["val"][t] = obs, actions, logp, value
            buf["rew"][t] = torch.from_numpy(reward).to(device)
            buf["done"][t] = torch.from_numpy(done).to(device)
            obs = torch.from_numpy(next_obs).to(device)
            decisions += N

            if info:
                # (the averages count only the learners' episodes that started at the spawn)
                spawn = info["from_spawn"] & ~info["scout"]
                episodes += len(info["ended"])
                ended_progress += list(info["progress"][spawn])
                ended_returns += list(info["returns"][spawn])
                ended_finished += list(info["finished"][spawn])
                practice_finishes += int((info["finished"] & info["synthetic"]).sum())
                for k, i in enumerate(info["ended"]):
                    # keep the best runs so far, to watch in the real game (not ammo
                    # restarts: those never happened in a real run)
                    if info["synthetic"][k]:
                        continue
                    if info["finished"][k] and (best_time is None or info["timer_ms"][k] < best_time):
                        best_time = int(info["timer_ms"][k])
                        save_run(out / "best_time", env.make_run(info["runs"][i]), info["decisions"][i], versions)
                        print(f"  new best time: {best_time / 1000:.3f} s (episode {episodes})")
                    if info["progress"][k] > best_progress + 0.01:
                        best_progress = float(info["progress"][k])
                        save_run(out / "best_progress", env.make_run(info["runs"][i]), info["decisions"][i], versions)
        with torch.no_grad():
            next_value = actor.act(obs)[3]

        # hand the rollout to the learner (once it's done with the last one), and play
        # the next one with the weights it has learned so far
        if learning:
            learning.join()
        actor.load_state_dict(agent.state_dict())
        if learn_stream:
            learn_stream.wait_stream(torch.cuda.current_stream())    # the rollout is written
        learning = threading.Thread(target=learn, args=(buf, next_value))
        learning.start()

        # --- 4. report -------------------------------------------------------
        elapsed = time.time() - start
        writer.add_scalar("speed/decisions_per_second", decisions / elapsed, decisions)
        if stats:
            writer.add_scalar("loss/policy", stats["policy"], decisions)
            writer.add_scalar("loss/value", stats["value"], decisions)
            writer.add_scalar("policy/entropy", stats["entropy"], decisions)
        writer.add_scalar("best/progress", best_progress, decisions)
        if best_time is not None:
            writer.add_scalar("best/time_s", best_time / 1000, decisions)
        if ended_progress:
            writer.add_scalar("episode/progress", np.mean(ended_progress), decisions)
            writer.add_scalar("episode/return", np.mean(ended_returns), decisions)
            writer.add_scalar("episode/finish_rate", np.mean(ended_finished), decisions)
        if update % 10 == 0:
            torch.save(actor.state_dict(), out / "agent.pt")
            mean_progress = np.mean(ended_progress) if ended_progress else 0
            print(f"update {update:5d}  {elapsed / 60:5.1f} min  {decisions / 1e6:7.1f}M decisions  "
                  f"episodes {episodes:8d}  progress mean {mean_progress:5.1%} best {best_progress:5.1%}"
                  + (f"  best time {best_time / 1000:.3f} s" if best_time else "")
                  + (f"  restart cells {len(env.restarts)}" if env.restarts else "")
                  + (f" (+{len(env.practice_restarts)} practice)" if env.practice_restarts else "")
                  + (f"  practice finishes {practice_finishes}" if practice_finishes else ""))

    if learning:
        learning.join()
    torch.save(agent.state_dict(), out / "agent.pt")
    writer.close()
    print(f"done: best progress {best_progress:.1%}"
          + (f", best time {best_time / 1000:.3f} s" if best_time else ", no finish yet"))


if __name__ == "__main__":
    main()
