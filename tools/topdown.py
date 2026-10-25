#!/usr/bin/env python3
"""Draw a top-down picture of a map as libgauntlet sees it, with a bot run on top.

The floor comes from collision traces only (no textures): brighter means higher.
Zones from the entity list are outlined: start timer (green), stop timer (red),
checkpoints (yellow), teleporters (purple), jump pads (cyan), kill zones (dark red).
The run is the scripted strafe jumper from sim/tests/test_strafe.c, coloured by speed.

    python3 tools/topdown.py vanilla_01 -o vanilla_01.png
"""

import argparse
import ctypes as C
import math
import os
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent


class Cmd(C.Structure):
    _fields_ = [("angles", C.c_int32 * 3), ("forward", C.c_int8), ("right", C.c_int8),
                ("up", C.c_int8), ("buttons", C.c_uint8)]


class State(C.Structure):
    _fields_ = [("origin", C.c_float * 3), ("velocity", C.c_float * 3),
                ("viewangles", C.c_float * 3), ("delta_angles", C.c_int32 * 3),
                ("command_time", C.c_int32), ("pm_flags", C.c_int32), ("pm_time", C.c_int32),
                ("ground_entity", C.c_int32), ("frame", C.c_int32), ("timer_state", C.c_int32),
                ("timer_ms", C.c_int32), ("checkpoints", C.c_int32), ("dead", C.c_int32),
                ("haste_ms", C.c_int32), ("flight_ms", C.c_int32)]


class MapInfo(C.Structure):
    _fields_ = [("mapname", C.c_char * 64), ("source", C.c_char * 512),
                ("num_entities", C.c_int32), ("num_unsupported", C.c_int32),
                ("has_timer", C.c_int32), ("spawn_origin", C.c_float * 3),
                ("spawn_angles", C.c_float * 3), ("world_mins", C.c_float * 3),
                ("world_maxs", C.c_float * 3)]


class Entity(C.Structure):
    _fields_ = [("classname", C.c_char * 64), ("target", C.c_char * 64),
                ("targetname", C.c_char * 64), ("origin", C.c_float * 3),
                ("absmin", C.c_float * 3), ("absmax", C.c_float * 3), ("supported", C.c_int32)]


def load_lib():
    lib = C.CDLL(str(ROOT / "build/cmake/sim/libgauntlet.so"))
    lib.gs_load_map.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p]
    lib.gs_last_error.restype = C.c_char_p
    lib.gs_env_new.restype = C.c_void_p
    lib.gs_env_spawn.argtypes = [C.c_void_p]
    lib.gs_env_step.argtypes = [C.c_void_p, C.POINTER(Cmd)]
    lib.gs_env_get_state.argtypes = [C.c_void_p, C.POINTER(State)]
    lib.gs_trace.argtypes = [C.c_float * 3, C.c_float * 3, C.c_void_p, C.c_void_p, C.c_float * 3]
    lib.gs_trace.restype = C.c_float
    lib.gs_entity.argtypes = [C.c_int, C.POINTER(Entity)]
    return lib


def floor_heights(lib, info, cell):
    """For each cell, the first surface below the map's outer shell (sky/roof)."""
    mins, maxs = info.world_mins, info.world_maxs
    w = int((maxs[0] - mins[0]) / cell) + 1
    h = int((maxs[1] - mins[1]) / cell) + 1
    heights = {}
    end = (C.c_float * 3)()
    hit = (C.c_float * 3)()
    for j in range(h):
        for i in range(w):
            x = mins[0] + (i + 0.5) * cell
            y = mins[1] + (j + 0.5) * cell
            z = maxs[2] + 1
            surfaces = []
            # A trace that starts inside a brush passes out of it, so stepping just
            # under each hit walks down through every surface in this column.
            for _ in range(6):
                start = (C.c_float * 3)(x, y, z)
                end[0], end[1], end[2] = x, y, mins[2] - 1
                if lib.gs_trace(start, end, None, None, hit) >= 1.0:
                    break
                surfaces.append(hit[2])
                z = hit[2] - 0.5
            if len(surfaces) >= 2:
                heights[(i, j)] = surfaces[1]   # [0] is the outside of the roof/sky
    return heights, w, h


def cmd_angle(deg, delta):
    return ((int(deg * 65536 / 360) & 65535) - delta) & 65535


def strafe_run(lib, frames):
    """The scripted strafe jumper from test_strafe.c; returns (x, y, speed) per frame."""
    env = lib.gs_env_new()
    lib.gs_env_spawn(env)
    s = State()
    lib.gs_env_get_state(env, C.byref(s))
    start_yaw = s.viewangles[1]
    path, jump_held = [], False

    def speed():
        return math.hypot(s.velocity[0], s.velocity[1])

    for f in range(frames):
        cmd = Cmd()
        cmd.forward = 127
        if f < 38:  # run up
            cmd.angles[1] = cmd_angle(start_yaw, s.delta_angles[1])
        else:
            side = 1 if ((f - 38) // 50) % 2 == 0 else -1
            vel_yaw = math.degrees(math.atan2(s.velocity[1], s.velocity[0]))
            best = 320 - 320 * 0.008
            theta = math.degrees(math.acos(best / speed())) if speed() > best else 0
            view = vel_yaw - theta + 45 if side > 0 else vel_yaw + theta - 45
            cmd.angles[1] = cmd_angle(view, s.delta_angles[1])
            cmd.right = 127 if side > 0 else -127
            if s.ground_entity >= 0 and not jump_held:
                cmd.up, jump_held = 127, True
            elif s.ground_entity < 0:
                jump_held = False
        lib.gs_env_step(env, C.byref(cmd))
        lib.gs_env_get_state(env, C.byref(s))
        path.append((s.origin[0], s.origin[1], speed()))
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


def zones(lib):
    """Trigger boxes, coloured by what they (or their targets) do."""
    ents = []
    e = Entity()
    i = 0
    while lib.gs_entity(i, C.byref(e)) == 0:
        ents.append((e.classname.decode(), e.target.decode(), e.targetname.decode(),
                     tuple(e.absmin), tuple(e.absmax)))
        i += 1
    by_name = {}
    for cls, _, name, _, _ in ents:
        if name:
            by_name.setdefault(name, []).append(cls)
    out = []
    for cls, target, _, amin, amax in ents:
        if not cls.startswith("trigger_"):
            continue
        kinds = [cls] + by_name.get(target, []) if cls == "trigger_multiple" else [cls]
        for k in kinds:
            if k in ZONE_COLORS:
                out.append((ZONE_COLORS[k], amin, amax))
                break
    return out


def speed_color(v, vmax):
    t = max(0.0, min(1.0, (v - 250) / max(1, vmax - 250)))
    return (int(40 + 215 * t), int(120 + 80 * (1 - abs(2 * t - 1))), int(255 * (1 - t)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("map")
    ap.add_argument("-o", "--out", default=None, help="output PNG (default: <map>.png)")
    ap.add_argument("--q3", default=os.environ.get("GAUNTLET_Q3", str(Path.home() / "Games/quake3")))
    ap.add_argument("--cell", type=float, default=8, help="units per pixel (default 8)")
    ap.add_argument("--frames", type=int, default=400, help="frames of the bot run")
    args = ap.parse_args()

    lib = load_lib()
    if lib.gs_load_map(args.q3.encode(), b"defrag", args.map.encode()) < 0:
        raise SystemExit(lib.gs_last_error().decode())
    info = MapInfo()
    lib.gs_map_info(C.byref(info))

    heights, w, h = floor_heights(lib, info, args.cell)
    # The lowest surface everywhere outside the playable area is the bottom of the
    # sky box; leave it as background.
    zs = sorted(heights.values())
    if zs:
        void = zs[0]
        heights = {k: z for k, z in heights.items() if z > void + 1}
        zs = sorted(heights.values())
    lo, hi = (zs[0], zs[-1]) if zs else (0, 1)
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

    for color, amin, amax in zones(lib):
        x0, y1 = px(amin[0], amin[1])
        x1, y0 = px(amax[0], amax[1])
        d.rectangle([x0, y0, x1, y1], outline=color, width=2)

    path = strafe_run(lib, args.frames)
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
