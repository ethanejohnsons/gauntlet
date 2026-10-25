# Gauntlet

An AI that learns to play Quake III [DeFRaG](https://q3df.org), in the spirit of MarI/O.

Training runs against **libgauntlet**, a fast headless simulation built from Quake III's
GPL movement and collision code. The best runs are replayed in the real engine
([oDFe](https://github.com/Defrag-racing/oDFe)) with the real DeFRaG mod, both to check
that the simulation was exact and to render footage for streams and clips.

All runs are AI / tool-assisted. They are never submitted to record tables or played on
public servers.

## Layout

| Path | What |
|---|---|
| `sim/` | **libgauntlet** (C): Q3's movement and collision code run headless, at ~10M steps/s |
| `sim/q3/` | Vendored Quake III source, changed as little as possible ([PATCHES.md](sim/q3/PATCHES.md)) |
| `sim/src/` | Our code around it: map loading, game logic, the public API |
| `sim/tests/` | C tests and a benchmark (`ctest`) |
| `py/gauntlet/` | Python: ctypes binding for libgauntlet, `.run` files, scripted bots, the training environment |
| `train/` | Training the AI: [ppo.py](train/ppo.py), one script, meant to be read top to bottom |
| `tools/` | Command-line tools: export a run, replay it in oDFe, compare, render video, top-down map view |
| `engine/` | oDFe as a submodule and the build script (our engine patches are kept private, see [docs/setup.md](docs/setup.md)) |
| `docs/setup.md` | Building, running and watching runs |
| `viewer/`, `maps/custom/` | Not started: the stream viewer (M4), custom maps |

## Reading the code

A good order:

1. [sim/include/gauntlet.h](sim/include/gauntlet.h): the whole public API of the sim, about 100 lines.
2. [sim/src/sim_local.h](sim/src/sim_local.h): what each source file does and the shared data.
3. [sim/src/game.c](sim/src/game.c): what happens each frame (`G_ClientThink`), spawning, triggers.
4. [sim/src/world.c](sim/src/world.c): loading a map and its entities, and the world queries.
5. [sim/src/env.c](sim/src/env.c): the player API and the thread pool.
6. [py/gauntlet/](py/gauntlet/) and [tools/](tools/): driving the sim from Python and replaying in the real game.
7. [py/gauntlet/env.py](py/gauntlet/env.py) then [train/ppo.py](train/ppo.py): how the AI sees, acts, is rewarded and learns.

The vendored code in `sim/q3/` is id's and oDFe's, kept as close to the original as
possible; you don't need to read it to follow ours. The one movement file worth a
look is `sim/q3/game/bg_pmove.c`, which is Q3's actual player movement.

## Game data

This repo contains **no game content**. You need your own Quake III Arena (`baseq3/pak0-8.pk3`)
and DeFRaG. The default location used by scripts is `~/Games/quake3`
(see [docs/setup.md](docs/setup.md)).

## Credits

- Quake III Arena by id Software (engine and game code released under GPLv2)
- DeFRaG by cgg and the DeFRaG team
- oDFe by the Defrag-racing team, based on Quake3e by ec-
- Every map used belongs to its author and is credited wherever it appears

## License

GPLv2 (see [LICENSE](LICENSE)), as required by the Quake III source it builds on.
