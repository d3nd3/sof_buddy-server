#!/usr/bin/env bash
# Copy minigames sprites from assets/sb/ into export/sb/ and SOF Base/sb/.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ASSETS="$ROOT/src/features/minigames/assets/sb"
LOCAL_EXPORT="${MINIGAME_ASSETS_LOCAL:-$ROOT/export}"

install_minigame_assets() {
	local dest="$1"
	mkdir -p "$dest/sb"
	cp -rv "$ASSETS/." "$dest/sb/"
	echo "Installed minigame assets under: $dest/sb/"
}

default_sof_base() {
	local wprefix="${WINEPREFIX:-$HOME/wprefix/sof-server}"
	local win_user="${WINE_WINDOWS_USER:-$USER}"
	local profile="$wprefix/drive_c/users/$win_user"
	local cand base
	for cand in "Soldier of Fortune" "Soldier Of Fortune"; do
		base="$profile/$cand/Base"
		if [[ -d "$base" ]] && [[ -f "$base/oldgamex86.dll" && -f "$base/pak0.pak" ]]; then
			echo "$base"
			return 0
		fi
	done
	echo "$profile/Soldier of Fortune/Base"
}

if [[ ! -d "$ASSETS" ]]; then
	echo "error: missing $ASSETS" >&2
	exit 1
fi

install_minigame_assets "$LOCAL_EXPORT"

if [[ "${SKIP_SOF_SERVER:-}" == "1" ]]; then
	exit 0
fi

if [[ -n "${SOF_SERVER_BASE:-}" ]]; then
	DEST="$SOF_SERVER_BASE"
else
	DEST="$(default_sof_base)"
fi
install_minigame_assets "$DEST"
