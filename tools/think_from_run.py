#!/usr/bin/env python3
"""Rebuild what the network was thinking during a best run from training.

Training doesn't record the network's activations (it plays 1024 players at once), but
for each best run it saves what was chosen at every decision and the versions of the
network that chose it (train/ppo.py). This replays the run in the sim, decision by
decision, and runs each decision's observation through the exact network version that
made it: the same numbers the network computed at the time. The result has the same
format as tools/play_agent.py's, for tools/overlay.py:

    <out>.think.npz     frame, obs, hidden1, hidden2, probs, action, value, version
    <out>.weights.pt    the network versions (overlay.py takes it as its agent)
    <out>.run           the run itself, checked against the replay

    .venv/bin/python tools/think_from_run.py runs/tatmt-s2/finish1/best_time -o build/runs/tatmt-s2_finish
"""

import argparse
import shutil
import sys
from pathlib import Path

import numpy as np
import torch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "py"))
sys.path.insert(0, str(ROOT / "train"))

from gauntlet import env as genv, runfile, sim  # noqa: E402
from ppo import Agent  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", help="a best run from training, without extension (e.g. runs/<map>/<name>/best_time)")
    ap.add_argument("-o", "--out", required=True, help="output path, without extension")
    args = ap.parse_args()

    base = Path(args.run)
    run = runfile.read(base.with_suffix(".run"))
    saved = np.load(base.with_suffix(".decisions.npz"))
    actions, versions = saved["action"].astype(np.int64), saved["version"]
    weights = torch.load(base.with_suffix(".weights.pt"), map_location="cpu")

    # replay from the spawn, the way training played it (no time limits: an episode
    # restarted from a remembered point had its own clock, so they could cut in early)
    frames = len(run.frames) - genv.SETTLE_FRAMES
    env = genv.VecEnv(run.map, 1, max_seconds=frames * sim.FRAME_MSEC / 1000 + 1, threads=1, stuck_seconds=None)
    obs = env.reset()
    agent, loaded = Agent(), None
    record = {k: [] for k in ("obs", "hidden1", "hidden2", "probs", "action", "value", "version")}
    for k, (action, version) in enumerate(zip(actions, versions)):
        if version != loaded:
            agent.load_state_dict(weights[int(version)])
            loaded = version
        with torch.no_grad():
            x = torch.tensor(obs[0])
            h1 = agent.body[1](agent.body[0](x))
            h2 = agent.body[3](agent.body[2](h1))
            logits = torch.split(agent.policy(h2), genv.ACTION_SIZES)
            probs = torch.cat([torch.softmax(l, -1) for l in logits])
            value = float(agent.value(h2))
        for key, v in (("obs", obs[0]), ("hidden1", h1.numpy()), ("hidden2", h2.numpy()), ("probs", probs.numpy()),
                       ("action", action), ("value", value), ("version", int(version))):
            record[key].append(v)
        obs, _, done, info = env.step(action[None])
        if done[0] and k < len(actions) - 1:
            sys.exit(f"the replay ended early, at decision {k} of {len(actions)}")

    # the replay must have played exactly the run's inputs (a best run is saved when its
    # episode ends, so the replay's last decision ends it too)
    played = info["runs"][0] if done[0] else env.history[0, :env.frames[0]]
    if env.make_run(played).frames != run.frames:
        sys.exit("the replay's inputs differ from the run's")

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(out.with_suffix(".think.npz"),
                        frame=genv.SETTLE_FRAMES + np.arange(len(actions)) * genv.FRAMES_PER_ACTION,
                        **{k: np.array(v) for k, v in record.items()})
    shutil.copy(base.with_suffix(".weights.pt"), out.with_suffix(".weights.pt"))
    shutil.copy(base.with_suffix(".run"), out.with_suffix(".run"))
    print(f"wrote {out.with_suffix('.think.npz')} ({len(actions)} decisions, "
          f"{len(np.unique(versions))} network versions) and {out.with_suffix('.weights.pt')}")


if __name__ == "__main__":
    main()
