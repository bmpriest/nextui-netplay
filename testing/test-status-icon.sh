#!/bin/sh
# launcher/status-icon.sh: the Varnish status icon follows the setting and the
# session - grey N (x=0) while armed, white N (x=12) once a peer is connected,
# hidden over gameplay and gone when the session ends.
#
# Varnish is a stand-in: a reader on its command FIFO that logs each line, so
# this checks exactly what would be drawn without a device.
#
#   ./test-status-icon.sh

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
fail=0
ok()   { echo "  ok   $1"; }
bad()  { echo "  FAIL $1"; fail=1; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }

ROOT=$(mktemp -d)
cleanup() {
	SDCARD_PATH="$ROOT" PLATFORM=tg5040 NETPLAY_PAK="$NP" sh "$SCRIPT" stop 2>/dev/null
	[ -n "${READER:-}" ] && kill "$READER" 2>/dev/null
	rm -rf "$ROOT"
}
trap cleanup EXIT

PLATFORM=tg5040
SDCARD_PATH="$ROOT"
NP="$ROOT/Tools/$PLATFORM/Netplay.pak"
STATE="$ROOT/.userdata/shared/Netplay"
VARNISH_DIR="$ROOT/Tools/$PLATFORM/Varnish.pak"
LOG="$ROOT/varnish.log"
BIN="$ROOT/bin"
export PLATFORM SDCARD_PATH NETPLAY_PAK="$NP"
mkdir -p "$NP/launcher" "$STATE" "$VARNISH_DIR" "$BIN"
cp "$HERE/../launcher/status-icon.sh" "$HERE/../launcher/state-path.sh" "$NP/launcher/"
SCRIPT="$NP/launcher/status-icon.sh"

printf '#!/bin/sh\nexit 0\n' > "$VARNISH_DIR/varnish"
chmod 755 "$VARNISH_DIR/varnish"
VARNISH_FIFO="$ROOT/varnish.fifo"
VARNISH_PID="$ROOT/varnish.pid"
export VARNISH_FIFO VARNISH_PID
mkfifo "$VARNISH_FIFO"
# The daemon: logs each command line for as long as the test runs.
( while [ -p "$VARNISH_FIFO" ]; do
	while IFS= read -r line; do echo "$line" >> "$LOG"; done < "$VARNISH_FIFO"
  done ) &
READER=$!
echo "$READER" > "$VARNISH_PID"

# pidof stand-in: a game is "running" while the marker exists.
cat > "$BIN/pidof" <<EOF
#!/bin/sh
[ "\$1" = minarch.elf ] && [ -e "$ROOT/in-game" ]
EOF
chmod 755 "$BIN/pidof"
PATH="$BIN:$PATH"
export PATH

last() { tail -n 1 "$LOG" 2>/dev/null; }

# The watcher polls once a second; give each change a few polls to land.
wait_for() {
	_want=$1
	for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
		[ "$(last)" = "$_want" ] && return 0
		sleep 0.25
	done
	return 1
}

watcher_alive() {
	_pid=$(cat "$STATE/status-icon.pid" 2>/dev/null) || return 1
	[ -n "$_pid" ] && kill -0 "$_pid" 2>/dev/null
}

SHEET="$NP/res/status@%dx.png"
ARMED="ICON netplay bottom-center 0 0 0 12 12 $SHEET"
CONNECTED="ICON netplay bottom-center 0 12 0 12 12 $SHEET"
HIDE="HIDE netplay"

echo "== setting off"
printf 'role=host\n' > "$STATE/session"
printf 'status_icon=0\n' > "$STATE/settings"
sh "$SCRIPT" sync
sleep 1.5
watcher_alive && bad "watcher started with the setting off" || ok "no watcher with the setting off"

echo "== host, no guests: grey"
printf 'status_icon=1\n' > "$STATE/settings"
sh "$SCRIPT" sync
wait_for "$ARMED" && ok "armed icon shown" || bad "armed icon not shown (last: '$(last)')"
sh "$SCRIPT" sync
pids=$(ps -eo args | grep -c '[s]tatus-icon.sh watch')
check "a second sync does not start a second watcher" "$pids" "1"

echo "== host with a guest: white"
printf 'pid=1\nguest_count=1\n' > "$STATE/broker.status"
wait_for "$CONNECTED" && ok "connected icon shown" || bad "connected icon not shown (last: '$(last)')"

echo "== in a game: hidden"
: > "$ROOT/in-game"
wait_for "$HIDE" && ok "hidden during gameplay" || bad "not hidden in game (last: '$(last)')"
rm -f "$ROOT/in-game"
wait_for "$CONNECTED" && ok "shown again after the game" || bad "not restored after game (last: '$(last)')"

echo "== session ends: watcher exits and hides"
rm -f "$STATE/session"
wait_for "$HIDE" && ok "hidden when the session ends" || bad "not hidden at session end (last: '$(last)')"
sleep 1.5
watcher_alive && bad "watcher outlived the session" || ok "watcher exited with the session"

echo "== guest: white once joined"
printf 'role=client\npeer=10.0.0.1\n' > "$STATE/session"
rm -f "$STATE/broker.status"
sh "$SCRIPT" sync
wait_for "$CONNECTED" && ok "joined guest shows connected" || bad "guest icon wrong (last: '$(last)')"

echo "== setting turned off while armed"
printf 'status_icon=0\n' > "$STATE/settings"
sh "$SCRIPT" sync
check "sync with the setting off hides at once" "$(last)" "$HIDE"
sleep 1.5
watcher_alive && bad "watcher survived the setting off" || ok "watcher stopped"

echo "== no Varnish installed"
rm -rf "$VARNISH_DIR"
printf 'status_icon=1\n' > "$STATE/settings"
out=$(sh "$SCRIPT" sync 2>&1; sleep 1.5; sh "$SCRIPT" stop 2>&1)
check "quiet without Varnish" "$out" ""

[ "$fail" = 0 ] && echo "status-icon: all passed" || { echo "status-icon: FAILURES"; exit 1; }
