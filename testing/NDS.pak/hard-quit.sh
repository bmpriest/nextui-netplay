#!/bin/sh

PID_FILE=/tmp/nextui-nds.pid
HARD_QUIT_FILE=/tmp/nextui-nds-hard-quit

if [ ! -r "$PID_FILE" ]; then
	echo "hard-quit: no active NDS process"
	exit 1
fi

pid=$(sed -n '1p' "$PID_FILE" 2>/dev/null)
case "$pid" in
	''|*[!0-9]*)
		echo "hard-quit: invalid pid file"
		exit 1
		;;
esac

if ! kill -0 "$pid" 2>/dev/null; then
	echo "hard-quit: process $pid is no longer running"
	exit 1
fi

cmdline=$(tr '\000' ' ' < "/proc/$pid/cmdline" 2>/dev/null)
case "$cmdline" in
	*drastic*) ;;
	*)
		echo "hard-quit: refusing stale pid $pid ($cmdline)"
		exit 1
		;;
esac

: > "$HARD_QUIT_FILE"
echo "hard-quit: requested stop for pid=$pid"

remaining=6
while kill -0 "$pid" 2>/dev/null && [ "$remaining" -gt 0 ]; do
	sleep 1
	remaining=$((remaining - 1))
done

if kill -0 "$pid" 2>/dev/null; then
	echo "hard-quit: graceful stop failed; sending SIGKILL"
	kill -KILL "$pid" 2>/dev/null
fi
