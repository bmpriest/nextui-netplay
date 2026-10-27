#!/bin/sh
#
# Press buttons on this device with exact timing.
#
# DevBridge's BTN sends press and release as two separate remote calls, so a
# hold is really "hold + one round trip". Over an ad hoc link reached through
# the host that is a second or more, which auto-repeats menus (Advance Wars 2
# moves two entries) or misses edge-triggered ones. Run on the device, a hold is
# what it says.
#
#   keys.sh [--bg] STEP...
#     STEP = BUTTONS:HOLD:GAP   BUTTONS joined with +, seconds (fractions ok)
#          | sleep:SECS
#     --bg   detach and return at once, so both devices can be driven together
#
#   keys.sh a+b+start+select:0.4:4 right:0.2:0.4 start:0.3:2

if [ "$1" = --bg ]; then
	shift
	# Out of the caller's session: a plain & dies with an adb shell when it
	# returns (over SSH it survives). The Brick's BusyBox has no setsid, but
	# start-stop-daemon -b detaches the same way.
	if command -v setsid >/dev/null 2>&1; then
		setsid sh "$0" "$@" </dev/null >/dev/null 2>&1 &
	elif command -v start-stop-daemon >/dev/null 2>&1; then
		# A pidfile left by an earlier call whose pid now belongs to another
		# shell would make it refuse to start, so start from none.
		rm -f "/tmp/netplay-bg.$$.pid"
		start-stop-daemon -S -b -m -p "/tmp/netplay-bg.$$.pid" -x /bin/sh -- "$0" "$@"
	else
		sh "$0" "$@" </dev/null >/dev/null 2>&1 &
	fi
	exit 0
fi

# A menu in front means the game is gone (crashed or quit): pressing on would
# walk the device through NextUI or toggle DevBridge's own settings.
in_menu() {
	# Brackets keep grep from finding its own command line.
	grep -qs -e 'nextui[.]elf' -e 'DevBridge[.]pak/launch[.]sh' /proc/[0-9]*/cmdline
}

IN=$(ls /mnt/SDCARD/Tools/*/DevBridge.pak/bin/input 2>/dev/null | head -1)
[ -x "$IN" ] || { echo "keys.sh: DevBridge input helper not found" >&2; exit 1; }

for step in "$@"; do
	in_menu && { echo "keys.sh: a menu is in front, not pressing" >&2; exit 1; }
	case $step in
	sleep:*) sleep "${step#sleep:}"; continue ;;
	esac
	buttons=${step%%:*}; rest=${step#*:}; hold=${rest%%:*}; gap=${rest#*:}
	for b in $(echo "$buttons" | tr + ' '); do "$IN" press "$b" >/dev/null || exit 1; done
	sleep "$hold"
	for b in $(echo "$buttons" | tr + ' '); do "$IN" release "$b" >/dev/null; done
	sleep "$gap"
done
