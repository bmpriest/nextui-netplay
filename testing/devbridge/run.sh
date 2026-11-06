#!/bin/sh
#
# Run the two-device gameplay tests through DevBridge.
#
#   run.sh [options] HOST_ADDR GUEST_ADDR TEST...
#
#   --record        capture reference screens instead of checking them
#   --port N        SSH port for both devices (default 22)
#   --identity KEY  SSH key (default ~/.ssh/id_ed25519)
#   --user USER     SSH user for both devices (default root; spruce: spruce).
#                   With $DEVBRIDGE_PASSWORD set, log in with that password.
#   --keep-going    run every line even after a failure
#   --play-only     the game is already running on both (another way of
#                   starting it, such as pure RetroArch's menus): skip the
#                   test's launch, its log checks and its quit
#
# HOST_ADDR is the device that hosts the armed session (Player 1), GUEST_ADDR
# the one that joined. Either may be adb:SERIAL for a device on USB ADB, which
# mixes with an SSH one (a NextUI Brick on USB against a spruce device on WiFi). Both must already be armed together and sitting in
# DevBridge, not in the Netplay app. See README.md.
#
# Tests use three placeholders, filled in here:
#   {host_refs} {guest_refs}  refs/<platform> for that device
#   {helpers}                 where device/*.sh were copied on the device
#
# On spruce (App/Netplay), refs are refs/spruce-<platform>, and OPEN_GAME's
# NextUI ROM folders ("Game Boy (GB)/...") become spruce's ("GB/...").

set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
DEVBRIDGE=${DEVBRIDGE:-$ROOT/../DevBridge.pak}
HELPERS=/tmp/netplay-devbridge
OUT=$ROOT/testing/results/devbridge

record=0 port=22 identity=$HOME/.ssh/id_ed25519 keep= user=root play_only=0
while [ $# -gt 0 ]; do
	case $1 in
	--record) record=1 ;;
	--port) port=$2; shift ;;
	--identity) identity=$2; shift ;;
	--user) user=$2; shift ;;
	--keep-going) keep=--keep-going ;;
	--play-only) play_only=1 ;;
	-*) echo "unknown option $1" >&2; exit 2 ;;
	*) break ;;
	esac
	shift
done
[ $# -ge 3 ] || { sed -n '3,23p' "$0" >&2; exit 2; }
host=$1 guest=$2; shift 2

[ -f "$DEVBRIDGE/tools/devbridge.py" ] || { echo "DevBridge not found at $DEVBRIDGE (set DEVBRIDGE)" >&2; exit 2; }

askpass=
if [ -n "${DEVBRIDGE_PASSWORD:-}" ]; then
	askpass=$(mktemp "${TMPDIR:-/tmp}/netplay-askpass.XXXXXX")
	printf '#!/bin/sh\nprintf "%%s\\n" "$DEVBRIDGE_PASSWORD"\n' > "$askpass"
	chmod 700 "$askpass"
	trap 'rm -f "$askpass"' EXIT
fi

sshd() {
	addr=$1; shift
	# adb shell ends its lines with CRLF.
	case $addr in adb:*)
		adb -s "${addr#adb:}" shell "$@" | tr -d '\r'
		return ;;
	esac
	if [ -n "$askpass" ]; then
		SSH_ASKPASS=$askpass SSH_ASKPASS_REQUIRE=force DEVBRIDGE_PASSWORD=$DEVBRIDGE_PASSWORD \
			setsid -w ssh -p "$port" -o ConnectTimeout=10 -o PubkeyAuthentication=no \
			-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
			"$user@$addr" "$@"
		return
	fi
	ssh -p "$port" -i "$identity" -o BatchMode=yes -o ConnectTimeout=10 \
		-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
		"$user@$addr" "$@"
}

# NextUI: the Tools/<platform> holding Netplay.pak. spruce: spruce-<the
# platform its App/Netplay build runs as>.
platform_of() {
	p=$(sshd "$1" 'for d in /mnt/SDCARD/Tools/*/Netplay.pak; do [ -d "$d" ] && basename "$(dirname "$d")"; done
		if [ -f /mnt/SDCARD/App/Netplay/launcher/firmware.sh ]; then
			. /mnt/SDCARD/spruce/scripts/helperFunctions.sh >/dev/null 2>&1
			. /mnt/SDCARD/App/Netplay/launcher/firmware.sh
			echo "spruce-$(np_spruce_platform "$PLATFORM")"
		fi' | head -1)
	[ -n "$p" ] || { echo "no Netplay on $1" >&2; exit 1; }
	echo "$p"
}

# For a spruce device's lines (and all: lines when both are spruce):
#   - NextUI's "Name (TAG)/rom" becomes "TAG/rom";
#   - a line ending "# not-spruce" is dropped, and "#spruce LINE" becomes LINE,
#     for steps spruce's RetroArch needs done differently.
spruce_roms() {
	roles=
	case $host_platform in spruce-*) roles="$roles host" ;; esac
	case $guest_platform in spruce-*) roles="$roles guest" ;; esac
	case "$roles" in " host guest") roles="$roles all" ;; esac
	awk -v roles="$roles" '
		function spruce(line,   r) { r = line; sub(/:.*/, "", r); return index(roles " ", " " r " ") > 0 }
		/^#spruce / { if (!spruce(substr($0, 9))) next; $0 = substr($0, 9) }
		/# not-spruce$/ { if (spruce($0)) next; sub(/ *# not-spruce$/, "") }
		{
			if ($0 ~ /^[a-z]+:OPEN_GAME / && spruce($0))
				$0 = gensub(/"[^"\/]* \(([A-Z0-9]+)\)\//, "\"\\1/", 1)
			print
		}'
}

# --play-only: from `all:WAIT_FOR game` on (what launched the game is gone),
# without check.sh, quit.sh or the closing wait for DevBridge.
play_only() {
	if [ $play_only = 1 ]; then
		awk '/^all:WAIT_FOR game/ { on = 1 } !on { next }
		     /check\.sh|quit\.sh|^all:WAIT_FOR devbridge/ { next } { print }'
	else
		cat
	fi
}

push_helpers() {
	sshd "$1" "mkdir -p $HELPERS"
	for f in "$HERE"/device/*.sh; do
		case $1 in
		adb:*) adb -s "${1#adb:}" push "$f" "$HELPERS/" >/dev/null ;;
		*) sshd "$1" "cat > $HELPERS/$(basename "$f")" < "$f" ;;
		esac
	done
}

# DevBridge's way to reach each device: --adb SERIAL=ALIAS or --connect.
devbridge_target() {
	case $1 in
	adb:*) echo "--adb ${1#adb:}=$2" ;;
	*) echo "--connect $1:$2" ;;
	esac
}

auth="--identity $identity"
[ -z "$askpass" ] || auth=

host_platform=$(platform_of "$host")
guest_platform=$(platform_of "$guest")
echo "host $host ($host_platform), guest $guest ($guest_platform)"
if [ "${DEVBRIDGE_HELPERS_READY:-0}" != 1 ]; then
	push_helpers "$host"
	push_helpers "$guest"
fi
mkdir -p "$OUT" "$HERE/refs/$host_platform" "$HERE/refs/$guest_platform"

status=0
for test in "$@"; do
	name=$(basename "$test" .test)
	session=$name-$(date +%Y%m%d-%H%M%S)
	script=$(mktemp "${TMPDIR:-/tmp}/netplay-devbridge.XXXXXX")

	{
		echo "SESSION $session $OUT"
		sed -e "s#{host_refs}#$HERE/refs/$host_platform#g" \
		    -e "s#{guest_refs}#$HERE/refs/$guest_platform#g" \
		    -e "s#{helpers}#$HELPERS#g" "$test" | spruce_roms | play_only |
		if [ $record = 1 ]; then
			# VERIFY_WAIT REF TIMEOUT -> wait the whole timeout, then capture
			# the screen under a name that says where its reference belongs.
			awk '{
				if (match($0, /^[a-z]+:VERIFY_WAIT /)) {
					split($0, w, " "); split(w[1], p, ":")
					n = split(w[2], parts, "/")
					ref = parts[n]; sub(/\.png$/, "", ref)
					print "WAIT " w[3]
					print p[1] ":SCREEN rec__" parts[n - 1] "__" ref
				} else print
			}'
		else
			cat
		fi
	} > "$script"
	if [ -n "${DEVBRIDGE_ATTEMPT_DIR:-}" ]; then
		cp "$script" "$DEVBRIDGE_ATTEMPT_DIR/rendered-$name.test"
	fi

	# check.sh only trusts logs newer than this. The host's confirmed checkpoint
	# outlives a clean exit (a relaunched host resumes from it), so without
	# clearing it a test would start where the previous run of the game ended.
	# A game already running (--play-only) keeps its checkpoint.
	if [ $play_only = 1 ]; then
		sshd "$host" "touch $HELPERS/started"
	else
		sshd "$host" "touch $HELPERS/started; rm -f /tmp/netplay-host-*.state"
	fi
	sshd "$guest" "touch $HELPERS/started"

	echo "== $name"
	# -P, not -I: VERIFY needs Pillow, often only in user site-packages.
	# shellcheck disable=SC2046
	if python3 -P "$DEVBRIDGE/tools/devbridge.py" $auth --port "$port" \
		--user "$user" $(devbridge_target "$host" host) $(devbridge_target "$guest" guest) \
		$keep run "$script"; then
		echo "== $name: PASS"
	else
		echo "== $name: FAIL (session $OUT/$session)"
		status=1
	fi
	rm -f "$script"

	if [ $record = 1 ]; then
		for f in "$OUT/$session"/rec__*.png; do
			[ -f "$f" ] || continue
			base=$(basename "$f" .png); base=${base#rec__}
			plat=${base%%__*}; ref=${base#*__}; ref=${ref%-host}; ref=${ref%-guest}
			cp "$f" "$HERE/refs/$plat/$ref.png"
			echo "recorded refs/$plat/$ref.png"
		done
	fi
done
exit $status
