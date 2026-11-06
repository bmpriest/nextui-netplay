#!/bin/bash
# Existing gameplay tests under the PC broker, with per-cell restoration.
set -euo pipefail
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
export RIG=${RIG:-$here/rig.conf}
. "$here/lib.sh"
stage=$1 protocol=$2 test=$3 system=$4 rom=$5 core=$6
[ "$test" != aw2 ] || rom='Advance Wars 2 - Black Hole Rising (USA).zip'
export OUT=$DEVBRIDGE_ATTEMPT_DIR/measurement CAPTURE_MODE=logs BROKER_PROTOCOL=$protocol
mkdir -p "$OUT"
# The existing tunnel fixture keeps its discovered guest MAC beside OUT.
# Carry that inventory into each isolated attempt before invoking arm.sh.
cp "$ROOT/testing/results/spruce/.h700-mac" "$OUT/.h700-mac"
stop_games() {
    for d in b h; do
        for mode in retroarch dsp nds; do on "$d" "sh $HELPERS/quit.sh $mode"; done
        # Bind-mounted DS wrappers can retain a different process comm name.
        # Verify the actual executable command line, not pidof alone.
        on "$d" 'pattern="^/tmp/netplay/(nds/(drastic|dsperate)|ra/ra[36][24]\.[^ ]+)( |$)"
            pids=$(pgrep -f "$pattern" || :)
            if [ -n "$pids" ]; then
                echo "Game processes remained after menu quit: $pids"
                for p in $pids; do printf "pid=%s comm=" "$p"; cat /proc/$p/comm; done
                kill $pids 2>/dev/null || :
                i=0; while pgrep -f "$pattern" >/dev/null && [ "$i" -lt 10 ]; do sleep 1; i=$((i+1)); done
                pids=$(pgrep -f "$pattern" || :)
                [ -z "$pids" ] || kill -9 $pids
                sleep 1
            fi
            ! pgrep -f "$pattern"'
    done
}
case $stage in
preflight)
    hashes=()
    for d in b h; do
        on "$d" "test -r '/mnt/SDCARD/Roms/$system/$rom'; df -k /mnt/SDCARD; date"
        hashes+=("$(on "$d" "sha256sum '/mnt/SDCARD/Roms/$system/$rom'" | awk '{print $1}')")
    done
    printf 'ROM hashes: %s %s\n' "${hashes[@]}"
    [ -n "${hashes[0]}" ] && [ "${hashes[0]}" = "${hashes[1]}" ]
    db exec all:STATUS
    ;;
setup)
    stop_games
    for d in b h; do
        push_helpers "$d"
        on "$d" "set -e; mkdir -p /tmp/netplay-suite-snapshot; cp '/mnt/SDCARD/Emu/$system/config.json' /tmp/netplay-suite-snapshot/config.json; cp /mnt/SDCARD/Saves/netplay/settings /tmp/netplay-suite-snapshot/settings; echo '$system' > /tmp/netplay-suite-snapshot/system"
        if [ "$system" = NDS ]; then
            on "$d" "sh $HELPERS/ds-layout.sh set"
            printf '%s\n' '{"layout":"vertical","order":"top-above-bottom","restore":true,"authorization":"user requested common basic DS view"}' > "$DEVBRIDGE_ATTEMPT_DIR/ds-layout.json"
            case $d:$core in b:dsperate-*|h:*-dsperate) emulator=DSperate ;; *) emulator=DraStic-original ;; esac
            on "$d" "/mnt/SDCARD/spruce/bin64/jq --arg emu '$emulator' '.menuOptions |= with_entries(if (.key | startswith(\"Emulator\")) then .value.overrides[\"mariokart.nds\"] = \$emu else . end)' /tmp/netplay-suite-snapshot/config.json > '/mnt/SDCARD/Emu/NDS/config.json'"
        elif [ "$core" != default ] && [ "$core" != cable ]; then
            on "$d" "sh $HELPERS/spruce-core.sh set '$system' '$rom' '$core'"
        fi
    done
    if [ "$system" = GB ] || [ "$system" = GBC ]; then
        instanced=1; [ "$core" != cable ] || instanced=0
        for d in b h; do on "$d" "sh $HELPERS/ctl.sh set instanced $instanced"; done
    fi
    # A durable session supports successive games. Re-arm only when its
    # observed roles/protocol differ, avoiding needless WiFi transitions.
    states=()
    for d in b h; do
        states+=("$(on "$d" "r=\$(sed -n 's/^role=//p' $SESSION_FILE); p=\$(sed -n 's/^protocol=//p' $SESSION_FILE); g=\$(sed -n 's/^instanced_gambatte=//p' $SESSION_FILE); echo \"\$r:\${p:-linkplay}:\${g:-0}\"")")
    done
    if [ "${states[0]%:*}" = "host:$protocol" ] && [ "${states[1]%:*}" = "client:$protocol" ] &&
       { { [ "$system" != GB ] && [ "$system" != GBC ]; } ||
         { [ "${states[0]##*:}" = "$instanced" ] && [ "${states[1]##*:}" = "$instanced" ]; }; }; then
        echo "Reusing observed $protocol host/client session"
    else
        "$here/arm.sh" "$protocol"
    fi
    devbridge_open b h
    ;;
test)
    # Run the full test: launch, verified navigation, gameplay, log assertions,
    # and quit. No --keep-going or automatic reference promotion.
    DEVBRIDGE_HELPERS_READY=1 "$ROOT/testing/devbridge/run.sh" --user spruce --port "$TUNNEL_PORT" "adb:$BRICK_ADB" "$TUNNEL_ADDR" "$ROOT/testing/devbridge/$test.test"
    ;;
collect)
    # Capture the rendered session even when navigation failed and judge is skipped.
    python3 - "$DEVBRIDGE_ATTEMPT_DIR" <<'ARCHIVE'
import pathlib, shlex, shutil, sys
attempt = pathlib.Path(sys.argv[1])
for script in attempt.glob('rendered-*.test'):
    fields = shlex.split(script.read_text().splitlines()[0])
    source = pathlib.Path(fields[2]) / fields[1]
    if source.is_dir():
        shutil.copytree(source, attempt / 'gameplay', dirs_exist_ok=True)
ARCHIVE
    stop_games
    for d in b h; do
        on "$d" "find /mnt/SDCARD/Saves/netplay/logs -type f -mmin -30 | head -50" > "$OUT/$d-log-inventory.txt"
        on "$d" 'f=$(find /mnt/SDCARD/Saves/netplay/logs/netplay-games -name "*.log" -type f -newer /tmp/netplay-devbridge/started | xargs -r ls -t | head -1); [ -z "$f" ] || cat "$f"' > "$OUT/$d.log"
    done
    ;;
judge)
    # Gameplay's own two-sided check.sh assertions are the functional gate.
    # No performance claim: the instrumentation overhead gate is outstanding.
    echo 'Functional assertions are in test.log; performance profile is unvalidated.'
    python3 "$here/broker-suite-judge.py" "$DEVBRIDGE_ATTEMPT_DIR"
    for d in b h; do
        [ -s "$OUT/$d.log" ] || { echo "NO_DATA: $d game log"; exit 1; }
    done
    ;;
teardown)
    stop_games
    for d in b h; do
        on "$d" "set -e; if [ -f /tmp/netplay-suite-snapshot/system ]; then s=\$(cat /tmp/netplay-suite-snapshot/system); sh $HELPERS/spruce-core.sh restore \"\$s\"; cp /tmp/netplay-suite-snapshot/config.json \"/mnt/SDCARD/Emu/\$s/config.json\"; [ ! -f /tmp/netplay-suite-snapshot/settings ] || cp /tmp/netplay-suite-snapshot/settings /mnt/SDCARD/Saves/netplay/settings; rm -f /tmp/netplay-suite-snapshot/system; fi"
    done
    for d in b h; do on "$d" "[ ! -f $HELPERS/ds-layout.sh ] || sh $HELPERS/ds-layout.sh restore"; done
    devbridge_open b h
    db exec all:STATUS
    ;;
finalize)
    "$here/arm.sh" linkplay
    devbridge_open b h
    db exec all:STATUS
    ;;
*) exit 2 ;;
esac
