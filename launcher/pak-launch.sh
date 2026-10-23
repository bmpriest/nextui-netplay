#!/bin/sh
#
# Netplay.pak entry point. Runs the session-setup app.


DIR="$(dirname "$0")"
cd "$DIR" || exit 1
NETPLAY_PAK="$(pwd)"
export NETPLAY_PAK

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"
: "${SYSTEM_PATH:=$SDCARD_PATH/.system/$PLATFORM}"
: "${LOGS_PATH:=$SDCARD_PATH/.userdata/$PLATFORM/logs}"

export SDCARD_PATH PLATFORM SYSTEM_PATH
export LD_LIBRARY_PATH="$DIR:$DIR/bin:$DIR/bin/$PLATFORM:$LD_LIBRARY_PATH"
export HOME="$SDCARD_PATH/.userdata/$PLATFORM"

mkdir -p "$LOGS_PATH"
LOG="$LOGS_PATH/netplay.txt"

# One fresh log per launch is the default, and stays the default: it is the
# ordinary convention, and a handheld's card should not accumulate diagnostics
# nobody asked for.
#
# It does lose data, and the loss is not hypothetical. Two field reports of
# "the device hung and I rebooted" arrived with logs that had already been
# overwritten by the launch that followed the reboot - in one case the surviving
# file was from a later, uneventful run, so the only record of the failure was
# gone before anyone thought to look. The run that matters is usually the one
# before the relaunch that erased it.
#
# So under verbose logging - which the user opts into, and which already retains
# a per-process log for each game launch - keep a small ring of previous runs
# instead. Bounded at NETPLAY_LOG_KEEP files of a few KB each, so "keep the
# evidence" cannot become "fill the card".
: "${NETPLAY_LOG_KEEP:=4}"

netplay_verbose_logs() {
	_s="$SDCARD_PATH/.userdata/shared/Netplay/settings"
	[ -f "$_s" ] || return 1
	grep -q '^verbose_logs=1$' "$_s" 2>/dev/null
}

netplay_rotate_log() {
	[ -f "$LOG" ] || return 0
	_n="$NETPLAY_LOG_KEEP"
	case "$_n" in ''|*[!0-9]*) _n=4 ;; esac
	[ "$_n" -gt 0 ] || return 0

	rm -f "$LOGS_PATH/netplay.$_n.txt"
	while [ "$_n" -gt 1 ]; do
		_p=$((_n - 1))
		[ -f "$LOGS_PATH/netplay.$_p.txt" ] &&
			mv "$LOGS_PATH/netplay.$_p.txt" "$LOGS_PATH/netplay.$_n.txt"
		_n=$_p
	done
	mv "$LOG" "$LOGS_PATH/netplay.1.txt"
}

if netplay_verbose_logs; then
	netplay_rotate_log
else
	# Not opted in: drop any ring left behind by a previous verbose session
	# rather than leaving it on the card indefinitely.
	_n=1
	while [ "$_n" -le 16 ]; do
		rm -f "$LOGS_PATH/netplay.$_n.txt"
		_n=$((_n + 1))
	done
fi

"$DIR/bin/$PLATFORM/netplay.elf" "$@" > "$LOG" 2>&1
