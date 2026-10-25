#!/usr/bin/env python3
"""Record what the network was "thinking" during a run, decision by decision.

Runs are deterministic, so this replays the .run in the sim, gives the trained
network (agent.pt) exactly the observation it saw at every decision, and saves
what it computed: its inputs, both hidden layers, the probability of every choice,
the choice it made and its value estimate. This is the data the stream overlay
draws, so the overlay shows the real network, not an animation.

    .venv/bin/python tools/think.py runs/vanilla_05/first/best_progress.run runs/vanilla_05/first/agent.pt

Writes <run>.think.npz next to the run, with one row per decision:

    frame        first frame of the decision (counting the settle frames, as in the .run)
    obs          (46,)  the inputs (layout in sim/include/gauntlet.h)
    hidden1      (256,) first hidden layer, after tanh
    hidden2      (256,) second hidden layer, after tanh
    probs        (19,)  probabilities: forward (3), side (3), jump (2), turn (11)
    action       (4,)   what was chosen: forward, side, jump, turn (indices)
    value        ()     how much reward the network expected from here on
"""

import argparse
import ctypes as C
import sys
from pathlib import Path

import numpy as np
import torch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "py"))
sys.path.insert(0, str(ROOT / "train"))

from gauntlet import env as genv, runfile, sim  # noqa: E402
from ppo import Agent  # noqa: E402


def decision_action(frames):
    """The (forward, side, jump, turn) indices behind one decision's frames."""
    first = frames[0]
    forward = int(np.sign(first[3])) + 1
    side = int(np.sign(first[4])) + 1
    jump = int(first[5] > 0)
    # the turn rate is the yaw change per frame, in degrees
    if len(frames) > 1:
        step = ((int(frames[1][1]) - int(first[1]) + 32768) % 65536 - 32768) * 360 / 65536
    else:
        step = 0.0
    turn = int(np.argmin(np.abs(genv.TURN_RATES - step)))
    return forward, side, jump, turn


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run")
    ap.add_argument("agent")
    args = ap.parse_args()

    run = runfile.read(args.run)
    sim.load_map(run.map)
    sim.build_route()
    L = sim.lib()
    L.gs_observe_batch.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_void_p, C.c_void_p, C.c_void_p]

    agent = Agent()
    agent.load_state_dict(torch.load(args.agent, map_location="cpu"))
    agent.eval()

    env = sim.Env()
    env.spawn()
    env_ptr = (C.c_void_p * 1)(env._env)
    obs = np.zeros(genv.OBS_SIZE, np.float32)
    cmds = list(run.cmds())
    frames = np.array(run.frames, np.int64)

    rows = {k: [] for k in ("frame", "obs", "hidden1", "hidden2", "probs", "action", "value")}
    for f in range(run.settle):
        env.step(cmds[f])
    for start in range(run.settle, len(cmds), genv.FRAMES_PER_ACTION):
        L.gs_observe_batch(env_ptr, 1, 1, obs.ctypes.data_as(C.c_void_p), None, None)
        with torch.no_grad():
            x = torch.tensor(obs)
            h1 = agent.body[1](agent.body[0](x))
            h2 = agent.body[3](agent.body[2](h1))
            logits = torch.split(agent.policy(h2), genv.ACTION_SIZES)
            probs = torch.cat([l.softmax(-1) for l in logits])
            value = agent.value(h2).item()
        rows["frame"].append(start)
        rows["obs"].append(obs.copy())
        rows["hidden1"].append(h1.numpy())
        rows["hidden2"].append(h2.numpy())
        rows["probs"].append(probs.numpy())
        rows["action"].append(decision_action(frames[start:start + genv.FRAMES_PER_ACTION]))
        rows["value"].append(value)
        for f in range(start, min(start + genv.FRAMES_PER_ACTION, len(cmds))):
            env.step(cmds[f])

    out = Path(args.run).with_suffix(".think.npz")
    np.savez_compressed(out, **{k: np.array(v) for k, v in rows.items()})

    # sanity check: the network should have given the chosen actions real probability
    probs, actions = np.array(rows["probs"]), np.array(rows["action"])
    offsets = np.cumsum((0,) + genv.ACTION_SIZES[:-1])
    chosen = probs[np.arange(len(actions))[:, None], offsets + actions]
    print(f"wrote {out}: {len(actions)} decisions; chosen actions had probability "
          f"{chosen.mean():.2f} on average (lowest {chosen.min():.3f})")


if __name__ == "__main__":
    main()
