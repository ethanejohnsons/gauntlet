# Gauntlet

An AI that teaches itself to play Quake III [DeFRaG](https://q3df.org), MarI/O style.

It doesn't learn inside the game. The movement and collision code Quake III shipped
under the GPL is pulled out into a small headless C library (libgauntlet), so
thousands of copies of the AI can race a map at once, far faster than real time.
When a run looks good, its inputs get played back in the real engine
([oDFe](https://github.com/Defrag-racing/oDFe)) with the real DeFRaG mod. That's
where the footage comes from, and it doubles as a check: the engine's run has to
match the simulation frame for frame, or the sim has a bug.

So far it finishes vanilla_09 (best 12.680 s) and has learned to rocket jump on
tatmt-s2, which it hasn't finished yet.

Every run here is AI / tool-assisted. None of them are submitted to record tables
or played on public servers.

## How it fits together

- **`sim/`** is libgauntlet. `sim/q3/` is the vendored Quake III code, left as
  close to the original as possible ([PATCHES.md](sim/q3/PATCHES.md) lists every
  change). `sim/src/` is the code around it: loading maps, triggers and timers,
  rockets, the route used for rewards, and the API in
  [gauntlet.h](sim/include/gauntlet.h). `ctest` runs the tests.
- **`py/gauntlet/`** drives the sim from Python: run files, a few scripted bots,
  and the training environment ([env.py](py/gauntlet/env.py)), which is where the
  rewards live.
- **`train/ppo.py`** trains the network with PPO. It's one file, best read top to
  bottom.
- **`tools/`** has the rest: playing a trained network, replaying runs in oDFe and
  comparing them with the sim, rendering video with the network overlay, and a
  top-down map view.
- **`engine/`** holds oDFe as a submodule plus its build script. The engine patches
  that replay inputs aren't public, see [docs/setup.md](docs/setup.md).
- `viewer/` and `maps/custom/` are placeholders for a stream viewer and custom
  training maps.

[docs/setup.md](docs/setup.md) covers building everything, training, and watching a
run in the game.

## Game data

There's **no game content** in this repo. You need your own copy of Quake III Arena
(`baseq3/pak0-8.pk3`) and DeFRaG. The scripts look in `~/Games/quake3` by default
(see [docs/setup.md](docs/setup.md)).

## Credits

- Quake III Arena by id Software (engine and game code released under GPLv2)
- DeFRaG by cgg and the DeFRaG team
- oDFe by the Defrag-racing team, based on Quake3e by ec-
- Every map belongs to its author and is credited wherever it appears

## License

GPLv2 (see [LICENSE](LICENSE)), as required by the Quake III source it builds on.
