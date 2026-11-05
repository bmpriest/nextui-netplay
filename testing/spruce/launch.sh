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
brick=$(dev_ip b) h700=$(dev_ip h)
proto=$(on b "sed -n 's/^protocol=//p' $SESSION_FILE")
delay=$(on b "sed -n 's/^input_delay=//p' $SESSION_FILE")
case $proto in retroarch) p=ra-netplay ;; *) p=linkplay ;; esac
capture_start "$DIR" host "$brick" and host "$h700" and not port 22
meta "$DIR" protocol "$p"
meta "$DIR" label "delay ${delay:-?}"
meta "$DIR" game "$(basename "${rom%.*}")"
meta "$DIR" host_ip "$brick"
meta "$DIR" guest_ip "$h700"

open_game b "$sys" "$rom"
sleep 3
open_game h "$sys" "$rom"
