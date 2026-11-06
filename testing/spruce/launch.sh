#!/bin/sh
#
# Open a game on both spruce devices in an armed Netplay session (arm.sh
# linkplay|retroarch), capturing the pair's traffic on both.
#
#   launch.sh TAG SYS ROM      e.g. launch.sh lp-sf2 SFC "Street Fighter II (USA).zip"
#
# The Brick (host) opens first and the RG35XXH 3 s later, as the tests do.
# Then play.sh plays the game's test, and collect.sh gathers the run.

. "$(dirname "$0")/lib.sh"

[ $# -eq 3 ] || { sed -n '3,9p' "$0" >&2; exit 2; }
tag=$1 sys=$2 rom=$3
DIR=$OUT/$tag

for d in b h; do on $d "sh $HELPERS/quit.sh retroarch >/dev/null 2>&1"; done
devbridge_open b h
host_status=$(on b "sh $HELPERS/ctl.sh status") || exit 1
guest_status=$(on h "sh $HELPERS/ctl.sh status") || exit 1
field() { printf '%s\n' "$1" | sed -n "s/^$2=//p" | head -1; }
brick=$(field "$host_status" ip)
h700=$(field "$guest_status" ip)
proto=${BROKER_PROTOCOL:-linkplay}
role=$(field "$host_status" role)
guest_role=$(field "$guest_status" role)
[ -n "$brick" ] && [ -n "$h700" ] || { echo "device IP unavailable (host=$brick guest=$h700)" >&2; exit 1; }
[ "$(field "$host_status" armed)" = 1 ] && [ "$(field "$guest_status" armed)" = 1 ] || { echo "Netplay not armed on both devices" >&2; exit 1; }
[ "$role" = host ] && [ "$guest_role" = client ] || { echo "wrong Netplay roles ($role/$guest_role)" >&2; exit 1; }
delay=$(field "$host_status" input_delay)
: "${delay:=configured}"
case $proto in retroarch) p=ra-netplay ;; *) p=linkplay ;; esac
capture_start "$DIR" host "$brick" and host "$h700" and not port 22
meta "$DIR" protocol "$p"
meta "$DIR" label "delay ${delay:-?}"
meta "$DIR" game "$(basename "${rom%.*}")"
meta "$DIR" host_ip "$brick"
meta "$DIR" guest_ip "$h700"

open_game b "$sys" "$rom" || exit 1
sleep 3
open_game h "$sys" "$rom" || exit 1
