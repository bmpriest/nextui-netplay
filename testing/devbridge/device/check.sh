#!/bin/sh
#
# Judge the netplay game log this test produced, on this device.
#
#   check.sh MODE ROM_PATTERN [MIN_FPS [MAX_WAITS]]
#     MODE         shared | instanced | link | dsp
#     ROM_PATTERN  part of the log directory name, e.g. Streets_of_Rage_2
#     MIN_FPS      fail if the session average falls below this
#     MAX_WAITS    fail if "Waiting for other player" came up more often
#
# Only logs newer than the marker run.sh touches before each test count, so a
# stale log from an earlier run can never pass for this one. Prints a summary;
# exits non-zero on anything that means the two devices did not play together.

MODE=$1 ROM=$2 MIN_FPS=${3:-0} MAX_WAITS=${4:-}
MARK=/tmp/netplay-devbridge/started

fail() { echo "FAIL: $*"; exit 1; }

log=$(ls -t $(find /mnt/SDCARD/.userdata/*/logs/netplay-games /mnt/SDCARD/Saves/netplay/logs/netplay-games -name '*.log' -newer "$MARK" 2>/dev/null) 2>/dev/null |
	grep -F "$ROM" | head -1)
[ -n "$log" ] || fail "no netplay log for $ROM since this test started"
echo "log: $log"

# Correctness first: any of these means the consoles parted ways.
bad=$(grep -iE 'disagree|diverg|desync|synchronization timed out|could not adopt' "$log" | head -3)
[ -z "$bad" ] || fail "$bad"

case $MODE in
shared)
	grep -q 'ROM and core identity match peer' "$log" || fail "peers never agreed on ROM and core"
	echo "checkpoints agreed: $(grep -c 'consoles agree\|checkpoint promoted\|in sync at frame' "$log")"
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
dsp)
	# DSperate local wireless: both radios in the session (mask 0003) at some
	# point, and the game never gave up. The archive is written when the
	# game exits, so this runs after QUIT_GAME.
	grep -q 'radios 0003' "$log" || fail "the two consoles' radios were never on together"
	grep -q '^lan: .* not answering yet' "$log" && echo "guest waited for its host"
	_timeouts=$(grep 'lan pacing:' "$log" | sed -n 's/.*, \([0-9]*\) timeouts.*/\1/p')
	echo "MP timeouts: $(echo "$_timeouts" | awk '{s+=$1} END {print s+0}'), worst window $(echo "$_timeouts" | sort -n | tail -n 1)"
	# A healthy race peaks near 40 timeouts in a 10 s window. An outage of a
	# few seconds shows as hundreds in one window, and Mario Kart answers it
	# with "Communication error": both consoles then race alone, at full
	# speed, and every other check here still passes.
	[ "$(echo "$_timeouts" | sort -n | tail -n 1)" -le "${DSP_MAX_WINDOW_TIMEOUTS:-100}" ] 2>/dev/null ||
		fail "a 10 s window had more than ${DSP_MAX_WINDOW_TIMEOUTS:-100} MP timeouts (the link went dead)"
	pat='lan pacing:' ;;
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

# Lockstep never drops input: a late packet holds the game instead, so the
# network shows up as stalled frames. "stalled 13 frames (2%), longest 6" or
# "input stalls 12 (1%), longest 4" per window; the slowest window shows how
# bad the worst stretch felt.
grep "$pat" "$log" | sed -n 's/.* \([0-9]*\) \(paired \)\{0,1\}frames in \([0-9]*\)ms.*\(stalled\|input stalls\) \([0-9]*\).*longest \([0-9]*\).*/\1 \3 \5 \6/p' |
	awk '
		{ f += $1; s += $3; if ($4 > l) l = $4; w = $1 * 1000 / $2; if (!n++ || w < low) low = w }
		END { if (n) printf "stalls: %d of %d frames (%.1f%%), longest %d frames, slowest window %.1f fps\n", s, f, s * 100 / f, l, low }'
# How many times the player saw "Waiting for other player": waits are input
# stalls long enough to put it up (STALL_OVERLAY_FRAMES), pauses are the other
# device's menu or a frontend hold over 250 ms ("(menu open)"). Each counted
# once. A low stall share can hide them, so they are their own numbers. The
# session total includes the last partial window; the per-window counts are
# the fallback for a log cut short.
if grep "$pat" "$log" | grep -q ', waits [0-9]'; then
	totals=$(grep 'session totals:' "$log" | tail -1)
	count() { # <field>
		if [ -n "$totals" ]; then
			echo "$totals" | sed -n "s/.* $1 \([0-9]*\).*/\1/p"
		else
			grep "$pat" "$log" | sed -n "s/.*, $1 \([0-9]*\),.*/\1/p" | awk '{ s += $1 } END { print s + 0 }'
		fi
	}
	waits=$(count waits) pauses=$(count pauses)
	echo "waiting messages: $waits (input), ${pauses:-0} (other player paused)"
	[ -n "$MAX_WAITS" ] && [ "$waits" -gt "$MAX_WAITS" ] &&
		fail "\"Waiting for other player\" $waits times, more than $MAX_WAITS"
fi
hiccups=$(grep -c 'frontend stalled - told peer' "$log")
[ "$hiccups" -gt 0 ] && echo "frontend stalls reported to peer: $hiccups"
echo "PASS"
