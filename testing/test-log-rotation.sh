#!/bin/sh
# pak-launch.sh log handling: fresh by default, a bounded ring when the user
# has opted into verbose logging.
#
# This exists because the default cost us the evidence twice. Two "the device
# hung and I rebooted" reports arrived with netplay.txt already overwritten by
# the launch that followed the reboot. Truncating is still the right default -
# it is what keeps the card clean - so the ring has to actually work when it is
# switched on, and has to actually be bounded.
#
#   ./test-log-rotation.sh

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
fail=0
ok()   { echo "  ok   $1"; }
bad()  { echo "  FAIL $1"; fail=1; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }

ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT

PLATFORM=tg5040
SDCARD_PATH="$ROOT"
LOGS_PATH="$ROOT/.userdata/$PLATFORM/logs"
NP="$ROOT/Tools/$PLATFORM/Netplay.pak"
STATE="$ROOT/.userdata/shared/Netplay"
export PLATFORM SDCARD_PATH LOGS_PATH
mkdir -p "$NP/launcher" "$NP/bin/$PLATFORM" "$STATE" "$LOGS_PATH"
# Staged the way the Makefile ships it: pak-launch.sh becomes the pak's own
# launch.sh at the pak root, so $0's directory is the pak, not launcher/.
cp "$HERE/../launcher/pak-launch.sh" "$NP/launch.sh"
chmod 755 "$NP/launch.sh"

# Stand in for netplay.elf: writes the run marker it is given to stdout, which
# is what pak-launch.sh redirects into the log.
cat > "$NP/bin/$PLATFORM/netplay.elf" <<'EOF'
#!/bin/sh
echo "run $1"
EOF
chmod 755 "$NP/bin/$PLATFORM/netplay.elf"

launch() { sh "$NP/launch.sh" "$1"; }
verbose() { printf 'verbose_logs=%s\n' "$1" > "$STATE/settings"; }
body() { cat "$1" 2>/dev/null; }

echo "== default: one fresh log per launch"
verbose 0
launch a; launch b; launch c
check "current log is the last run" "$(body "$LOGS_PATH/netplay.txt")" "run c"
[ -e "$LOGS_PATH/netplay.1.txt" ] \
	&& bad "kept history without opting in" \
	|| ok "no history kept"
check "log count" "$(ls "$LOGS_PATH" | wc -l | tr -d ' ')" "1"

echo
echo "== verbose: previous runs are retained, newest first"
verbose 1
launch d; launch e
check "current log" "$(body "$LOGS_PATH/netplay.txt")" "run e"
check "previous run" "$(body "$LOGS_PATH/netplay.1.txt")" "run d"
check "run before that" "$(body "$LOGS_PATH/netplay.2.txt")" "run c"

echo
echo "== verbose: the ring is bounded"
i=0
while [ $i -lt 12 ]; do launch "x$i"; i=$((i + 1)); done
check "files kept" "$(ls "$LOGS_PATH" | wc -l | tr -d ' ')" "5"   # current + 4
check "oldest retained" "$(body "$LOGS_PATH/netplay.4.txt")" "run x7"
[ -e "$LOGS_PATH/netplay.5.txt" ] && bad "ring exceeded its bound" || ok "nothing beyond the bound"

echo
echo "== NETPLAY_LOG_KEEP is honoured"
rm -f "$LOGS_PATH"/netplay*.txt
i=0
while [ $i -lt 6 ]; do NETPLAY_LOG_KEEP=2 launch "k$i"; i=$((i + 1)); done
check "files kept" "$(ls "$LOGS_PATH" | wc -l | tr -d ' ')" "3"   # current + 2

echo
echo "== turning verbose off clears the ring it left behind"
verbose 0
launch z
check "current log" "$(body "$LOGS_PATH/netplay.txt")" "run z"
check "ring removed" "$(ls "$LOGS_PATH" | wc -l | tr -d ' ')" "1"

echo
echo "== a missing settings file is not verbose"
rm -f "$STATE/settings"
launch p; launch q
check "current log" "$(body "$LOGS_PATH/netplay.txt")" "run q"
[ -e "$LOGS_PATH/netplay.1.txt" ] && bad "kept history with no settings file" || ok "defaults to fresh"

echo
[ $fail -eq 0 ] && echo "all log-rotation tests passed" || echo "log-rotation tests FAILED"
exit $fail
