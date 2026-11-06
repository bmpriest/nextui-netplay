#!/bin/sh
#
# Gather a run from both spruce devices and quit both games.
#
#   collect.sh TAG
#
# Into $OUT/TAG: brick.pcap and h700.pcap (each device's capture of the pair),
# brick.log and h700.log (each device's Netplay game log from this run, when a
# session was armed), and `end` in run.env: when play ended, from the test's
# after-play screenshot (`*-after-*`; aw2.test has none, so set it by hand
# there). Then the run's row of the comparison
# table (table.py).

. "$(dirname "$0")/lib.sh"

[ $# -eq 1 ] || { sed -n '3,11p' "$0" >&2; exit 2; }
DIR=$OUT/$1
[ -f "$DIR/run.env" ] || { echo "no run $DIR (pure-ra.sh or launch.sh starts one)" >&2; exit 1; }

LOGS='find /mnt/SDCARD/Saves/netplay/logs/netplay-games -name "*.log" -newer /tmp/racap/started 2>/dev/null | xargs -r ls -t | head -1'
if [ "${CAPTURE_MODE:-pcap}" != logs ]; then
	on h 'kill $(pidof tcpdump) 2>/dev/null; sleep 1; cat /tmp/racap/cap.pcap' > "$DIR/h700.pcap"
	on b 'kill $(pidof tcpdump) 2>/dev/null; sleep 1' >/dev/null
	adb -s "$BRICK_ADB" pull /tmp/racap/cap.pcap "$DIR/brick.pcap" >/dev/null
fi
# The launcher archives the game log once the game has quit.
on h "sh $HELPERS/quit.sh retroarch >/dev/null 2>&1; sleep 2; f=\$($LOGS); [ -n \"\$f\" ] && cat \"\$f\"" > "$DIR/h700.log"
on b "sh $HELPERS/quit.sh retroarch >/dev/null 2>&1; sleep 2; f=\$($LOGS); [ -n \"\$f\" ] && cat \"\$f\"" > "$DIR/brick.log"
for f in "$DIR"/*.log; do [ -s "$f" ] || rm -f "$f"; done

after=$(find "$ROOT/testing/results/devbridge" -name '*-after-*-host.png' -newer "$DIR/started" 2>/dev/null |
	xargs -r ls -t | head -1)
if [ -n "$after" ]; then
	meta "$DIR" end "$(stat -c %Y "$after")"
else
	echo "no after-play screenshot since this run started: set end=EPOCH in $DIR/run.env by hand" >&2
fi
ls -la "$DIR"
[ -n "$after" ] && python3 -I "$HERE/table.py" "$DIR"
