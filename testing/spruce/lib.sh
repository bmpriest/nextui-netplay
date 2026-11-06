# Sourced by the rig scripts here: rig.conf, how to reach each device, and the
# DevBridge, capture and log helpers they share. See README.md.
#
# Devices:
#   b   spruce Brick (USB ADB)
#   h   spruce RG35XXH on the ad hoc network, through the tunnel
#   hl  spruce RG35XXH on the house network
#   ap  NextUI Brick serving the ad hoc network (USB ADB)

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
RIG=${RIG:-$HERE/rig.conf}
[ -f "$RIG" ] || { echo "no $RIG: copy rig.example.conf to rig.conf and fill it in" >&2; exit 2; }
. "$RIG"
: "${DEVBRIDGE:=$ROOT/../DevBridge.pak}"
: "${OUT:=$ROOT/testing/results/spruce}"
HELPERS=/tmp/netplay-devbridge
TCPDUMP=/mnt/SDCARD/spruce/bin64/tcpdump
SESSION_FILE=/mnt/SDCARD/Saves/netplay/session
mkdir -p "$OUT"
OUT=$(CDPATH= cd -- "$OUT" && pwd)

[ -n "${DEVBRIDGE_PASSWORD:-}" ] || { echo "set DEVBRIDGE_PASSWORD to the spruce password" >&2; exit 2; }
export DEVBRIDGE_PASSWORD
ASKPASS=$(mktemp "${TMPDIR:-/tmp}/netplay-askpass.XXXXXX")
printf '#!/bin/sh\nprintf "%%s\\n" "$DEVBRIDGE_PASSWORD"\n' > "$ASKPASS"
chmod 700 "$ASKPASS"
trap 'rm -f "$ASKPASS"' EXIT

spruce_ssh() {
	addr=$1 port=$2; shift 2
	SSH_ASKPASS=$ASKPASS SSH_ASKPASS_REQUIRE=force timeout "${ON_TIMEOUT:-180}" \
		setsid -w ssh -p "$port" -o ConnectTimeout=15 -o PubkeyAuthentication=no \
            -o ControlMaster=auto -o ControlPath=/tmp/netplay-fixture-ssh-%C -o ControlPersist=120 \
		-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
		"spruce@$addr" "$@"
}

# on DEV COMMAND: run a shell command on a device; adb's CRLF is stripped.
on() {
	d=$1; shift
	case $d in
	b)  timeout "${ON_TIMEOUT:-180}" adb -s "$BRICK_ADB" shell "$*" | tr -d '\r' ;;
	ap) timeout "${ON_TIMEOUT:-180}" adb -s "$AP_ADB" shell "$*" | tr -d '\r' ;;
	h)  spruce_ssh "$TUNNEL_ADDR" "$TUNNEL_PORT" "$*" ;;
	hl) spruce_ssh "$H700_LAN" 22 "$*" ;;
	*)  echo "unknown device $d" >&2; return 2 ;;
	esac
}

# Copy testing/devbridge/device/*.sh (keys.sh, ctl.sh, quit.sh ...) and this
# rig's device/*.sh to /tmp/netplay-devbridge on a device. Over SSH in one
# connection: a burst of them makes spruce's sshd reset new ones for a while.
push_helpers() {
	on "$1" "mkdir -p $HELPERS"
	case $1 in
	b|ap)
		[ $1 = b ] && s=$BRICK_ADB || s=$AP_ADB
		for f in "$ROOT"/testing/devbridge/device/*.sh "$HERE"/device/*.sh; do
			adb -s "$s" push "$f" "$HELPERS/" >/dev/null
		done ;;
	*)
		tar -cf - -C "$ROOT/testing/devbridge/device" . -C "$HERE/device" . | on "$1" "tar -xf - -C $HELPERS" ;;
	esac
}

# db ARGS...: DevBridge with h (tunnel) and b (adb) attached.
db() {
	db_output=$(mktemp "${TMPDIR:-/tmp}/netplay-db-output.XXXXXX") || return 1
	(cd "$DEVBRIDGE" && timeout "${DB_TIMEOUT:-180}" python3 -P tools/devbridge.py --user spruce \
		--session-root "$OUT/shots" --connect "$TUNNEL_ADDR:h:$TUNNEL_PORT" --adb "$BRICK_ADB=b" "$@") > "$db_output" 2>&1
	db_status=$?
	grep -v '^connected' "$db_output"
	rm -f "$db_output"
	return "$db_status"
}

# shot DEV|all NAME: screenshot; prints the png path(s).
shot() { db exec "$1:SCREEN" "$2" | grep -o '/[^ ]*\.png'; }

# devbridge_open DEV...: DevBridge's app in front, which OPEN_GAME needs. From
# spruce's menu that is the way spruce opens an app: queue it in
# /tmp/cmd_to_run.sh and end the menu (SIGKILL: it ignores SIGTERM), and
# spruce's loop runs it. DevBridge's
# launcher keeps its pid in /tmp/DevBridge-spruce-ui while it is up.
DB_UP='[ -f /tmp/DevBridge-spruce-ui ] && kill -0 $(cat /tmp/DevBridge-spruce-ui) 2>/dev/null'
devbridge_open() {
	for d in "$@"; do
		on $d "$DB_UP && exit 0
			printf '%s\n' 'cd \"/mnt/SDCARD/App/DevBridge\"; chmod a+x \"launch.sh\"; ./launch.sh' > /tmp/cmd_to_run.sh
			kill -9 \$(pidof MainUI) 2>/dev/null
			i=0; while [ \$i -lt 30 ] && ! { $DB_UP; }; do sleep 0.5; i=\$((i+1)); done
			$DB_UP || echo '$d: DevBridge did not open'"
	done
}

# spruce_menu DEV...: spruce's own game menu in front instead of DevBridge's
# app (device/spruce-menu.sh).
spruce_menu() { for d in "$@"; do on $d "sh $HELPERS/spruce-menu.sh"; done; }

# open DEV SYS ROM: open Roms/SYS/ROM in spruce's launcher.
open_game() {
	response=$(db exec "$1:OPEN_GAME" "$2" "$2/$3")
	open_status=$?
	printf '%s\n' "$response"
	[ "$open_status" = 0 ] || return "$open_status"
	printf '%s\n' "$response" | grep -q "$1: ok=queued" || return 1
}

# keys DEV STEP...: keys.sh on the device (exact holds; see keys.sh). keys.sh
# refuses to press while a menu is in front, so spruce's own menu and the
# invitation prompt take DevBridge's BTN instead.
keys() { d=$1; shift; on "$d" "sh $HELPERS/keys.sh $*" >/dev/null 2>&1; }

# The device's address on wlan0.
dev_ip() { on "$1" 'ip -4 addr show wlan0' | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | head -1; }

# capture_start DIR FILTER...: tcpdump on both spruce devices into /tmp/racap,
# detached (setsid on the RG35XXH; the Brick's BusyBox has only
# start-stop-daemon). /tmp/racap/started marks the start for log lookups.
capture_start() {
	dir=$1; shift
	mkdir -p "$dir"; touch "$dir/started"
	if [ "${CAPTURE_MODE:-pcap}" = logs ]; then
		on b 'mkdir -p /tmp/racap; touch /tmp/racap/started' || return 1
		on h 'mkdir -p /tmp/racap; touch /tmp/racap/started' || return 1
		meta "$dir" telemetry "device logs only; no packet capture"
		return 0
	fi
	on h "mkdir -p /tmp/racap; rm -f /tmp/racap/*; touch /tmp/racap/started; setsid $TCPDUMP -i wlan0 -s 0 -U -w /tmp/racap/cap.pcap $* </dev/null >/dev/null 2>&1 & sleep 1; pidof tcpdump >/dev/null || echo 'h: tcpdump did not start'"
	on b "mkdir -p /tmp/racap; rm -f /tmp/racap/*; touch /tmp/racap/started; start-stop-daemon -S -b -m -p /tmp/racap/pid -x $TCPDUMP -- -i wlan0 -s 0 -U -w /tmp/racap/cap.pcap $*; sleep 1; pidof tcpdump >/dev/null || echo 'b: tcpdump did not start'"
}

# meta DIR KEY VALUE: one line of DIR/run.env, read by table.py.
meta() { grep -v "^$2=" "$1/run.env" 2>/dev/null > "$1/run.env.tmp"; echo "$2=$3" >> "$1/run.env.tmp"; mv "$1/run.env.tmp" "$1/run.env"; }

# The ad hoc network the AP Brick serves: SSID and passphrase from its hostapd.conf.
ap_ssid() { on ap 'sed -n "s/^ssid=//p" /tmp/netplay_hostapd.conf 2>/dev/null'; }
ap_psk() { on ap 'sed -n "s/^wpa_passphrase=//p" /tmp/netplay_hostapd.conf 2>/dev/null'; }

# rejoin_h [--end-first]: the RG35XXH back on the ad hoc network, detached
# (the command loses its connection when the WiFi changes), then the tunnel,
# once adhoc-join.sh's done marker can be read through it. Asked through the
# tunnel, or on the house network: a guest whose host went away puts its
# house WiFi back by itself.
rejoin_h() {
	cmd="rm -f /tmp/adhoc-join.done; setsid sh $HELPERS/adhoc-join.sh '$(ap_ssid)' '$(ap_psk)' ${1:-} > /tmp/adhoc-join.log 2>&1 < /dev/null &"
	ON_TIMEOUT=25 on h "$cmd" 2>/dev/null || ON_TIMEOUT=25 on hl "$cmd" 2>/dev/null ||
		{ echo "RG35XXH not reachable on either network" >&2; return 1; }
	for i in $(seq 1 12); do
		sleep 10
		tunnel_up 2 >/dev/null 2>&1 || continue
		ON_TIMEOUT=20 on h '[ -f /tmp/adhoc-join.done ] && ip -4 addr show wlan0 | grep -q "inet 10\.0\.0\."' 2>/dev/null && return 0
	done
	echo "RG35XXH did not come back to the ad hoc network" >&2
	return 1
}

# The RG35XXH's ad hoc address, from the AP's ARP table by its MAC (saved by
# adhoc.sh up), then the tunnel to it through the AP Brick's house address.
# tunnel_up [TRIES]: ARP scans before giving up (default 40, about 2 minutes).
H700_MAC_FILE=$OUT/.h700-mac
tunnel_up() {
    if [ "${NETWORK:-adhoc}" = house ]; then
        tunnel_down
        adb -s "$BRICK_ADB" forward tcp:40244 tcp:22 >/dev/null || return 1
        SSH_ASKPASS=$ASKPASS SSH_ASKPASS_REQUIRE=force setsid -w ssh -f -N -p 40244 -o PubkeyAuthentication=no -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ExitOnForwardFailure=yes -o ServerAliveInterval=15 -L "$TUNNEL_ADDR:$TUNNEL_PORT:$H700_LAN:22" spruce@127.0.0.1 || return 1
        echo "$H700_LAN"
        return 0
    fi
	mac=$(cat "$H700_MAC_FILE" 2>/dev/null)
	[ -n "$mac" ] || { echo "no RG35XXH MAC: run adhoc.sh up first" >&2; return 1; }
	ip=
	for i in $(seq 1 "${1:-40}"); do
		ip=$(on ap 'for a in $(seq 2 30); do ping -c1 -W1 10.0.0.$a >/dev/null 2>&1 & done; wait; cat /proc/net/arp' |
			awk -v m="$mac" 'tolower($4) == tolower(m) && $3 == "0x2" { print $1 }' | head -1)
		[ -n "$ip" ] && break
		sleep 2
	done
	[ -n "$ip" ] || { echo "RG35XXH not on the ad hoc network" >&2; return 1; }
	tunnel_down
    # USB reaches the AP's SSH daemon without the unreliable house-WiFi hop.
    adb -s "$AP_ADB" forward tcp:40243 tcp:22 >/dev/null || return 1
	ssh -f -N -p 40243 -o StrictHostKeyChecking=accept-new -o ExitOnForwardFailure=yes -o ServerAliveInterval=15 -o BatchMode=yes \
		-L "$TUNNEL_ADDR:$TUNNEL_PORT:$ip:22" root@127.0.0.1 || return 1
	echo "$ip"
}
tunnel_down() {
	ps -eo pid,args | awk -v f="$TUNNEL_ADDR:$TUNNEL_PORT:" '$2 == "ssh" && index($0, f) { print $1 }' | xargs -r kill
}
