#!/usr/bin/env bash
# Replay a run in the patched oDFe (off-screen, no window), check the engine against
# the sim, and render the run to an MP4. The run is either a scripted bot's, made on
# the spot, or a saved .run file (for example one the AI made in training):
#
#   tools/render_run.sh <map> [bot] [frames]      e.g. tools/render_run.sh df_vq3x-1 strafe
#   tools/render_run.sh <file.run> [name]         e.g. tools/render_run.sh runs/vanilla_01/first/best_time.run
#
# Output: build/video/<name>.mp4. Engine files go to build/q3home (a separate
# fs_homepath), so your own ~/.q3a config isn't touched.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARG="${1:?usage: $0 <map> [bot] [frames]  or  $0 <file.run> [name]}"
Q3="${GAUNTLET_Q3:-$HOME/Games/quake3}"
HOME_Q3="$ROOT/build/q3home"
ENGINE="$ROOT/build/odfe/release-linux-x86_64/oDFe.x64"
WIDTH="${WIDTH:-1280}"
HEIGHT="${HEIGHT:-720}"

[[ -x "$ENGINE" ]] || "$ROOT/engine/build.sh"
mkdir -p "$HOME_Q3/defrag/runs" "$ROOT/build/video"

if [[ "$ARG" == *.run ]]; then
	MAP=$(sed -n 's/^map //p' "$ARG")
	NAME="${2:-${MAP}_$(basename "$(dirname "$ARG")")_$(basename "$ARG" .run)}"
	cp "$ARG" "$HOME_Q3/defrag/runs/$NAME.run"
else
	MAP="$ARG"
	NAME="${MAP}_${2:-strafe}"
	python3 "$ROOT/tools/export_run.py" "$MAP" --bot "${2:-strafe}" --frames "${3:-400}" \
		-o "$HOME_Q3/defrag/runs/$NAME.run" | head -2
fi
rm -f "$HOME_Q3/defrag/gs_replay.log" "$HOME_Q3/defrag/gs_video.log" "$HOME_Q3/defrag/demos/gs_$NAME.dm_68" "$HOME_Q3/defrag/videos/gs_$NAME.mp4"

# frames of the replay at 125 fps, plus a margin; `wait` counts client frames
REPLAY_FRAMES=$(sed -n 's/^frames //p' "$HOME_Q3/defrag/runs/$NAME.run")
WAIT=$(( REPLAY_FRAMES + 60 ))

engine() {
	SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy timeout 300 "$ENGINE" \
		+set fs_basepath "$Q3" +set fs_homepath "$HOME_Q3" +set fs_game defrag \
		+set r_fullscreen 0 +set r_mode -1 +set r_customwidth "$WIDTH" +set r_customheight "$HEIGHT" \
		+set s_initsound 0 +set cl_allowDownload 0 +set com_maxfps 125 \
		+set net_enabled 1 +set net_ip 127.0.0.1 "$@" > "$ROOT/build/odfe-$NAME.log" 2>&1
}

echo "replaying in oDFe and recording a demo..."
engine +map "$MAP" +wait 375 +record "gs_$NAME" +wait 20 \
	+gs_replay "runs/$NAME.run" +wait "$WAIT" +stoprecord +wait 20 +quit
grep 'gs_replay:' "$ROOT/build/odfe-$NAME.log" | tail -1

python3 "$ROOT/tools/compare_run.py" "$HOME_Q3/defrag/runs/$NAME.run" "$HOME_Q3/defrag/gs_replay.log" || true

echo "rendering the demo..."
engine +set cl_aviFrameRate 60 +set nextdemo quit +demo "gs_$NAME" +wait 3 +video-pipe "gs_$NAME"
cp "$HOME_Q3/defrag/videos/gs_$NAME.mp4" "$ROOT/build/video/$NAME.mp4"
# timing logs, for drawing overlays (tools/overlay.py)
cp "$HOME_Q3/defrag/gs_video.log" "$ROOT/build/video/$NAME.video.log"
cp "$HOME_Q3/defrag/gs_replay.log" "$ROOT/build/video/$NAME.replay.log"
echo "video: build/video/$NAME.mp4"
