#!/bin/sh
#
# Put this spruce device on an ad hoc network (Netplay's spruce join: spruce's
# own supplicant keeps the network, so spruce's WiFi script does not undo it).
#
#   adhoc-join.sh SSID PSK [--end-first]
#
# --end-first ends this device's Netplay session first, which puts the house
# WiFi back, then joins: how a guest re-arms. A renewal follows if no lease
# came within 8 s, and the broadcast route RetroArch's LAN discovery needs
# (the NextUI AP's DHCP gives no default route). Prints the address.

ssid=$1 psk=$2
# /tmp/adhoc-join.done marks the end, for a caller that lost its connection.
rm -f /tmp/adhoc-join.done
trap 'touch /tmp/adhoc-join.done' EXIT
[ "${3:-}" = --end-first ] && { sh /tmp/netplay-devbridge/ctl.sh end >/dev/null 2>&1; sleep 3; }
cd /mnt/SDCARD/App/Netplay || exit 1
. /mnt/SDCARD/spruce/scripts/helperFunctions.sh >/dev/null 2>&1
sh launcher/wifi-platform.sh spruce-join "$ssid" "$psk"
i=0
while [ $i -lt 25 ] && ! ip -4 addr show wlan0 | grep -q 'inet 10\.0\.0\.'; do
	sleep 1
	i=$((i + 1))
	[ $i = 8 ] && sh launcher/wifi-platform.sh spruce-renew
done
ip route add 255.255.255.255 dev wlan0 2>/dev/null
ip -4 addr show wlan0 | sed -n 's/.*inet \([0-9.]*\).*/\1/p'
