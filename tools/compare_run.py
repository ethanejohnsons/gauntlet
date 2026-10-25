#!/usr/bin/env python3
"""Compare a gs_replay log from oDFe with the sim, frame by frame.

Replays the .run in the sim and checks every frame the engine logged (one per
server snapshot) for position and velocity. Exits 0 if everything is within the
tolerance, 1 otherwise.

    python3 tools/compare_run.py ~/.q3a/defrag/runs/vanilla_01_strafe.run ~/.q3a/defrag/gs_replay.log
"""

import argparse
import ctypes as C
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "py"))

from gauntlet import runfile, sim  # noqa: E402


def read_log(path):
    """Engine states by frame index: {frame: (origin, velocity, ground, pm_flags)}."""
    states, header = {}, {}
    for line in open(path):
        parts = line.split()
        if not parts:
            continue
        if not parts[0].lstrip("-").isdigit():
            header[parts[0]] = parts[1:]
            continue
        f = int(parts[0])
        # the log prints floats with %.9g, which round-trips to the exact float32
        v = [C.c_float(float(x)).value for x in parts[1:7]]
        states[f] = (tuple(v[0:3]), tuple(v[3:6]), int(parts[7]), int(parts[8]))
    return header, states


def sim_states(run):
    """Sim state after each frame of the run."""
    sim.load_map(run.map)
    env = sim.Env()
    env.spawn()
    out = []
    for cmd in run.cmds():
        s = env.step(cmd)
        out.append((tuple(s.origin), tuple(s.velocity), s.ground_entity))
    return out


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

    checked, exact, worst, first_bad = 0, 0, (0.0, None), None
    for f in sorted(engine):
        if f < run.settle or f >= len(ours):
            continue
        e_org, e_vel, _, _ = engine[f]
        s_org, s_vel, _ = ours[f]
        diff = max(max(abs(a - b) for a, b in zip(e_org, s_org)),
                   max(abs(a - b) for a, b in zip(e_vel, s_vel)))
        checked += 1
        exact += diff == 0
        if diff > worst[0]:
            worst = (diff, f)
        if diff > args.tol and first_bad is None:
            first_bad = f

    print(f"{run.map}: {len(run.frames)} frames in the run ({run.settle} settling), "
          f"{checked} engine snapshots compared")
    print(f"  exactly equal: {exact}/{checked}   worst difference: {worst[0]:.6g}"
          + (f" at frame {worst[1]}" if worst[1] is not None else ""))
    if first_bad is None:
        print(f"  OK: sim and engine agree within {args.tol} on every compared frame")
        return 0

    f = first_bad
    print(f"  FIRST DIFFERENCE at frame {f} ({(f - run.settle) * sim.FRAME_MSEC / 1000:.3f} s after settling):")
    e_org, e_vel, e_ground, e_flags = engine[f]
    s_org, s_vel, s_ground = ours[f]
    print(f"    engine origin {e_org}  velocity {e_vel}  ground {e_ground}")
    print(f"    sim    origin {s_org}  velocity {s_vel}  ground {s_ground}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
