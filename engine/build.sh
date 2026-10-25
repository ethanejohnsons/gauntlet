#!/usr/bin/env bash
# Build oDFe with gauntlet's patches, without touching the engine/oDFe submodule.
#
# The submodule's committed tree is copied to build/odfe-src, engine/patches/*.patch
# are applied there in order, and the result is built into build/odfe. The copy is
# only refreshed when the submodule commit or a patch changes, so rebuilds are
# incremental, and edits made in build/odfe-src survive until then.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUB="$ROOT/engine/oDFe"
SRC="$ROOT/build/odfe-src"
OUT="$ROOT/build/odfe"

stamp="$(git -C "$SUB" rev-parse HEAD; cat "$ROOT"/engine/patches/*.patch 2>/dev/null | sha256sum)"
if [[ ! -f "$SRC/.gauntlet-stamp" || "$(cat "$SRC/.gauntlet-stamp")" != "$stamp" ]]; then
	echo "Preparing patched oDFe source in $SRC"
	rm -rf "$SRC"
	mkdir -p "$SRC"
	git -C "$SUB" archive HEAD | tar -x -C "$SRC"
	# commit the clean tree, so `git -C build/odfe-src diff` shows exactly our patches
	git -C "$SRC" init -q
	git -C "$SRC" add -A
	git -C "$SRC" -c user.name=gauntlet -c user.email=gauntlet@localhost commit -qm "oDFe $(git -C "$SUB" rev-parse --short HEAD)"
	for p in "$ROOT"/engine/patches/*.patch; do
		[[ -e "$p" ]] || continue
		echo "  applying $(basename "$p")"
		patch -d "$SRC" -p1 --quiet --forward < "$p"
	done
	echo "$stamp" > "$SRC/.gauntlet-stamp"
fi

make -C "$SRC" -j"$(nproc)" release BUILD_DIR="$OUT" "$@"
