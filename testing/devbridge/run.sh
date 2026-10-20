#!/bin/sh
#
# Run the two-device gameplay tests through DevBridge.
#
#   run.sh [options] HOST_ADDR GUEST_ADDR TEST...
#
#   --record        capture reference screens instead of checking them
#   --port N        SSH port for both devices (default 22)
#   --identity KEY  SSH key (default ~/.ssh/id_ed25519)
#   --keep-going    run every line even after a failure
#
# HOST_ADDR is the device that hosts the armed session (Player 1), GUEST_ADDR
# the one that joined. Both must already be armed together and sitting in
# DevBridge, not in the Netplay app. See README.md.
#
# Tests use three placeholders, filled in here:
#   {host_refs} {guest_refs}  refs/<platform> for that device
#   {helpers}                 where device/*.sh were copied on the device

set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
DEVBRIDGE=${DEVBRIDGE:-$ROOT/../DevBridge.pak}
HELPERS=/tmp/netplay-devbridge
OUT=$ROOT/testing/results/devbridge

record=0 port=22 identity=$HOME/.ssh/id_ed25519 keep=
while [ $# -gt 0 ]; do
	case $1 in
	--record) record=1 ;;
	--port) port=$2; shift ;;
	--identity) identity=$2; shift ;;
	--keep-going) keep=--keep-going ;;
	-*) echo "unknown option $1" >&2; exit 2 ;;
	*) break ;;
	esac
	shift
done
[ $# -ge 3 ] || { sed -n '3,20p' "$0" >&2; exit 2; }
host=$1 guest=$2; shift 2

[ -f "$DEVBRIDGE/tools/devbridge.py" ] || { echo "DevBridge not found at $DEVBRIDGE (set DEVBRIDGE)" >&2; exit 2; }

sshd() {
	addr=$1; shift
	ssh -p "$port" -i "$identity" -o BatchMode=yes -o ConnectTimeout=10 \
		-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
		"root@$addr" "$@"
}

platform_of() {
	p=$(sshd "$1" 'for d in /mnt/SDCARD/Tools/*/Netplay.pak; do [ -d "$d" ] && basename "$(dirname "$d")"; done' | head -1)
	[ -n "$p" ] || { echo "no Netplay.pak on $1" >&2; exit 1; }
	echo "$p"
}

push_helpers() {
	sshd "$1" "mkdir -p $HELPERS"
	for f in "$HERE"/device/*.sh; do
		sshd "$1" "cat > $HELPERS/$(basename "$f")" < "$f"
	done
}

host_platform=$(platform_of "$host")
guest_platform=$(platform_of "$guest")
echo "host $host ($host_platform), guest $guest ($guest_platform)"
push_helpers "$host"
push_helpers "$guest"
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
		    -e "s#{helpers}#$HELPERS#g" "$test" |
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

	# check.sh only trusts logs newer than this
	sshd "$host" "touch $HELPERS/started"
	sshd "$guest" "touch $HELPERS/started"

	echo "== $name"
	# -P, not -I: VERIFY needs Pillow, often only in user site-packages.
	if python3 -P "$DEVBRIDGE/tools/devbridge.py" --identity "$identity" --port "$port" \
		--connect "$host:host" --connect "$guest:guest" $keep run "$script"; then
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
