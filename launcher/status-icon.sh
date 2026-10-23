#!/bin/sh
# Netplay status icon, drawn by Varnish.pak's overlay.
#
#   status-icon.sh sync    start or stop the watcher to match the setting and session
#   status-icon.sh stop    stop the watcher and hide the icon
#   status-icon.sh watch   (internal) the watcher loop
#
# The setting (status_icon=) is 0 no icon, 1 only while a session is armed,
# 2 always - dimmed with no session, so Netplay's presence shows at a glance.
#
# The icon comes from res/status@<scale>x.png, a NextUI-style asset sheet: one
# pixel-exact sheet per device scale, each state its own 14x14 (1x units) icon,
# recoloured by Varnish with the theme's icon colour exactly as NextUI recolours
# its own status icons (tools/make-status-icons.py builds them). The N is a
# plug and a socket: dim when idle, the left half lit while hosting, both once
# a peer is connected.
#
# Optional by design: without Varnish (or on a platform it does not support)
# every send fails quietly and Netplay behaves exactly as before.

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"

NP="${NETPLAY_PAK:-$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak}"
. "$NP/launcher/state-path.sh"
STATE="$NETPLAY_STATE"
SETTINGS="$STATE/settings"
SESSION="$STATE/session"
PIDFILE="$STATE/status-icon.pid"
VARNISH="$SDCARD_PATH/Tools/$PLATFORM/Varnish.pak/varnish"
VARNISH_PID="${VARNISH_PID:-/tmp/varnish.pid}"
VARNISH_FIFO="${VARNISH_FIFO:-/tmp/varnish.fifo}"
CLIENT=netplay
POSITION=bottom-center
SHEET="$NP/res/status@%dx.png"

# Varnish drops overlays sent during its start-up warm-up and forgets them all if
# it restarts, so an unchanged icon is re-sent this often (seconds).
RESEND=30

mode() {
	_mode=$(sed -n 's/^status_icon=//p' "$SETTINGS" 2>/dev/null)
	echo "${_mode:-0}"
}

# Whether the icon belongs on screen at all, game or not.
wanted() {
	case $(mode) in
		2) return 0 ;;
		1) [ -f "$SESSION" ] ;;
		*) return 1 ;;
	esac
}

watcher_pid() {
	_pid=$(cat "$PIDFILE" 2>/dev/null) || return 1
	[ -n "$_pid" ] && grep -q status-icon "/proc/$_pid/cmdline" 2>/dev/null || return 1
	echo "$_pid"
}

# idle | hosting | connected. A host is connected once the broker has a guest;
# a guest is connected by joining, which is what the Netplay app itself reports.
session_state() {
	if [ ! -f "$SESSION" ]; then
		echo idle
		return
	fi
	if grep -q '^role=client$' "$SESSION" 2>/dev/null; then
		echo connected
		return
	fi
	_guests=$(sed -n 's/^guest_count=//p' "$STATE/broker.status" 2>/dev/null)
	if [ "${_guests:-0}" -gt 0 ] 2>/dev/null; then echo connected; else echo hosting; fi
}

# Over gameplay a centred icon would cover the game; the shim draws its own
# in-game notices.
in_game() {
	pidof minarch.elf >/dev/null 2>&1
}

# Straight to Varnish's command FIFO rather than through its binary: the
# binary needs NextUI's library path to load SDL, and the watcher can be
# started from places that do not have it (the quick menu, the boot hook).
# Only while the daemon is alive, as opening a FIFO nobody reads would block.
send() {
	[ -x "$VARNISH" ] && [ -p "$VARNISH_FIFO" ] || return 1
	_vpid=$(cat "$VARNISH_PID" 2>/dev/null)
	[ -n "$_vpid" ] && kill -0 "$_vpid" 2>/dev/null || return 1
	printf '%s\n' "$1" > "$VARNISH_FIFO" 2>/dev/null
}

show() {
	case "$1" in
		connected) _x=28 ;;
		hosting)   _x=14 ;;
		*)         _x=0 ;;
	esac
	send "ICON $CLIENT $POSITION 0 $_x 0 14 14 $SHEET"
}

hide() {
	send "HIDE $CLIENT"
	:
}

watch() {
	echo $$ > "$PIDFILE"
	trap 'hide; rm -f "$PIDFILE"; exit 0' TERM INT HUP
	_shown=
	_since=0
	_daemon=
	while wanted; do
		if in_game; then
			_want=hidden
		else
			_want=$(session_state)
		fi
		_now_daemon=$(cat "$VARNISH_PID" 2>/dev/null)
		_since=$((_since + 1))
		if [ "$_want" != "$_shown" ] || [ "$_now_daemon" != "$_daemon" ] ||
		   { [ "$_want" != hidden ] && [ "$_since" -ge "$RESEND" ]; }; then
			if [ "$_want" = hidden ]; then hide; else show "$_want"; fi
			_shown=$_want
			_since=0
			_daemon=$_now_daemon
		fi
		sleep 1
	done
	hide
	rm -f "$PIDFILE"
}

case "$1" in
	sync)
		if wanted; then
			watcher_pid >/dev/null && exit 0
			# Detached, so it outlives the app that armed the session. The pid
			# is recorded here as well as by the watcher so two quick syncs
			# cannot both start one.
			if command -v setsid >/dev/null 2>&1; then
				setsid sh "$NP/launcher/status-icon.sh" watch </dev/null >/dev/null 2>&1 &
			else
				sh "$NP/launcher/status-icon.sh" watch </dev/null >/dev/null 2>&1 &
			fi
			echo $! > "$PIDFILE"
		else
			sh "$NP/launcher/status-icon.sh" stop
		fi
		;;
	stop)
		_pid=$(watcher_pid) && kill "$_pid" 2>/dev/null
		rm -f "$PIDFILE"
		hide
		;;
	watch)
		watch
		;;
	*)
		echo "usage: $0 sync|stop" >&2
		exit 2
		;;
esac
