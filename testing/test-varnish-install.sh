#!/bin/sh
# launcher/varnish-install.sh: reports whether the bundled Varnish is needed,
# installs it, updates an older one without disturbing the rest of a full
# install, and runs its --install. Varnish is a stand-in that logs its calls.
#
#   ./test-varnish-install.sh

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
NP="$ROOT/Tools/$PLATFORM/Netplay.pak"
DEST="$ROOT/Tools/$PLATFORM/Varnish.pak"
LOG="$ROOT/varnish.log"
export PLATFORM SDCARD_PATH NETPLAY_PAK="$NP"
mkdir -p "$NP/launcher"
cp "$HERE/../launcher/varnish-install.sh" "$NP/launcher/"
SCRIPT="$NP/launcher/varnish-install.sh"

# A bundle at the given version whose varnish logs how it was run.
make_bundle() {
	B="$NP/varnish/$PLATFORM"
	mkdir -p "$B/include"
	cat > "$B/varnish" <<EOF
#!/bin/sh
echo "\$(basename "\$(pwd)") \$*" >> "$LOG"
exit \${FAKE_RC:-0}
EOF
	chmod 755 "$B/varnish"
	printf '{\n  "name": "Varnish",\n  "version": "%s"\n}\n' "$1" > "$B/pak.json"
	echo overlay > "$B/varnish_overlay.so"
	echo header > "$B/include/varnish.h"
	printf '#!/bin/sh\n' > "$B/launch.sh"
}

echo "== nothing bundled"
check "unavailable without a bundle" "$(sh "$SCRIPT" status)" "unavailable"
sh "$SCRIPT" install 2>/dev/null
check "install fails without a bundle" "$?" "3"

echo "== fresh install"
make_bundle v0.2.0
check "missing before install" "$(sh "$SCRIPT" status)" "missing"
sh "$SCRIPT" install
check "install succeeds" "$?" "0"
[ -x "$DEST/varnish" ] && ok "varnish installed and executable" || bad "varnish not installed"
check "header copied" "$(cat "$DEST/include/varnish.h" 2>/dev/null)" "header"
check "--install run from the pak" "$(tail -n 1 "$LOG" 2>/dev/null)" "Varnish.pak --install"
check "current after install" "$(sh "$SCRIPT" status)" "current"
leftovers=$(find "$DEST" -name '*.netplay-new' | wc -l)
check "no temporary files left" "$leftovers" "0"

echo "== an older full install"
rm -rf "$DEST"
mkdir -p "$DEST/bin"
echo ffmpeg > "$DEST/bin/ffmpeg"
printf '#!/bin/sh\nexit 0\n' > "$DEST/varnish"
chmod 755 "$DEST/varnish"
printf '{ "name": "Varnish", "version": "v0.1.0" }\n' > "$DEST/pak.json"
check "outdated" "$(sh "$SCRIPT" status)" "outdated"
sh "$SCRIPT" install
check "update succeeds" "$?" "0"
check "updated to the bundled version" "$(sh "$SCRIPT" status)" "current"
check "full install keeps its ffmpeg" "$(cat "$DEST/bin/ffmpeg" 2>/dev/null)" "ffmpeg"

echo "== a newer install is left alone"
printf '{ "name": "Varnish", "version": "v0.10.1" }\n' > "$DEST/pak.json"
check "newer counts as current" "$(sh "$SCRIPT" status)" "current"
printf '{ "name": "Varnish", "version": "0.2" }\n' > "$DEST/pak.json"
check "0.2 equals v0.2.0" "$(sh "$SCRIPT" status)" "current"

echo "== varnish --install fails"
rm -rf "$DEST"
FAKE_RC=1 sh "$SCRIPT" install 2>/dev/null
check "reported" "$?" "2"

[ "$fail" = 0 ] && echo "varnish-install: all passed" || { echo "varnish-install: FAILURES"; exit 1; }
