#!/bin/sh
#
# How long spruce takes to reopen its menu on a given game: what an
# invitation's Browse costs. Points PyUI's saved position at the game, ends
# the menu, and times spruce's loop starting a new one.
#
#   menu-timing.sh LABEL ROM_PATH
#     LABEL      the system's label (Emu/<SYS>/config.json .label: GB, NES, Genesis ...)
#     ROM_PATH   the game, e.g. "/mnt/SDCARD/Roms/GB/Pokemon - Blue Version (USA, Europe) (SGB Enhanced).zip"
#
# Prints when the new MainUI process appeared; a screenshot shows when it
# drew (about 4.6 s more on the RG35XXH). The previous position is kept in
# /tmp/pyui-state.json.bak; copy it back over Saves/pyui-state.json while the
# menu is closed (in a game) to undo.

F=/mnt/SDCARD/Saves/pyui-state.json
JQ=/mnt/SDCARD/spruce/bin64/jq
[ $# -eq 2 ] || { sed -n '3,14p' "$0" >&2; exit 2; }
P=$(pidof MainUI | awk '{print $1}')
[ -n "$P" ] || { echo "spruce's menu is not running" >&2; exit 1; }
cp "$F" /tmp/pyui-state.json.bak
kill -STOP "$P"
"$JQ" --arg s "$1" --arg g "$2" \
	'.lastMainMenuSelection = "Game" | .lastSystemSelection = $s | .inGameSelectionScreen = true
	 | .[$s].lastGameSelection = $g | .[$s].subfolder = ""' "$F" > "$F.tmp" && mv "$F.tmp" "$F"
s=$(cut -d' ' -f1 /proc/uptime)
kill -9 "$P"
i=0
while [ $i -lt 100 ]; do
	N=$(pidof MainUI)
	[ -n "$N" ] && [ "$N" != "$P" ] && break
	sleep 0.1
	i=$((i + 1))
done
e=$(cut -d' ' -f1 /proc/uptime)
echo "old menu $P, new $N: new process after $(echo "$s $e" | awk '{printf "%.2f", $2 - $1}') s"
