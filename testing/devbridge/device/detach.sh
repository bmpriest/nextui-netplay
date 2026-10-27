#!/bin/sh
#
# Run a command detached from this shell, output to a file.
#
#   detach.sh OUT COMMAND [ARG...]
#
# For steps that take this device's network away from the caller: joining an
# ad hoc network or reloading the WiFi driver ends the SSH session they were
# started from, and a command still attached to it dies with it. OUT gets the
# command's output and then "exit=N"; poll it from the next connection.

out=$1; shift
rm -f "$out"
if [ "${DETACHED:-}" != 1 ]; then
	export DETACHED=1
	if command -v setsid >/dev/null 2>&1; then
		setsid sh "$0" "$out" "$@" </dev/null >/dev/null 2>&1 &
	elif command -v start-stop-daemon >/dev/null 2>&1; then
		rm -f "/tmp/netplay-detach.$$.pid"
		start-stop-daemon -S -b -m -p "/tmp/netplay-detach.$$.pid" -x /bin/sh -- "$0" "$out" "$@"
	else
		nohup sh "$0" "$out" "$@" </dev/null >/dev/null 2>&1 &
	fi
	exit 0
fi
trap '' HUP
"$@" > "$out.part" 2>&1
echo "exit=$?" >> "$out.part"
mv "$out.part" "$out"
