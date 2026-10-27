#!/bin/sh
#
# Random gameplay input on this device: stand-in for a player.
#
#   gameplay.sh [--bg] SECONDS SPEC...
#     SPEC = a chord such as a, right+b. One is picked at random, held
#     0.1-0.9s, released, then a 0-0.3s gap. Repeat a SPEC to weight it.
#     Every button is released at the end.
#
# Randomness comes from hexdump: the Brick's BusyBox has no od and its sh has
# no $RANDOM.

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
[ -x "$IN" ] || { echo "gameplay.sh: DevBridge input helper not found" >&2; exit 1; }

end=$(( $(date +%s) + $1 )); shift
n=$#
next=0
while now=$(date +%s); [ "$now" -lt "$end" ]; do
	if [ "$now" -ge "$next" ]; then in_menu && break; next=$((now + 5)); fi
	r=$(hexdump -n2 -e '1/2 "%u"' /dev/urandom)
	i=$(( r % n + 1 )); eval chord=\${$i}
	for b in $(echo "$chord" | tr + ' '); do "$IN" press "$b" >/dev/null; done
	sleep 0.$(( (r / n) % 9 + 1 ))
	for b in $(echo "$chord" | tr + ' '); do "$IN" release "$b" >/dev/null; done
	sleep 0.$(( (r / 7) % 4 ))
done
"$IN" release-all >/dev/null
