#!/bin/sh
#
# Arm a Netplay session between the two spruce devices on the ad hoc network
# (adhoc.sh up first): the spruce Brick hosts, the RG35XXH joins.
#
#   arm.sh linkplay     Netplay's own protocol
#   arm.sh retroarch    RetroArch's protocol, carried by Netplay
#   arm.sh off          no session on either, for pure RetroArch
#
# Ending a session on spruce puts the house WiFi back, so each device ends,
# arms, then rejoins the ad hoc network; the RG35XXH does so detached, and the
# tunnel follows it. About a minute. `ctl.sh set instanced 0|1` on both
# before arming picks Gambatte's link cable or the paired (instanced) consoles.

. "$(dirname "$0")/lib.sh"

proto=${1:-}
case $proto in linkplay|retroarch|off) ;; *) sed -n '3,13p' "$0" >&2; exit 2 ;; esac
ssid=$(ap_ssid) psk=$(ap_psk)
[ -n "$ssid" ] || { echo "no ad hoc network: adhoc.sh up first" >&2; exit 1; }

# The guest first: a guest whose host ends puts its house WiFi back by itself,
# mid-way through anything this script asks of it.
rejoin_h --end-first > /dev/null || exit 1
echo "h700 $(dev_ip h)"

on b "sh $HELPERS/ctl.sh end >/dev/null 2>&1"
[ $proto = off ] || on b "sh $HELPERS/ctl.sh host wifi --protocol $proto" | tail -1
brick=$(on b "sh $HELPERS/adhoc-join.sh '$ssid' '$psk'")
on b 'echo 1 > /sys/kernel/debug/ieee80211/phy0/xradio/ps_disable'
echo "brick $brick"
if [ $proto = off ]; then
	# The app takes RetroArch's hook down when a session ends; make sure.
	for d in b h; do on $d 'sh /mnt/SDCARD/App/Netplay/launcher/ra-hook.sh down >/dev/null 2>&1; sh /mnt/SDCARD/App/Netplay/launcher/ra-hook.sh status' | sed "s/^/$d: /"; done
else
	on h "sh $HELPERS/ctl.sh join $brick --protocol $proto" | tail -1
	for d in b h; do echo "$d: $(on $d "grep -E '^(role|protocol|input_delay)=' $SESSION_FILE | tr '\n' ' '")"; done
fi
