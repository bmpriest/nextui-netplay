#!/bin/sh
# launcher/quick-menu.sh: exactly one quick menu runs while the setting is on
# and Varnish is installed, and none otherwise. The menu binary is a stand-in
# that just waits, so this checks the lifecycle without a device.
#
#   ./test-quick-menu.sh

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
fail=0
ok()   { echo "  ok   $1"; }
bad()  { echo "  FAIL $1"; fail=1; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }

ROOT=$(mktemp -d)
cleanup() {
	SDCARD_PATH="$ROOT" PLATFORM=tg5040 NETPLAY_PAK="$NP" sh "$SCRIPT" stop 2>/dev/null
	rm -rf "$ROOT"
}
trap cleanup EXIT

PLATFORM=tg5040
SDCARD_PATH="$ROOT"
NP="$ROOT/Tools/$PLATFORM/Netplay.pak"
STATE="$ROOT/.userdata/shared/Netplay"
VARNISH_DIR="$ROOT/Tools/$PLATFORM/Varnish.pak"
export PLATFORM SDCARD_PATH NETPLAY_PAK="$NP"
mkdir -p "$NP/launcher" "$NP/bin/$PLATFORM" "$STATE" "$VARNISH_DIR"
cp "$HERE/../launcher/quick-menu.sh" "$HERE/../launcher/state-path.sh" "$NP/launcher/"
SCRIPT="$NP/launcher/quick-menu.sh"

printf '#!/bin/sh\nexit 0\n' > "$VARNISH_DIR/varnish"
chmod 755 "$VARNISH_DIR/varnish"
printf '#!/bin/sh\nwhile :; do sleep 1; done\n' > "$NP/bin/$PLATFORM/netplay-quickmenu.elf"
chmod 755 "$NP/bin/$PLATFORM/netplay-quickmenu.elf"

running() { ps -eo args | grep "[n]etplay-quickmenu.elf" | grep -vc grep; }
settle() { sleep 0.3; }

echo "== setting off"
printf 'quick_menu=0\n' > "$STATE/settings"
sh "$SCRIPT" sync; settle
check "nothing runs with the setting off" "$(running)" "0"

echo "== setting on"
printf 'quick_menu=1\n' > "$STATE/settings"
sh "$SCRIPT" sync; settle
check "one menu runs" "$(running)" "1"
sh "$SCRIPT" sync; settle
check "a second sync does not start another" "$(running)" "1"

echo "== setting turned off"
printf 'quick_menu=0\n' > "$STATE/settings"
sh "$SCRIPT" sync; settle
check "sync with the setting off stops it" "$(running)" "0"
[ -e "$STATE/quick-menu.pid" ] && bad "pid file left behind" || ok "pid file removed"

echo "== no Varnish"
printf 'quick_menu=1\n' > "$STATE/settings"
rm -rf "$VARNISH_DIR"
out=$(sh "$SCRIPT" sync 2>&1); settle
check "quiet without Varnish" "$out" ""
check "nothing runs without Varnish" "$(running)" "0"

[ "$fail" = 0 ] && echo "quick-menu: all passed" || { echo "quick-menu: FAILURES"; exit 1; }
