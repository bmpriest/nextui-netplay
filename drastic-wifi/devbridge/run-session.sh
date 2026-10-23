#!/bin/sh
# Run one DraStic Wi-Fi devbridge macro end to end and collect its evidence.
#
# Rebuilt after the original measure.sh was lost. It encodes the procedural
# rules this document's log records the hard way:
#
#   - fail fast unless both consoles are at DevBridge, because OPEN_GAME is
#     silently skipped when a game is already up and the macro then drives
#     whatever session it finds;
#   - recreate /tmp/drastic-wifi-session, which lives in tmpfs and does not
#     survive a reboot;
#   - clear stale traces before the run, and wait for /tmp to drain afterwards,
#     because the launcher moves the trace out of /tmp only once DraStic exits
#     and pulling early returns the *previous* run's file.
#
# Usage: run-session.sh <macro.test> [label]

set -e

HOST_SERIAL=1c000c66050607724dd
CLIENT_SERIAL=2c000c81b5c4078245d
HOST_ADHOC=10.0.0.1
CLIENT_ADHOC=10.0.0.20
PORT=55437

REPO=/home/benja/dev/sbcs/my-paks/.worktrees/netplay-drastic
DEVBRIDGE="python3 /home/benja/dev/sbcs/my-paks/DevBridge.pak/tools/devbridge.py"
# The .test files carry their own CONNECT_ADB lines. Passing --adb to `run` as
# well aborts the macro on its first line with "alias already connected", so the
# flags are used only for the ad-hoc `exec` calls that have no macro to do it.
ADBFLAGS="--adb $HOST_SERIAL=host --adb $CLIENT_SERIAL=client"
LOGDIR=/mnt/SDCARD/.userdata/tg5040/logs

MACRO="$1"
LABEL="${2:-$(basename "$MACRO" .test)}"
[ -n "$MACRO" ] || { echo "usage: run-session.sh <macro.test> [label]" >&2; exit 2; }

cd "$REPO"

OUT="$REPO/devbridge-sessions/evidence-$LABEL-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
echo "evidence: $OUT"

# 1. Both consoles must be idle at DevBridge before anything else.
echo "== preflight =="
$DEVBRIDGE $ADBFLAGS exec all:STATUS 2>&1 | tee "$OUT/preflight.txt"
if [ "$(grep -c 'foreground=devbridge' "$OUT/preflight.txt")" -ne 2 ]; then
	echo "PREFLIGHT FAILED: both consoles must be at foreground=devbridge" >&2
	exit 1
fi

# 2. Session files: tmpfs, so gone after every reboot.
adb -s "$HOST_SERIAL" shell \
	"printf 'role=host\nport=$PORT\npeer=$CLIENT_ADHOC\n' > /tmp/drastic-wifi-session"
adb -s "$CLIENT_SERIAL" shell \
	"printf 'role=client\nport=$PORT\npeer=$HOST_ADHOC\n' > /tmp/drastic-wifi-session"

# 3. Move prior traces aside rather than merely clearing /tmp. A run that never
#    reaches the emulator leaves the log directory untouched, and pulling "the
#    newest trace" then returns a previous run's file and scores it as this
#    one's. After this, anything in NDS-wifi/ was produced by this run.
for s in "$HOST_SERIAL" "$CLIENT_SERIAL"; do
	adb -s "$s" shell "mkdir -p $LOGDIR/NDS-wifi/archive; \
		for f in $LOGDIR/NDS-wifi/*.txt; do [ -e \"\$f\" ] && \
			mv \"\$f\" $LOGDIR/NDS-wifi/archive/; done; \
		rm -f /tmp/NDS-wifi-*.txt; rm -f $LOGDIR/NDS.txt" >/dev/null 2>&1
done

# 4. The macro itself. Its own failure is the result, not an error here.
echo "== $MACRO =="
set +e
# `... | tee` reports tee's status, which is always 0: a macro that aborted on
# its first line was being recorded as a pass. Keep the real one.
$DEVBRIDGE run "$MACRO" >"$OUT/macro.txt" 2>&1
MACRO_STATUS=$?
cat "$OUT/macro.txt"
set -e
echo "macro exit: $MACRO_STATUS" | tee -a "$OUT/macro.txt"

# 5. Safe quit through DraStic's own menu. Never kill -9 during normal testing.
echo "== safe quit =="
set +e
$DEVBRIDGE run drastic-wifi/devbridge/asphalt-safe-quit.test \
	>"$OUT/safe-quit.txt" 2>&1
cat "$OUT/safe-quit.txt"
set -e

# 6. Wait for the launcher to move each trace out of /tmp. Pulling before this
#    races the move and silently returns the previous run's file.
echo "== waiting for /tmp to drain =="
for s in "$HOST_SERIAL" "$CLIENT_SERIAL"; do
	i=0
	while [ "$i" -lt 60 ]; do
		n=$(adb -s "$s" shell "ls /tmp/NDS-wifi-*.txt 2>/dev/null | wc -l" | tr -d '\r')
		[ "$n" = "0" ] && break
		i=$((i + 1))
		sleep 1
	done
	[ "$i" -lt 60 ] || echo "WARNING: $s still has a trace in /tmp after 60 s" >&2
done

# 7. Collect.
for pair in "host:$HOST_SERIAL" "client:$CLIENT_SERIAL"; do
	role=${pair%%:*}
	s=${pair#*:}
	newest=$(adb -s "$s" shell "ls -t $LOGDIR/NDS-wifi/*.txt 2>/dev/null | head -1" | tr -d '\r')
	if [ -n "$newest" ]; then
		adb -s "$s" pull "$newest" "$OUT/$role-trace.txt" >/dev/null 2>&1 \
			&& echo "$role trace: $(basename "$newest")"
	else
		# The launcher moves the trace out of /tmp only on a clean exit. A
		# console whose emulator hung and had to be signalled never runs that
		# step, and the trace — which is the evidence for why it hung — is
		# still sitting in tmpfs. Take it from there rather than losing it.
		stranded=$(adb -s "$s" shell "ls -t /tmp/NDS-wifi-*.txt 2>/dev/null | head -1" | tr -d '\r')
		if [ -n "$stranded" ]; then
			adb -s "$s" pull "$stranded" "$OUT/$role-trace.txt" >/dev/null 2>&1 \
				&& echo "$role trace: $(basename "$stranded") (recovered from /tmp; emulator did not exit cleanly)"
		else
			echo "WARNING: no trace for $role" >&2
		fi
	fi
	if ! adb -s "$s" pull "$LOGDIR/NDS.txt" "$OUT/$role-stderr.txt" \
			>/dev/null 2>&1; then
		echo "WARNING: no $role stderr log; the emulator did not launch" >&2
	fi
done

# 8. Summary. stderr carries the transport's stop counters.
echo
echo "== counters =="
for role in host client; do
	f="$OUT/$role-stderr.txt"
	[ -f "$f" ] || continue
	echo "-- $role --"
	grep -E 'mp queue|beacons tx|mp datagrams|peer wait timed out' "$f" | tail -20
done

echo
echo "evidence: $OUT"
exit $MACRO_STATUS
