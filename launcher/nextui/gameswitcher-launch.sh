#!/bin/sh
# Installed as Emus/<platform>/NETPLAY.pak/launch.sh.  The matching ROM lives
# under a hidden Roms directory, so it is eligible for recent.txt without
# adding a fake console to NextUI's Games list.
#
# With the quick menu running, picking Netplay in the Game Switcher opens that
# menu over NextUI rather than the full app: this leaves it a request and
# exits, and the menu appears once NextUI is back in front and settled
# (app/quickmenu.c, in_nextui_menus).

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"

NP="$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak"
. "$NP/launcher/state-path.sh"

_pid=$(cat "$NETPLAY_STATE/quick-menu.pid" 2>/dev/null)
if grep -q '^quick_menu=1$' "$NETPLAY_STATE/settings" 2>/dev/null &&
   [ -n "$_pid" ] && grep -q netplay-quickmenu "/proc/$_pid/cmdline" 2>/dev/null; then
	: > "$NETPLAY_STATE/quick-menu.open"
	exit 0
fi
exec "$NP/launch.sh" --quick
