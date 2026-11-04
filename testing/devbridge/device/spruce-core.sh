#!/bin/sh
#
# Pick spruce's RetroArch core for one game, and put spruce's choice back.
#
#   spruce-core.sh set EMU ROM_FILE CORE    e.g. set GBA "Mario Kart.zip" mgba
#   spruce-core.sh restore EMU
#
# spruce keeps a per-game core in Emu/EMU/config.json, under
# menuOptions.Emulator.overrides keyed by the ROM's file name. It also caches
# the core each game last ran with (Saves/spruce/last_core_run), and when that
# differs it stops the launch to ask whether to move the save over; nothing
# answers that in a test. So `set` records the new core there too. `set` saves
# both first (once, so a second set cannot save the modified copies) and
# `restore` puts them back.

EMU=$2
CFG=/mnt/SDCARD/Emu/$EMU/config.json
SAVED=/tmp/netplay-devbridge/spruce-core-$EMU
JQ=/mnt/SDCARD/spruce/bin64/jq

case $1 in
set)
	ROM=$3 CORE=$4
	CACHE="/mnt/SDCARD/Saves/spruce/last_core_run/$EMU/$ROM"
	[ -f "$CFG" ] || { echo "spruce-core: no $CFG" >&2; exit 1; }
	. /mnt/SDCARD/spruce/scripts/emu/lib/core_mappings.sh 2>/dev/null
	folder=$(get_core_folder "${CORE}_libretro.so" 2>/dev/null)
	[ -n "$folder" ] || { echo "spruce-core: spruce does not know core $CORE" >&2; exit 1; }
	if [ ! -d "$SAVED" ]; then
		mkdir -p "$SAVED" && cp "$CFG" "$SAVED/config.json" || exit 1
		printf '%s\n' "$ROM" > "$SAVED/rom"
		[ -f "$CACHE" ] && cp "$CACHE" "$SAVED/cache"
	fi
	"$JQ" --arg game "$ROM" --arg core "$CORE" '.menuOptions.Emulator.overrides[$game] = $core' \
		"$SAVED/config.json" > "$CFG.tmp" && mv "$CFG.tmp" "$CFG" || exit 1
	mkdir -p "$(dirname "$CACHE")" && printf '%s\n' "$folder" > "$CACHE" && sync || exit 1
	echo "spruce-core: $EMU/$ROM -> $CORE ($folder)"
	;;
restore)
	[ -d "$SAVED" ] || { echo "spruce-core: nothing saved for $EMU"; exit 0; }
	CACHE="/mnt/SDCARD/Saves/spruce/last_core_run/$EMU/$(cat "$SAVED/rom")"
	cp "$SAVED/config.json" "$CFG" || exit 1
	if [ -f "$SAVED/cache" ]; then cp "$SAVED/cache" "$CACHE"; else rm -f "$CACHE"; fi
	sync && rm -rf "$SAVED" || exit 1
	echo "spruce-core: $EMU restored"
	;;
*) echo "usage: spruce-core.sh set EMU ROM CORE | restore EMU" >&2; exit 2 ;;
esac
