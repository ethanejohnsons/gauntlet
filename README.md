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
| `sim/` | libgauntlet (C): vendored Q3 code in `sim/q3`, map loading, triggers, batch step API |
| `py/gauntlet/` | Python binding (cffi) and vector environment |
| `train/` | PPO training, observations, rewards, curriculum |
| `engine/oDFe/` | oDFe engine (submodule), plus our patches for input replay and telemetry |
| `viewer/` | Compositor: game footage + neural net + stats, streamed or recorded with ffmpeg |
| `tools/` | Sim-vs-engine validation, scheduler, clip rendering |
| `maps/custom/` | Our own training map sources |

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
