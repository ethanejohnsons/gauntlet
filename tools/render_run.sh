#!/usr/bin/env bash
# Simulate a bot run, replay it in the patched oDFe (off-screen, no window), check
# the engine against the sim, and render the run to an MP4.
#
#   tools/render_run.sh <map> [bot] [frames]      e.g. tools/render_run.sh df_vq3x-1 strafe
#
# Output: build/video/<map>_<bot>.mp4. Engine files go to build/q3home (a separate
# fs_homepath), so your own ~/.q3a config isn't touched.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAP="${1:?usage: $0 <map> [bot] [frames]}"
BOT="${2:-strafe}"
FRAMES="${3:-400}"
Q3="${GAUNTLET_Q3:-$HOME/Games/quake3}"
HOME_Q3="$ROOT/build/q3home"
ENGINE="$ROOT/build/odfe/release-linux-x86_64/oDFe.x64"
NAME="${MAP}_${BOT}"
WIDTH="${WIDTH:-1280}"
HEIGHT="${HEIGHT:-720}"

[[ -x "$ENGINE" ]] || "$ROOT/engine/build.sh"
mkdir -p "$HOME_Q3/defrag/runs" "$ROOT/build/video"
rm -f "$HOME_Q3/defrag/gs_replay.log" "$HOME_Q3/defrag/demos/gs_$NAME.dm_68" "$HOME_Q3/defrag/videos/gs_$NAME.mp4"

python3 "$ROOT/tools/export_run.py" "$MAP" --bot "$BOT" --frames "$FRAMES" \
	-o "$HOME_Q3/defrag/runs/$NAME.run" | head -2

# frames of the replay at 125 fps, plus a margin; `wait` counts client frames
REPLAY_FRAMES=$(sed -n 's/^frames //p' "$HOME_Q3/defrag/runs/$NAME.run")
WAIT=$(( REPLAY_FRAMES + 60 ))

engine() {
	SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy timeout 300 "$ENGINE" \
		+set fs_basepath "$Q3" +set fs_homepath "$HOME_Q3" +set fs_game defrag \
		+set r_fullscreen 0 +set r_mode -1 +set r_customwidth "$WIDTH" +set r_customheight "$HEIGHT" \
		+set s_initsound 0 +set cl_allowDownload 0 +set com_maxfps 125 "$@" > "$ROOT/build/odfe-$NAME.log" 2>&1
}

echo "replaying in oDFe and recording a demo..."
engine +map "$MAP" +wait 375 +record "gs_$NAME" +wait 20 \
	+gs_replay "runs/$NAME.run" +wait "$WAIT" +stoprecord +wait 20 +quit
grep 'gs_replay:' "$ROOT/build/odfe-$NAME.log" | tail -1

python3 "$ROOT/tools/compare_run.py" "$HOME_Q3/defrag/runs/$NAME.run" "$HOME_Q3/defrag/gs_replay.log" || true

echo "rendering the demo..."
engine +set cl_aviFrameRate 60 +set nextdemo quit +demo "gs_$NAME" +wait 3 +video-pipe "gs_$NAME"
cp "$HOME_Q3/defrag/videos/gs_$NAME.mp4" "$ROOT/build/video/$NAME.mp4"
echo "video: build/video/$NAME.mp4"
