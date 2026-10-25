# Vendored Quake III code

All files here are GPLv2 Quake III source, copied in and changed as little as possible.
Every change is marked in the source with a `gauntlet:` comment, a `GAUNTLET_SIM` block, or
one of the macros defined there (`GS_TLS`, `CM_CHECKCOUNT`, `BRUSH_STAMP`, `PATCH_STAMP`,
`GS_EnsureStamps`), which expand to the original code when `GAUNTLET_SIM` is not defined.

## Sources

| Files | From | Why that source |
|---|---|---|
| `qcommon/cm_*.c/h`, `q_math.c`, `q_shared.c/h`, `q_platform.h`, `qfiles.h`, `surfaceflags.h`, `qcommon.h` | oDFe `engine/oDFe/code/qcommon` (submodule commit `5c573f4e`) | Collision runs in the engine, so it must be the engine's exact code |
| `game/bg_public.h` | oDFe `engine/oDFe/code/game` | Matches the engine's structs |
| `game/bg_pmove.c`, `bg_slidemove.c`, `bg_misc.c`, `bg_local.h` | id Software Quake-III-Arena 1.32 GPL release (`code/game`) | DeFRaG's VQ3 movement is built from the 1.32 game code (DeFRaG itself is closed source) |
| `game/q_math_vm.c` | Functions copied from id's 1.32 `code/game/q_math.c` | The QVM uses the game-side math, which differs from oDFe's (e.g. `VectorNormalize`) |

## Changes

1. **Thread-safe collision** (`cm_local.h`, `cm_trace.c`, `cm_test.c`, `cm_load.c`).
   Traces used to write `checkcount` into the shared brush and patch structs to avoid
   testing the same brush twice. With `GAUNTLET_SIM` they use per-thread stamp arrays
   (`gs_brushStamp` / `gs_patchStamp`, see `src/q3_shim.c`), so the map data is read-only
   while stepping. The trace statistics counters are thread-local. Results are the same:
   it is the same "already visited" test, just stored elsewhere.
2. **Thread-safe pmove** (`bg_local.h`, `bg_pmove.c`). `pm`, `pml` and `c_pmove` are
   `_Thread_local`.
3. **`AngleVectors` without statics** (`q_math_vm.c`). The `static` locals were there to
   dodge an old MSVC bug. They are always written before being read, so dropping
   `static` changes nothing except making it thread-safe.

## Compiling like the QVM

The game-side files (`game/*.c` and `src/game.c`) are built with
`-fsingle-precision-constant -include game/bg_vmcompat.h`. That header makes
sin/cos/atan2/sqrt return float like the QVM syscalls, and renames the game-side math
helpers to `BGM_*` so they don't clash with the engine's versions. The reasons:

- In the QVM, `double` is 32-bit, so constants like `0.5` or `M_PI` are single precision.
- Math functions are engine syscalls: float in, double libm call, float out.

What still needs checking against the real engine (M2): DeFRaG's own changes to the 1.32
movement, timer start/stop semantics, `KEEP_SPEED` teleporters, and constant folding in
q3lcc. Two folded constants (`M_PI*2/360`, `65536/360.0`) were checked by hand and round
the same way.
