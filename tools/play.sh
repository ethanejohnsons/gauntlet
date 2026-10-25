#!/usr/bin/env bash
# Start the patched oDFe on a map, in a window, to play or to watch a replay:
#
#   tools/play.sh vanilla_01
#   python3 tools/export_run.py vanilla_01      # in another terminal
#   then in the game console (~):  /gs_replay runs/vanilla_01_strafe.run
#
# Uses your normal oDFe home (~/.q3a), where export_run.py puts runs by default.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE="$ROOT/build/odfe/release-linux-x86_64/oDFe.x64"
[[ -x "$ENGINE" ]] || "$ROOT/engine/build.sh"

exec "$ENGINE" +set fs_basepath "${GAUNTLET_Q3:-$HOME/Games/quake3}" +set fs_homepath "$HOME/.q3a" \
	+set fs_game defrag +set com_maxfps 125 +map "${1:-vanilla_01}"
