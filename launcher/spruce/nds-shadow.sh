#!/bin/sh
# Entered in place of spruce's drastic64 or dsperate, after its stock setup.
. /mnt/SDCARD/App/Netplay/launcher/firmware.sh
NP=$NETPLAY_PAK
bin=$(basename "$0")
real=/tmp/netplay/nds/$bin
[ -x "$real" ] || exit 127
[ -f "$NETPLAY_STATE/session" ] || exec "$real" "$@"
SPRUCE_PLATFORM=$PLATFORM
PLATFORM=$(np_spruce_platform "$SPRUCE_PLATFORM") || exec "$real" "$@"
NETPLAY_SESSION=$NETPLAY_STATE/session
export PLATFORM NETPLAY_SESSION
. "$NP/launcher/pre-launch.sh"
[ -f "$NETPLAY_SESSION" ] || exec "$real" "$@"
role=$(sed -n 's/^role=//p' "$NETPLAY_SESSION" | head -n 1)
peer=$(sed -n 's/^peer=//p' "$NETPLAY_SESSION" | head -n 1)
case "$role" in
	host) ;;
	client) case "$peer" in ''|*[!0-9.]*) exec "$real" "$@" ;; esac ;;
	*) exec "$real" "$@" ;;
esac
rom=$1
case "$rom" in "$SDCARD_PATH/Roms/"*|/mnt/sdcard/Roms/*)
	sh "$NP/launcher/save-backup.sh" "$rom" || :
	sh "$NP/launcher/invite.sh" send "$rom" "$bin" || : ;;
esac
if [ "$bin" = drastic64 ]; then
	radio=$NP/bin/$PLATFORM/libdrastic_wifi.so
	[ -f "$radio" ] || exec "$real" "$@"
	if [ "$role" = host ]; then DRASTIC_WIFI_LAN=host; else DRASTIC_WIFI_LAN=join:$peer; fi
	DRASTIC_WIFI_LAN_NAME=Netplay
	DRASTIC_WIFI_LAN_TIMEOUT_MS=100
	grep -q '^adhoc_ssid=' "$NETPLAY_SESSION" && DRASTIC_WIFI_LAN_TIMEOUT_MS=50
	export DRASTIC_WIFI_LAN DRASTIC_WIFI_LAN_NAME DRASTIC_WIFI_LAN_TIMEOUT_MS
	real=/tmp/netplay/nds/drastic
	LD_PRELOAD="$radio${LD_PRELOAD:+:$LD_PRELOAD}" exec "$real" "$@"
fi
if [ "$role" = host ]; then set -- "$@" --lan-host Netplay; else set -- "$@" --lan-join "$peer"; fi
DS_LAN_TIMEOUT_MS=100
grep -q '^adhoc_ssid=' "$NETPLAY_SESSION" && DS_LAN_TIMEOUT_MS=50
export DS_LAN_TIMEOUT_MS
set -- "$@" --no-autoload
exec "$real" "$@"
