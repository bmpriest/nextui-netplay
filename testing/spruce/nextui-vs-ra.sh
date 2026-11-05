#!/bin/sh
#
# Netplay's RetroArch protocol on the NextUI Brick against stock RetroArch on
# the spruce RG35XXH, both on the house network.
#
#   nextui-vs-ra.sh brickhost|sprucehost SYS "NextUI rom path" "spruce rom path" [SECONDS]
#     e.g. nextui-vs-ra.sh brickhost FC "Nintendo Entertainment System (FC)/River City Ransom (USA).zip" \
#                                       "FC/River City Ransom (USA).zip" 60
#
# brickhost: the NextUI Brick arms as a RetroArch-protocol host and opens the
# game; RetroArch on the RG35XXH connects to it. sprucehost: RetroArch hosts
# and the Brick joins it. Both then play SECONDS (default 60) of random input,
# and the Brick's Netplay log (wrapping, sync, CRC and savestate lines) and
# RetroArch's own netplay log lines are printed. RetroArch's capture stays in
# /tmp/ratest/h700.pcap on the RG35XXH (testing/retroarch/radecode.py).
# The RG35XXH's wrapper stays up for the next run; device/ra-wrap.sh down
# takes it off. This ends the NextUI Brick's session, so the ad hoc rig
# (adhoc.sh) is down while it runs.

. "$(dirname "$0")/lib.sh"

[ $# -ge 4 ] || { sed -n '3,19p' "$0" >&2; exit 2; }
mode=$1 sys=$2 nrom=$3 srom=$4 secs=${5:-60}
G="right right a b left a b up down"

# DevBridge against the NextUI Brick (adb) and the RG35XXH on the house network.
dbl() {
	(cd "$DEVBRIDGE" && timeout 180 python3 -P tools/devbridge.py --user spruce --session-root "$OUT/shots" \
		--adb "$AP_ADB=n" --connect "$H700_LAN:h" "$@") 2>&1 | grep -v '^connected'
}

push_helpers ap; push_helpers hl
on ap "sh $HELPERS/quit.sh minarch >/dev/null; sh $HELPERS/ctl.sh end >/dev/null"
on hl "sh $HELPERS/quit.sh retroarch >/dev/null; sh $HELPERS/ra-wrap.sh down >/dev/null"

if [ "$mode" = brickhost ]; then
	on ap "sh $HELPERS/ctl.sh host wifi --protocol retroarch" | tail -1
	on hl "sh $HELPERS/ra-wrap.sh up --connect $AP_LAN --nick spruce-h700 -v --log-file /tmp/ratest/ra.log"
	on ap "ping -c 1 -W 1 $H700_LAN >/dev/null"
	dbl exec "n:OPEN_GAME" "$sys" "$nrom" | tail -1
	sleep 12
	dbl exec "h:OPEN_GAME" "$sys" "$srom" | tail -1
else
	on hl "sh $HELPERS/ra-wrap.sh up --host --nick spruce-h700 -v --log-file /tmp/ratest/ra.log"
	dbl exec "h:OPEN_GAME" "$sys" "$srom" | tail -1
	sleep 15
	on ap "sh $HELPERS/ctl.sh join $H700_LAN --protocol retroarch" | tail -1
	dbl exec "n:OPEN_GAME" "$sys" "$nrom" | tail -1
fi
sleep 20
on ap "sh $HELPERS/gameplay.sh --bg $secs $G >/dev/null 2>&1"
on hl "sh $HELPERS/gameplay.sh --bg $secs $G >/dev/null 2>&1"
sleep $((secs + 8))

echo "--- NextUI Brick"
on ap 'f=$(ls -t /mnt/SDCARD/.userdata/*/logs/netplay-games/*/*/*.log | head -1); echo $f
	grep -E "wrapping|synced|plays port|PAUSE|mismatch|CRC agrees" "$f" | tail -6
	echo "savestate requests: $(grep -c "asked for a savestate" "$f")"
	grep "ra: frame" "$f" | tail -4'
echo "--- spruce RetroArch"
on hl 'grep -i netplay /tmp/ratest/ra.log | grep -vE "UPnP|connectable" | tail -6'
