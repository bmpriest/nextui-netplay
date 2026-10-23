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
	sh "$0" "$@" </dev/null >/dev/null 2>&1 &
	exit 0
fi

IN=$(ls /mnt/SDCARD/Tools/*/DevBridge.pak/bin/input 2>/dev/null | head -1)
[ -x "$IN" ] || { echo "gameplay.sh: DevBridge input helper not found" >&2; exit 1; }

end=$(( $(date +%s) + $1 )); shift
n=$#
while [ "$(date +%s)" -lt "$end" ]; do
	r=$(hexdump -n2 -e '1/2 "%u"' /dev/urandom)
	i=$(( r % n + 1 )); eval chord=\${$i}
	for b in $(echo "$chord" | tr + ' '); do "$IN" press "$b" >/dev/null; done
	sleep 0.$(( (r / n) % 9 + 1 ))
	for b in $(echo "$chord" | tr + ' '); do "$IN" release "$b" >/dev/null; done
	sleep 0.$(( (r / 7) % 4 ))
done
"$IN" release-all >/dev/null
