#!/usr/bin/env python3
"""Compare a gs_replay log from oDFe with the sim, frame by frame.

Replays the .run in the sim and checks position, velocity and health on every frame
the engine logged (one per server snapshot), skipping the run's settle frames. Exits
0 if every frame matches, 1 otherwise.

Health is compared as change since the first compared frame. Its starting value
differs: health over 100 ticks down once a second, and the engine player has been
standing at the spawn for longer than the sim's. A one-point wobble is allowed,
since those ticks land on different frames.

    python3 tools/compare_run.py ~/.q3a/defrag/runs/vanilla_01_strafe.run ~/.q3a/defrag/gs_replay.log
"""

import argparse
import ctypes as C
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "py"))

from gauntlet import runfile, sim  # noqa: E402


def read_log(path):
    """The engine's states as {frame: (origin, velocity, ground_entity, health)}, and
    the header lines ("map", "base", ...) as {key: [values]}. Older logs have no health
    column; it reads as None."""
    states, header = {}, {}
    for line in open(path):
        parts = line.split()
        if not parts:
            continue
        if not parts[0].isdigit():
            header[parts[0]] = parts[1:]
            continue
        # floats are printed with %.9g, which round-trips to the exact float32
        v = [C.c_float(float(x)).value for x in parts[1:7]]
        health = int(parts[8]) if len(parts) > 8 else None
        states[int(parts[0])] = (tuple(v[0:3]), tuple(v[3:6]), int(parts[7]), health)
    return header, states


def sim_states(run):
    """The sim's state after each frame of the run, in the same form as read_log."""
    sim.load_map(run.map)
    env = sim.Env()
    env.spawn()
    out = []
    for cmd in run.cmds():
        s = env.step(cmd)
        out.append((tuple(s.origin), tuple(s.velocity), s.ground_entity, s.health))
    return out


def difference(a, b, health_offset):
    """Largest per-axis difference in position or velocity between two states; health
    changing differently (beyond the one-point tick wobble) counts as infinite."""
    if a[3] is not None and health_offset is not None and abs((a[3] - b[3]) - health_offset) > 1:
        return float("inf")
    return max(abs(x - y) for x, y in zip(a[0] + a[1], b[0] + b[1]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run")
    ap.add_argument("log")
    ap.add_argument("--tol", type=float, default=0.01, help="max allowed difference per axis (default 0.01)")
    args = ap.parse_args()

    run = runfile.read(args.run)
    header, engine = read_log(args.log)
    if header.get("map", [run.map])[0] != run.map:
        print(f"warning: log is from {header['map'][0]}, run is for {run.map}")
    ours = sim_states(run)

    frames = [f for f in sorted(engine) if run.settle <= f < len(ours)]
    offset = engine[frames[0]][3] - ours[frames[0]][3] if frames and engine[frames[0]][3] is not None else None
    diffs = {f: difference(engine[f], ours[f], offset) for f in frames}
    exact = sum(d == 0 for d in diffs.values())
    worst = max(frames, key=diffs.get) if frames else None
    bad = [f for f in frames if diffs[f] > args.tol]

    print(f"{run.map}: {len(run.frames)} frames in the run ({run.settle} settling), "
          f"{len(frames)} engine snapshots compared")
    if worst is not None:
        where = f" at frame {worst}" if diffs[worst] else ""
        print(f"  exactly equal: {exact}/{len(frames)}   worst difference: {diffs[worst]:.6g}{where}")
    if not bad:
        print(f"  OK: sim and engine agree within {args.tol} on every compared frame")
        return 0

    f = bad[0]
    print(f"  FIRST DIFFERENCE at frame {f} ({(f - run.settle) * sim.FRAME_MSEC / 1000:.3f} s after settling):")
    print(f"    engine origin {engine[f][0]}  velocity {engine[f][1]}  ground {engine[f][2]}  health {engine[f][3]}")
    print(f"    sim    origin {ours[f][0]}  velocity {ours[f][1]}  ground {ours[f][2]}  health {ours[f][3]}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
