#!/usr/bin/env python3
"""Teach a network to play a given run (behaviour cloning), then check it plays it.

Replays the run in the sim from the spawn, keeping the observation at every decision
and the action the run took there (recovered from its inputs). Then trains the
network to choose those actions, until it picks the run's action at every decision
with probability >= --confidence. Finally it lets the network play alone from the
spawn, always taking its most likely choice: because the sim is deterministic, a
network that has learned the run plays it again, and every decision is its own.

The usual use: a finishing run found by tools/search_ending.py (or an earlier
training's best) taught to the network, before training it further with ppo.py
--init, so it starts from a network that can finish.

    .venv/bin/python train/imitate.py runs/tatmt-s2/finish11/best_time.run \\
        --init runs/tatmt-s2/finish11/agent.pt -o build/tatmt-s2_imitated.pt
"""

import argparse
import sys
from pathlib import Path

import numpy as np
import torch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "py"))
sys.path.insert(0, str(ROOT / "train"))

from gauntlet import env as genv, runfile, sim  # noqa: E402
from ppo import Agent  # noqa: E402


def replay(run):
    """The observation at each of the run's decisions, and the action it took."""
    seconds = len(run.frames) * sim.FRAME_MSEC / 1000 + 1
    env = genv.VecEnv(run.map, 1, max_seconds=seconds, threads=1, stuck_seconds=None)
    obs = env.reset()
    frames = np.array(run.frames[genv.SETTLE_FRAMES:], np.int64)
    observations, actions = [], []
    for k in range(len(frames) // genv.FRAMES_PER_ACTION):
        action = env._action_for(frames[k * genv.FRAMES_PER_ACTION:(k + 1) * genv.FRAMES_PER_ACTION])
        if action is None:
            sys.exit(f"decision {k} isn't one of the env's actions")
        observations.append(obs[0])
        actions.append(action)
        obs, _, done, _ = env.step(action[None])
        if done[0]:
            break
    return np.array(observations), np.array(actions)


def play_greedy(agent, run):
    """Let the network play alone from the spawn, taking its most likely choices.
    Returns (finished, timer_ms, decisions played, whether it played the run exactly)."""
    seconds = len(run.frames) * sim.FRAME_MSEC / 1000 + 10
    env = genv.VecEnv(run.map, 1, max_seconds=seconds, threads=1, stuck_seconds=None)
    obs = env.reset()
    for k in range(int(seconds / (genv.FRAMES_PER_ACTION * sim.FRAME_MSEC / 1000))):
        with torch.no_grad():
            h = agent.body(torch.tensor(obs))
            logits = torch.split(agent.policy(h), genv.ACTION_SIZES, dim=-1)
            action = torch.stack([l.argmax(-1) for l in logits], -1).numpy()
        obs, _, done, info = env.step(action)
        if done[0]:
            same = env.make_run(info["runs"][0]).frames == run.frames
            return bool(info["finished"][0]), int(info["timer_ms"][0]), k + 1, same
    return False, 0, k + 1, False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", help="the run to teach (a whole run from the spawn)")
    ap.add_argument("--init", help="start from this agent.pt (default: a fresh network)")
    ap.add_argument("-o", "--out", required=True, help="where to save the taught agent.pt")
    ap.add_argument("--confidence", type=float, default=0.99,
                    help="train until every one of the run's choices has at least this probability")
    ap.add_argument("--steps", type=int, default=20000, help="at most this many training steps")
    ap.add_argument("--lr", type=float, default=3e-4)
    args = ap.parse_args()

    run = runfile.read(args.run)
    obs, actions = replay(run)
    print(f"{args.run}: {len(actions)} decisions to learn")

    agent = Agent()
    if args.init:
        agent.load_state_dict(torch.load(args.init, map_location="cpu"))
    x, a = torch.tensor(obs), torch.tensor(actions)
    # (the value head gets no gradient here, so it's left as it was)
    optimizer = torch.optim.Adam(agent.parameters(), lr=args.lr)
    for step in range(args.steps):
        logits = torch.split(agent.policy(agent.body(x)), genv.ACTION_SIZES, dim=-1)
        # the log-probability of the run's action, per decision and choice
        logp = torch.stack([torch.log_softmax(l, -1).gather(-1, a[:, c, None]).squeeze(-1)
                            for c, l in enumerate(logits)], -1)
        worst = logp.min().exp().item()            # the least likely of all the run's choices
        if step % 500 == 0 or worst >= args.confidence:
            print(f"  step {step:5d}: lowest probability of a choice the run made {worst:.4f}", flush=True)
            if worst >= args.confidence:
                break
        loss = -logp.sum(-1).mean()
        optimizer.zero_grad()
        loss.backward()
        optimizer.step()
    torch.save(agent.state_dict(), args.out)

    finished, timer_ms, played, same = play_greedy(agent, run)
    print(f"played alone from the spawn: {'finished in %.3f s' % (timer_ms / 1000) if finished else 'did not finish'}"
          f" after {played} decisions; the same run exactly: {same}")
    print(f"saved {args.out}")


if __name__ == "__main__":
    main()
