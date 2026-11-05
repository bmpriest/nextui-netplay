#!/bin/sh
#
# A test at several input delays: the NextUI Brick hosting the spruce
# RG35XXH, both on the house network with a session already armed between
# them (the NextUI Brick hosting over WiFi, the RG35XXH joined).
#
#   delay-sweep.sh TEST DELAY...     e.g. delay-sweep.sh rcr 3 4 5 6
#
# Each delay is pinned in both session files (input_delay=N, no automatic
# delay) before the test; the next game launch reads it. Prints each run's
# check summary; the whole output is in $OUT/delay-N.log. The last delay
# stays pinned: re-arm to get the automatic one back.

. "$(dirname "$0")/lib.sh"

[ $# -ge 2 ] || { sed -n '3,12p' "$0" >&2; exit 2; }
test=$ROOT/testing/devbridge/${1%.test}.test; shift
for d in "$@"; do
	pin="sed -i -e 's/^input_delay=.*/input_delay=$d/' -e '/^input_delay_auto=/d'"
	on ap "sh $HELPERS/quit.sh minarch >/dev/null 2>&1; $pin /mnt/SDCARD/.userdata/shared/Netplay/session; grep ^input_delay /mnt/SDCARD/.userdata/shared/Netplay/session"
	on hl "sh $HELPERS/quit.sh retroarch >/dev/null 2>&1; $pin $SESSION_FILE; grep ^input_delay $SESSION_FILE"
	DEVBRIDGE=$DEVBRIDGE timeout 1200 "$ROOT/testing/devbridge/run.sh" --user spruce \
		"adb:$AP_ADB" "$H700_LAN" "$test" > "$OUT/delay-$d.log" 2>&1
	echo "delay $d rc=$?"
	grep -E "agreed|fps average|stalls:|waiting messages|FAIL|DESYNC|^== " "$OUT/delay-$d.log"
done
