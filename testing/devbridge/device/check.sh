#!/bin/sh
#
# Judge the netplay game log this test produced, on this device.
#
#   check.sh MODE ROM_PATTERN [MIN_FPS]
#     MODE         shared | instanced | link
#     ROM_PATTERN  part of the log directory name, e.g. Streets_of_Rage_2
#     MIN_FPS      fail if the session average falls below this
#
# Only logs newer than the marker run.sh touches before each test count, so a
# stale log from an earlier run can never pass for this one. Prints a summary;
# exits non-zero on anything that means the two devices did not play together.

MODE=$1 ROM=$2 MIN_FPS=${3:-0}
MARK=/tmp/netplay-devbridge/started

fail() { echo "FAIL: $*"; exit 1; }

log=$(ls -t $(find /mnt/SDCARD/.userdata/*/logs/netplay-games -name '*.log' -newer "$MARK" 2>/dev/null) 2>/dev/null |
	grep -F "$ROM" | head -1)
[ -n "$log" ] || fail "no netplay log for $ROM since this test started"
echo "log: $log"

# Correctness first: any of these means the consoles parted ways.
bad=$(grep -iE 'disagree|diverg|desync|synchronization timed out|could not adopt' "$log" | head -3)
[ -z "$bad" ] || fail "$bad"

case $MODE in
shared)
	grep -q 'ROM and core identity match peer' "$log" || fail "peers never agreed on ROM and core"
	echo "checkpoints agreed: $(grep -c 'consoles agree\|checkpoint promoted' "$log")"
	pat='netplay-shim\] pacing:' ;;
instanced)
	grep -q 'instanced link agreed by both devices' "$log" || fail "instanced link never agreed"
	echo "paired checkpoints agreed: $(grep -c 'paired consoles agree' "$log")"
	grep 'GBLC serial' "$log" | tail -1 | sed 's/^.*GBLC serial/link cable:/'
	pat='instanced pacing:' ;;
link)
	grep -q 'link pacing:' "$log" || fail "no link traffic logged"
	dropped=$(grep 'link pacing:' "$log" | sed -n 's/.*, \([0-9]*\) dropped.*/\1/p' | awk '{s+=$1} END {print s+0}')
	packets=$(grep 'link pacing:' "$log" | sed -n 's/.* \([0-9]*\) packets.*/\1/p' | awk '{s+=$1} END {print s+0}')
	echo "link packets: $packets, dropped: $dropped"
	[ "$packets" -gt 0 ] || fail "link carried no packets"
	[ "$dropped" -eq 0 ] || fail "$dropped link packets dropped"
	pat='link pacing:' ;;
*)
	fail "unknown mode $MODE" ;;
esac

# "600 frames in 10139ms" -> total frames and time across every window.
grep "$pat" "$log" | sed -n 's/.* \([0-9]*\) \(paired \)\{0,1\}frames in \([0-9]*\)ms.*/\1 \3/p' |
	awk -v min="$MIN_FPS" '
		{ f += $1; ms += $2; n++ }
		END {
			if (!n) { print "FAIL: no pacing windows"; exit 1 }
			fps = f * 1000 / ms
			printf "pacing: %d windows, %.1f fps average\n", n, fps
			if (fps < min) { printf "FAIL: below %s fps\n", min; exit 1 }
		}' || exit 1
echo "PASS"
