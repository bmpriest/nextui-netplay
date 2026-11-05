#!/bin/sh
#
# Put both spruce devices on the NextUI Brick's ad hoc network, or take them off.
#
#   adhoc.sh up     AP up (ctl.sh host adhoc on the NextUI Brick), both spruce
#                   devices join it, the spruce Brick's radio is kept awake,
#                   the tunnel to the RG35XXH comes up
#   adhoc.sh down   both spruce sessions end (which puts their house WiFi back),
#                   the Brick's power save and the AP go back as they were
#   adhoc.sh tunnel the tunnel only, after the RG35XXH rejoined
#
# The NextUI Brick is only the access point: both spruce devices are plain
# stations on it, and the PC reaches the RG35XXH through the AP's house side.

. "$(dirname "$0")/lib.sh"

# xradio (the Brick's radio) drops the AP under power save ("BSS lost"); its
# debugfs switch keeps it awake until reboot or until it is written back.
PS=/sys/kernel/debug/ieee80211/phy0/xradio/ps_disable

case ${1:-} in
up)
	for d in ap b hl; do push_helpers $d; done
	ssid=$(ap_ssid)
	if [ -z "$ssid" ] || [ -z "$(on ap 'pidof hostapd')" ]; then
		on ap "sh $HELPERS/ctl.sh host adhoc" | tail -1
		ssid=$(ap_ssid)
	fi
	psk=$(ap_psk)
	[ -n "$ssid" ] || { echo "the AP Brick did not come up" >&2; exit 1; }
	echo "ssid $ssid"

	# The RG35XXH leaves the house network mid-command, so it joins detached.
	if mac=$(on hl 'cat /sys/class/net/wlan0/address' 2>/dev/null) && [ -n "$mac" ]; then
		echo "$mac" > "$H700_MAC_FILE"
		on hl "setsid sh $HELPERS/adhoc-join.sh '$ssid' '$psk' > /tmp/adhoc-join.log 2>&1 < /dev/null &"
	else
		echo "RG35XXH not on the house network; assuming it is already on $ssid"
	fi
	echo "brick $(on b "sh $HELPERS/adhoc-join.sh '$ssid' '$psk'; [ -f /tmp/netplay-ps-was ] || sed -n 's/^ps_disable=\([0-9]*\).*/\1/p' $PS > /tmp/netplay-ps-was 2>/dev/null; echo 1 > $PS")"
	ip=$(tunnel_up) || exit 1
	echo "h700 $ip (tunnel $TUNNEL_ADDR:$TUNNEL_PORT)"
	push_helpers h
	;;
down)
	on h "setsid sh -c 'sh $HELPERS/ctl.sh end' > /tmp/adhoc-end.log 2>&1 < /dev/null &" 2>/dev/null
	on b "sh $HELPERS/ctl.sh end | tail -1; echo \$(cat /tmp/netplay-ps-was 2>/dev/null || echo 0) > $PS; rm -f /tmp/netplay-ps-was"
	on ap "sh $HELPERS/ctl.sh end" | tail -1
	tunnel_down
	for i in $(seq 1 60); do ping -c1 -W2 "$H700_LAN" >/dev/null 2>&1 && break; done
	echo "brick $(dev_ip b), h700 back on $H700_LAN: $(ping -c1 -W2 "$H700_LAN" >/dev/null 2>&1 && echo yes || echo no)"
	;;
tunnel)
	ip=$(tunnel_up) || exit 1
	echo "h700 $ip (tunnel $TUNNEL_ADDR:$TUNNEL_PORT)"
	;;
*)
	sed -n '3,13p' "$0" >&2; exit 2 ;;
esac
