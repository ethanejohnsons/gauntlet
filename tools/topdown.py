#!/usr/bin/env python3
"""Draw a top-down picture of a map as libgauntlet sees it, with a bot run on top.

The floor comes from collision traces only (no textures): brighter means higher.
Zones from the entity list are outlined: start timer (green), stop timer (red),
checkpoints (yellow), teleporters (purple), jump pads (cyan), kill zones (dark red).
The run is the scripted strafe jumper (py/gauntlet/bots.py), coloured by speed.

    python3 tools/topdown.py vanilla_01 -o vanilla_01.png
"""

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "py"))

from gauntlet import bots, sim  # noqa: E402


def floor_heights(info, cell):
    """For each cell, the first surface below the map's outer shell (sky/roof)."""
    L = sim.lib()
    mins, maxs = info.world_mins, info.world_maxs
    w = int((maxs[0] - mins[0]) / cell) + 1
    h = int((maxs[1] - mins[1]) / cell) + 1
    heights = {}
    start, end, hit = sim.Vec3(), sim.Vec3(), sim.Vec3()
    for j in range(h):
        for i in range(w):
            x = mins[0] + (i + 0.5) * cell
            y = mins[1] + (j + 0.5) * cell
            z = maxs[2] + 1
            surfaces = []
            # A trace that starts inside a brush passes out of it, so stepping just
            # under each hit walks down through every surface in this column.
            for _ in range(6):
                start[0], start[1], start[2] = x, y, z
                end[0], end[1], end[2] = x, y, mins[2] - 1
                if L.gs_trace(start, end, None, None, hit) >= 1.0:
                    break
                surfaces.append(hit[2])
                z = hit[2] - 0.5
            if len(surfaces) >= 2:
                heights[(i, j)] = surfaces[1]   # [0] is the outside of the roof/sky
    # Outside the playable area the first floor found is the bottom of the sky box,
    # the lowest surface of all; leave those cells as background.
    if heights:
        void = min(heights.values())
        heights = {k: z for k, z in heights.items() if z > void + 1}
    return heights, w, h


def bot_run(frames):
    """The scripted strafe jumper; returns (x, y, speed) per frame."""
    env = sim.Env()
    s = env.spawn()
    bot = bots.StrafeBot()
    path = []
    for _ in range(frames):
        s = env.step(bot(s))
        path.append((s.origin[0], s.origin[1], bots.hspeed(s)))
        if s.dead:
            break
    return path


ZONE_COLORS = {
    "target_startTimer": (60, 200, 90),
    "target_stopTimer": (230, 70, 70),
    "target_checkpoint": (235, 200, 60),
    "trigger_teleport": (170, 110, 230),
    "trigger_push": (80, 200, 220),
    "trigger_hurt": (150, 30, 30),
}


def zones():
    """Trigger boxes, coloured by what they (or their targets) do."""
    ents = sim.entities()
    by_name = {}
    for e in ents:
        if e["targetname"]:
            by_name.setdefault(e["targetname"], []).append(e["classname"])
    out = []
    for e in ents:
        cls = e["classname"]
        if not cls.startswith("trigger_"):
            continue
        kinds = [cls] + by_name.get(e["target"], []) if cls == "trigger_multiple" else [cls]
        for k in kinds:
            if k in ZONE_COLORS:
                out.append((ZONE_COLORS[k], e["absmin"], e["absmax"]))
                break
    return out


def speed_color(v, vmax):
    t = max(0.0, min(1.0, (v - 250) / max(1, vmax - 250)))
    return (int(40 + 215 * t), int(120 + 80 * (1 - abs(2 * t - 1))), int(255 * (1 - t)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("map")
    ap.add_argument("-o", "--out", default=None, help="output PNG (default: <map>.png)")
    ap.add_argument("--q3", default=sim.default_q3_path())
    ap.add_argument("--cell", type=float, default=8, help="units per pixel (default 8)")
    ap.add_argument("--frames", type=int, default=400, help="frames of the bot run")
    args = ap.parse_args()

    try:
        info = sim.load_map(args.map, args.q3)
    except RuntimeError as e:
        raise SystemExit(str(e))

    heights, w, h = floor_heights(info, args.cell)
    lo, hi = (min(heights.values()), max(heights.values())) if heights else (0, 1)
    scale = 2
    img = Image.new("RGB", (w * scale, h * scale), (18, 18, 22))
    d = ImageDraw.Draw(img)

    def px(x, y):  # world -> image (y up in the world, down in the image)
        return ((x - info.world_mins[0]) / args.cell * scale,
                (h - (y - info.world_mins[1]) / args.cell) * scale)

    for (i, j), z in heights.items():
        g = int(55 + 150 * (z - lo) / max(1, hi - lo))
        x0, y0 = i * scale, (h - 1 - j) * scale
        d.rectangle([x0, y0, x0 + scale - 1, y0 + scale - 1], fill=(g, g, g + 8))

    for color, amin, amax in zones():
        x0, y1 = px(amin[0], amin[1])
        x1, y0 = px(amax[0], amax[1])
        d.rectangle([x0, y0, x1, y1], outline=color, width=2)

    path = bot_run(args.frames)
    vmax = max(p[2] for p in path)
    for a, b in zip(path, path[1:]):
        d.line([px(a[0], a[1]), px(b[0], b[1])], fill=speed_color(b[2], vmax), width=3)
    sx, sy = px(info.spawn_origin[0], info.spawn_origin[1])
    d.ellipse([sx - 5, sy - 5, sx + 5, sy + 5], outline=(255, 255, 255), width=2)

    d.text((8, 6), f"{args.map}  |  floor height: dark = low, light = high  |  "
                   f"run: blue = slow, red = fast (max {vmax:.0f} ups)", fill=(230, 230, 230))
    out = args.out or f"{args.map}.png"
    img.save(out)
    print(f"wrote {out} ({img.width}x{img.height}), {len(heights)} floor cells, run max {vmax:.0f} ups")


if __name__ == "__main__":
    main()
