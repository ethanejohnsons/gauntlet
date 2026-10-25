#!/usr/bin/env python3
"""Run a scripted bot in the sim and save its inputs as a .run file for gs_replay.

The run starts with one second of standing still, so the engine player (who may
have been idle at the spawn for a while) and the sim player begin from the same
settled state.

    python3 tools/export_run.py vanilla_01 --bot strafe
    # then in oDFe on that map:  /gs_replay runs/vanilla_01_strafe.run
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "py"))

from gauntlet import bots, runfile, sim  # noqa: E402

BOTS = {"strafe": bots.StrafeBot, "idle": bots.Idle}
IDLE_FRAMES = 125


def record(mapname, bot, frames):
    sim.load_map(mapname)
    env = sim.Env()
    s = env.spawn()
    run = runfile.Run(map=mapname, delta=tuple(s.delta_angles), settle=IDLE_FRAMES)
    idle = bots.Idle()
    for f in range(IDLE_FRAMES + frames):
        cmd = idle(s) if f < IDLE_FRAMES else bot(s)
        run.add(cmd)
        s = env.step(cmd)
        if s.dead:
            break
    return run, s


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("map")
    ap.add_argument("--bot", choices=BOTS, default="strafe")
    ap.add_argument("--frames", type=int, default=400, help="bot frames after the idle second")
    ap.add_argument("--home", default=str(Path.home() / ".q3a"),
                    help="oDFe fs_homepath; the run goes to <home>/defrag/runs/ (default ~/.q3a)")
    ap.add_argument("-o", "--out", help="write here instead")
    args = ap.parse_args()

    run, s = record(args.map, BOTS[args.bot](), args.frames)
    out = Path(args.out) if args.out else Path(args.home) / "defrag/runs" / f"{args.map}_{args.bot}.run"
    out.parent.mkdir(parents=True, exist_ok=True)
    runfile.write(out, run)
    print(f"wrote {out}: {len(run.frames)} frames ({len(run.frames) * sim.FRAME_MSEC / 1000:.2f} s)")
    print(f"sim end: origin ({s.origin[0]:.1f} {s.origin[1]:.1f} {s.origin[2]:.1f}), "
          f"speed {bots.hspeed(s):.0f} ups{', dead' if s.dead else ''}")
    if out.is_relative_to(Path(args.home) / "defrag"):
        print(f"in oDFe on {args.map}:  /gs_replay {out.relative_to(Path(args.home) / 'defrag')}")


if __name__ == "__main__":
    main()
