# Setup

## Build oDFe

Dependencies on Arch: `base-devel`, `git`, `sdl2` (installs `sdl2-compat`), `curl`, `libx11`.
Vulkan headers, libjpeg, ogg and vorbis are bundled with oDFe, so nothing else is needed.

```sh
git submodule update --init
engine/build.sh
```

The script copies the submodule to `build/odfe-src`, applies `engine/patches/` (the private
`gs_replay` input replay, see [engine/patches/README.md](../engine/patches/README.md)) and
builds there, so the submodule stays clean. Output in `build/odfe/release-linux-x86_64/`:

| File | What |
|---|---|
| `oDFe.x64` | Client |
| `oDFe.ded.x64` | Dedicated server |
| `oDFe_opengl_x86_64.so`, `oDFe_vulkan_x86_64.so` | Renderers |

## Game data

Not in this repo. Default location is `~/Games/quake3`:

```
~/Games/quake3/
  baseq3/pak0.pk3 ... pak8.pk3     # retail Quake III Arena + point release
  defrag/                          # DeFRaG 1.91.25, as unpacked
    zz-defrag_vm_191.pk3           # game logic (QVMs)
    zz-defrag_*.pk3                # media and fixes
    promode&vanilla.pk3            # vanilla_01..10, among others
    *.pk3                          # any extra maps (df_vq3x-1..5 etc.)
```

Only the `.pk3` files (and DeFRaG's cfgs) are used. The Windows `.exe`/`.dll` files that come
with the DeFRaG pack are not needed: oDFe is the engine.

## Smoke test

Runs the dedicated server for 10 seconds on `vanilla_01`, bound to loopback only, with no master
servers. `fs_homepath` points to a scratch dir so nothing is written into the game folder.

```sh
Q3HOME=/tmp/gauntlet-q3home; mkdir -p $Q3HOME
timeout 10 build/odfe/release-linux-x86_64/oDFe.ded.x64 \
  +set dedicated 1 +set net_enabled 1 +set net_ip 127.0.0.1 \
  +set sv_master1 "" +set sv_master2 "" +set sv_master3 "" \
  +set fs_basepath $HOME/Games/quake3 +set fs_homepath $Q3HOME +set fs_game defrag \
  +map vanilla_01
```

Expected output (exit code 124 from `timeout` is normal):

```
5068 files in 27 pk3 files
Opening IP socket: 127.0.0.1:27960
Server: vanilla_01
Loading vm file vm/qagame.qvm...
gamename: defrag
InitGame: ...\mapname\vanilla_01\...\defrag_vers\19125\...
```

The pk3 count depends on which extra maps are installed. Lines like `fs_game is write protected.`
are harmless: the `+set` values were already applied at startup.

Never use `dedicated 2` (public, sends heartbeats to master servers).

## Build and test libgauntlet (the sim)

Needs `cmake`, `ninja` and `zlib`. Build from the repo root:

```sh
cmake -S . -B build/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake
ctest --test-dir build/cmake      # map load, strafe, threads, items, route, observations
build/cmake/sim/bench             # steps per second across thread counts
```

`cmake --build build/cmake --target odfe` builds the engine too.

The tests read game data from `$GAUNTLET_Q3` (default `~/Games/quake3`). Any test takes a
map name as its argument, e.g. `build/cmake/sim/test_map vanilla_02`, which also lists the
map's entities the sim doesn't handle yet.

Debug and Release builds give bit-identical physics (the float flags in `sim/CMakeLists.txt`
make sure of it), so either is fine for development.

## CLion

Open the repo root. CLion picks up the top-level `CMakeLists.txt`. Run or debug the
`test_strafe` target, and set a breakpoint in `G_ClientThink` (`sim/src/game.c`) to step
through a player frame. CLion's own `cmake-build-*` folders and `.idea/` are git-ignored.

## Watch a sim run in the real game

Render a bot run to video. This runs off-screen, with no game window:

```sh
tools/render_run.sh df_vq3x-1 strafe      # -> build/video/df_vq3x-1_strafe.mp4
```

It simulates the run, replays it in the patched oDFe, compares every frame the engine
reports with the sim (`tools/compare_run.py`), then renders the recorded demo with
`video-pipe`. Engine files go to `build/q3home`, not `~/.q3a`.

Or watch it live in the game window:

```sh
python3 tools/export_run.py vanilla_01     # writes ~/.q3a/defrag/runs/vanilla_01_strafe.run
tools/play.sh vanilla_01
```

In the game, open the console (`~`) and type `/gs_replay runs/vanilla_01_strafe.run`.
Keyboard and mouse are ignored while it plays; `/gs_replay_stop` stops it. The run's
first second is standing still, which lines the engine player up with the sim.

## The route

`gs_route_build` computes every open place's distance to the finish (see the comment at
the top of `sim/src/route.c`). To see it:

```sh
python3 tools/topdown.py vanilla_02 --route -o build/topdown/vanilla_02_route.png
```

`build/cmake/sim/test_route <map> v` prints the grid statistics for a map.

## Training the AI

Training uses PyTorch in a virtualenv inside the repo (`.venv/`, git-ignored, about 5 GB
with CUDA). To set it up (pip's temporary files go to `build/` because `/tmp` may be small):

```sh
python3 -m venv .venv
mkdir -p build/pip-tmp && TMPDIR=$PWD/build/pip-tmp .venv/bin/pip install torch numpy pillow tensorboard
```

Train on a map, here for 30 minutes:

```sh
.venv/bin/python -u train/ppo.py vanilla_01 --minutes 30
```

Everything goes to `runs/<map>/<date-time>/` (git-ignored): TensorBoard logs, the network
(`agent.pt`) and the best runs so far, `best_progress.run` and `best_time.run`. To follow
training in a browser:

```sh
.venv/bin/tensorboard --logdir runs/
```

To record what the network was "thinking" during a run (inputs, hidden layers, choice
probabilities and value, per decision; the data the stream overlay will draw):

```sh
.venv/bin/python tools/think.py runs/<map>/<name>/best_progress.run runs/<map>/<name>/agent.pt
```

To look at a saved run, draw it on the map or render it in the real game:

```sh
python3 tools/topdown.py vanilla_01 --route --run runs/vanilla_01/<name>/best_progress.run -o build/topdown/ai.png
tools/render_run.sh runs/vanilla_01/<name>/best_progress.run
```
