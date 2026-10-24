# Setup

## Build oDFe

Dependencies on Arch: `base-devel`, `git`, `sdl2` (installs `sdl2-compat`), `curl`, `libx11`.
Vulkan headers, libjpeg, ogg and vorbis are bundled with oDFe, so nothing else is needed.

```sh
git submodule update --init
make -C engine/oDFe -j16 release BUILD_DIR=$PWD/build/odfe
```

The build is out-of-tree, so the submodule stays clean. Output in `build/odfe/release-linux-x86_64/`:

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
