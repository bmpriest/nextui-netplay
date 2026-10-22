#!/bin/sh
# Netplay quick menu: START in NextUI's menus opens a Netplay menu, drawn by
# Varnish.pak's overlay. bin/<platform>/netplay-quickmenu.elf does the work;
# this keeps exactly one running while the setting is on.
#
#   quick-menu.sh sync    start or stop it to match the setting
#   quick-menu.sh stop    stop it
#
# Called when the setting changes and from the boot hook. Optional by design:
# without Varnish there is nothing to draw the menu, so it does not run.

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"

NP="${NETPLAY_PAK:-$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak}"
. "$NP/launcher/state-path.sh"
STATE="$NETPLAY_STATE"
SETTINGS="$STATE/settings"
PIDFILE="$STATE/quick-menu.pid"
BIN="$NP/bin/$PLATFORM/netplay-quickmenu.elf"
VARNISH="$SDCARD_PATH/Tools/$PLATFORM/Varnish.pak/varnish"

enabled() {
	grep -q '^quick_menu=1$' "$SETTINGS" 2>/dev/null
}

menu_pid() {
	_pid=$(cat "$PIDFILE" 2>/dev/null) || return 1
	[ -n "$_pid" ] && grep -q netplay-quickmenu "/proc/$_pid/cmdline" 2>/dev/null || return 1
	echo "$_pid"
}

stop() {
	_pid=$(menu_pid) && kill "$_pid" 2>/dev/null
	rm -f "$PIDFILE"
}

case "$1" in
	sync)
		if enabled && [ -x "$VARNISH" ] && [ -x "$BIN" ]; then
			menu_pid >/dev/null && exit 0
			# Detached, so it outlives the app or boot script that started it.
			# It records its own pid; this one covers two syncs close together.
			if command -v setsid >/dev/null 2>&1; then
				SDCARD_PATH="$SDCARD_PATH" PLATFORM="$PLATFORM" \
					setsid "$BIN" </dev/null >/dev/null 2>&1 &
			else
				SDCARD_PATH="$SDCARD_PATH" PLATFORM="$PLATFORM" \
					"$BIN" </dev/null >/dev/null 2>&1 &
			fi
			echo $! > "$PIDFILE"
		else
			stop
		fi
		;;
	stop)
		stop
		;;
	*)
		echo "usage: $0 sync|stop" >&2
		exit 2
		;;
esac
