#!/usr/bin/env python3
"""Let a trained network play a map, and keep its best attempt with its thinking.

Plays `--attempts` episodes at once in the sim, keeps the one that got furthest
(or finished fastest), and saves two files. By default the network picks its moves
at random by its own probabilities, as in training, so attempts differ; with
--greedy it always takes its most likely move, which looks more decisive on screen
(every attempt is then the same).

    <out>.run         the inputs, for gs_replay / tools/render_run.sh
    <out>.think.npz   what the network computed at every decision of that same
                      attempt (layout: tools/think.py)

Both come from the same playthrough, so the overlay can't drift from what the
network did.

    .venv/bin/python tools/play_agent.py tatmt-s2 runs/tatmt-s2/second/agent.pt -o build/runs/tatmt-s2_ai
"""

import argparse
import sys
from pathlib import Path

import numpy as np
import torch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "py"))
sys.path.insert(0, str(ROOT / "train"))

from gauntlet import env as genv, runfile  # noqa: E402
from ppo import Agent  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("map")
    ap.add_argument("agent")
    ap.add_argument("-o", "--out", required=True, help="output path, without extension")
    ap.add_argument("--attempts", type=int, default=64)
    ap.add_argument("--max-seconds", type=float, default=30)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--greedy", action="store_true", help="always take the most likely move")
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    env = genv.VecEnv(args.map, args.attempts, max_seconds=args.max_seconds)
    agent = Agent()
    agent.load_state_dict(torch.load(args.agent, map_location="cpu"))
    agent.eval()

    n = args.attempts
    record = [{k: [] for k in ("obs", "hidden1", "hidden2", "probs", "action", "value")} for _ in range(n)]
    result = [None] * n          # (finished, timer_ms, progress, frames) once the first episode ends
    obs = env.reset()
    decision = 0
    while any(r is None for r in result):
        with torch.no_grad():
            x = torch.tensor(obs)
            h1 = agent.body[1](agent.body[0](x))
            h2 = agent.body[3](agent.body[2](h1))
            logits = torch.split(agent.policy(h2), genv.ACTION_SIZES, dim=-1)
            dists = [torch.distributions.Categorical(logits=l) for l in logits]
            if args.greedy:
                actions = torch.stack([d.probs.argmax(-1) for d in dists], dim=-1)
            else:
                actions = torch.stack([d.sample() for d in dists], dim=-1)
            probs = torch.cat([d.probs for d in dists], dim=-1)
            value = agent.value(h2).squeeze(-1)
        for i in range(n):
            if result[i] is None:
                rec = record[i]
                rec["obs"].append(obs[i])
                rec["hidden1"].append(h1[i].numpy())
                rec["hidden2"].append(h2[i].numpy())
                rec["probs"].append(probs[i].numpy())
                rec["action"].append(actions[i].numpy())
                rec["value"].append(float(value[i]))
        obs, _, _, info = env.step(actions.numpy())
        decision += 1
        if info:
            for k, i in enumerate(info["ended"]):
                if result[i] is None:
                    result[i] = (bool(info["finished"][k]), int(info["timer_ms"][k]),
                                 float(info["progress"][k]), info["runs"][i])

    # best: a finish beats no finish; then the faster time, or the further progress
    best = max(range(n), key=lambda i: (result[i][0], -result[i][1] if result[i][0] else result[i][2]))
    finished, timer_ms, progress, frames = result[best]
    run = env.make_run(frames)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    runfile.write(out.with_suffix(".run"), run)

    rec = record[best]
    decisions = len(rec["obs"])
    np.savez_compressed(
        out.with_suffix(".think.npz"),
        frame=genv.SETTLE_FRAMES + np.arange(decisions) * genv.FRAMES_PER_ACTION,
        **{k: np.array(v) for k, v in rec.items()},
    )
    progresses = [r[2] for r in result]
    print(f"{n} attempts: progress mean {np.mean(progresses):.1%}, best {progress:.1%}"
          + (f", finished in {timer_ms / 1000:.3f} s" if finished else ""))
    print(f"wrote {out.with_suffix('.run')} and {out.with_suffix('.think.npz')} ({decisions} decisions)")


if __name__ == "__main__":
    main()
