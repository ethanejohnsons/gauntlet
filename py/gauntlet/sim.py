"""ctypes binding for libgauntlet (sim/include/gauntlet.h).

    info = sim.load_map("vanilla_01")
    env = sim.Env()
    state = env.spawn()
    state = env.step(cmd)          # cmd is a sim.Cmd; one 8 ms frame

The ctypes structs below must match gauntlet.h field for field.
"""

import ctypes as C
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FRAME_MSEC = 8


def default_q3_path():
    return os.environ.get("GAUNTLET_Q3", str(Path.home() / "Games/quake3"))


BUTTON_ATTACK = 1
WP_MACHINEGUN, WP_ROCKET_LAUNCHER = 2, 5


class Cmd(C.Structure):
    _fields_ = [("angles", C.c_int32 * 3), ("forward", C.c_int8), ("right", C.c_int8),
                ("up", C.c_int8), ("buttons", C.c_uint8), ("weapon", C.c_uint8), ("unused", C.c_uint8 * 3)]


class State(C.Structure):
    _fields_ = [("origin", C.c_float * 3), ("velocity", C.c_float * 3),
                ("viewangles", C.c_float * 3), ("delta_angles", C.c_int32 * 3),
                ("command_time", C.c_int32), ("pm_flags", C.c_int32), ("pm_time", C.c_int32),
                ("ground_entity", C.c_int32), ("frame", C.c_int32), ("timer_state", C.c_int32),
                ("timer_ms", C.c_int32), ("checkpoints", C.c_int32), ("dead", C.c_int32),
                ("haste_ms", C.c_int32), ("flight_ms", C.c_int32), ("health", C.c_int32),
                ("armor", C.c_int32), ("weapon", C.c_int32), ("weapons", C.c_int32), ("rockets", C.c_int32),
                ("weapon_time", C.c_int32), ("rockets_in_flight", C.c_int32)]


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


Vec3 = C.c_float * 3

_lib = None


def lib():
    """Load libgauntlet once (from $GAUNTLET_LIB or the default CMake build folder)."""
    global _lib
    if _lib is None:
        path = os.environ.get("GAUNTLET_LIB", str(ROOT / "build/cmake/sim/libgauntlet.so"))
        L = C.CDLL(path)
        L.gs_load_map.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p]
        L.gs_last_error.restype = C.c_char_p
        L.gs_map_info.argtypes = [C.POINTER(MapInfo)]
        L.gs_num_entities.restype = C.c_int
        L.gs_entity.argtypes = [C.c_int, C.POINTER(Entity)]
        L.gs_env_new.restype = C.c_void_p
        L.gs_env_free.argtypes = [C.c_void_p]
        L.gs_env_spawn.argtypes = [C.c_void_p]
        L.gs_env_step.argtypes = [C.c_void_p, C.POINTER(Cmd)]
        L.gs_env_get_state.argtypes = [C.c_void_p, C.POINTER(State)]
        L.gs_env_place.argtypes = [C.c_void_p, Vec3, Vec3, C.c_float]
        L.gs_trace.argtypes = [Vec3, Vec3, C.c_void_p, C.c_void_p, Vec3]
        L.gs_trace.restype = C.c_float
        L.gs_route_build.argtypes = [C.c_float]
        L.gs_route_distance.argtypes = [C.c_int, Vec3]
        L.gs_route_distance.restype = C.c_float
        L.gs_route_ahead.argtypes = [C.c_int, Vec3, C.c_float, Vec3]
        L.gs_route_num_targets.restype = C.c_int
        L.gs_route_target_entity.argtypes = [C.c_int]
        L.gs_env_set_route_target.argtypes = [C.c_void_p, C.c_int]
        _lib = L
    return _lib


def load_map(name, q3_path=None, game="defrag"):
    L = lib()
    if L.gs_load_map((q3_path or default_q3_path()).encode(), game.encode(), name.encode()) < 0:
        raise RuntimeError(L.gs_last_error().decode())
    info = MapInfo()
    L.gs_map_info(C.byref(info))
    return info


def entities():
    """All map entities as dicts."""
    L = lib()
    out, e = [], Entity()
    for i in range(L.gs_num_entities()):
        L.gs_entity(i, C.byref(e))
        out.append({"classname": e.classname.decode(), "target": e.target.decode(),
                    "targetname": e.targetname.decode(), "origin": tuple(e.origin),
                    "absmin": tuple(e.absmin), "absmax": tuple(e.absmax),
                    "supported": bool(e.supported)})
    return out


def trace(start, end):
    """Point trace through the world: (fraction of the way that is clear, end point)."""
    hit = Vec3()
    frac = lib().gs_trace(Vec3(*start), Vec3(*end), None, None, hit)
    return frac, (hit[0], hit[1], hit[2])


ROUTE_START, ROUTE_FINISH, ROUTE_CHECKPOINT0 = 0, 1, 2   # route targets (then one per checkpoint)


def build_route(cell=0):
    """Build the loaded map's route (distances to the start zone and the finish).
    Takes a few seconds."""
    L = lib()
    if L.gs_route_build(cell) < 0:
        raise RuntimeError(L.gs_last_error().decode())


def route_distance(pos, target=ROUTE_FINISH):
    """Distance from pos to the target along the route, or -1 if not connected."""
    return lib().gs_route_distance(target, Vec3(*pos))


def route_ahead(pos, ahead, target=ROUTE_FINISH):
    """The point about `ahead` units further along the route to the target, or None."""
    out = Vec3()
    return tuple(out) if lib().gs_route_ahead(target, Vec3(*pos), ahead, out) == 0 else None


def zone_center(target):
    """The middle of the first trigger zone of a route target (start, finish or a checkpoint)."""
    ents = entities()
    if target == ROUTE_START or target == ROUTE_FINISH:
        kind = "target_startTimer" if target == ROUTE_START else "target_stopTimer"
        names = {e["targetname"] for e in ents if e["classname"] == kind}
    else:
        names = {ents[lib().gs_route_target_entity(target)]["targetname"]}
    for e in ents:
        if e["classname"] == "trigger_multiple" and e["target"] in names:
            return tuple((a + b) / 2 for a, b in zip(e["absmin"], e["absmax"]))
    return None


def route_legs(spawn):
    """The order of a run: the start zone, then the checkpoints (each time the nearest
    unvisited one along the route), then the finish. Returns [(target, length)], where
    length is the route distance from the previous zone (the spawn for the first)."""
    L = lib()
    legs = [(ROUTE_START, route_distance(spawn, ROUTE_START))]
    here = zone_center(ROUTE_START)
    remaining = list(range(ROUTE_CHECKPOINT0, L.gs_route_num_targets()))
    while remaining:
        dists = [(route_distance(here, k), k) for k in remaining]
        dists = [(d, k) for d, k in dists if d >= 0]
        if not dists:
            break   # the rest aren't reachable from here; skip them
        d, k = min(dists)
        legs.append((k, d))
        remaining.remove(k)
        here = zone_center(k)
    legs.append((ROUTE_FINISH, route_distance(here, ROUTE_FINISH)))
    return legs


class Env:
    """One simulated player on the loaded map. step() and spawn() return the
    player's State; the same object is reused, so copy values you want to keep."""

    def __init__(self):
        self._lib = lib()
        self._env = self._lib.gs_env_new()
        self.state = State()

    def __del__(self):
        if getattr(self, "_env", None):
            self._lib.gs_env_free(self._env)

    def spawn(self):
        self._lib.gs_env_spawn(self._env)
        return self.get_state()

    def step(self, cmd):
        self._lib.gs_env_step(self._env, C.byref(cmd))
        return self.get_state()

    def get_state(self):
        self._lib.gs_env_get_state(self._env, C.byref(self.state))
        return self.state


def cmd_angle(deg, delta):
    """Raw 16-bit cmd angle that makes the view look at `deg`, given delta_angles."""
    return ((int(deg * 65536 / 360) & 65535) - delta) & 65535
