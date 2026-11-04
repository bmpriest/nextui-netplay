#!/bin/sh
#
# Drive the Netplay app headless, for unattended test runs.
#
#   ctl.sh status
#   ctl.sh host wifi|adhoc
#   ctl.sh join IP | adhoc | discover [SECONDS]
#   ctl.sh end
#   ctl.sh powersave on|off
#   ctl.sh pak dsperate|drastic install
#   ctl.sh set adhoc_switch|compatibility_cores 0|1
#   ctl.sh set instanced 0|1|2
#
# The same environment launch.sh gives the app (NextUI's Netplay.pak or
# spruce's App/Netplay), then `netplay.elf --ctl`.
# Its stdout is the answer ("ok ..." / "error ..." last); its log goes to
# netplay-ctl.txt next to the app's own.

if [ -f /mnt/SDCARD/App/Netplay/launcher/firmware.sh ]; then
	# spruce: as App/Netplay/launch.sh sets it up.
	. /mnt/SDCARD/spruce/scripts/helperFunctions.sh >/dev/null 2>&1
	. /mnt/SDCARD/App/Netplay/launcher/firmware.sh
	pak=$NETPLAY_PAK
	PLATFORM=$(np_spruce_platform "$PLATFORM") || { echo "error no Netplay build for this device"; exit 1; }
	SYSTEM_PATH=$pak/nextui/.system/$PLATFORM
	USERDATA_PATH=$pak/nextui/.userdata/$PLATFORM
	SHARED_USERDATA_PATH=$pak/nextui/.userdata/shared
	LOGS_PATH=$NETPLAY_LOGS
	HOME=$USERDATA_PATH
	LD_LIBRARY_PATH=$pak/bin/$PLATFORM:${LD_LIBRARY_PATH:-}
	export PLATFORM SYSTEM_PATH USERDATA_PATH SHARED_USERDATA_PATH LOGS_PATH HOME LD_LIBRARY_PATH
else
	for pak in /mnt/SDCARD/Tools/*/Netplay.pak; do
		[ -x "$pak/launch.sh" ] && break
	done
	[ -d "$pak" ] || { echo "error no Netplay.pak"; exit 1; }

	: "${SDCARD_PATH:=/mnt/SDCARD}"
	PLATFORM=$(basename "$(dirname "$pak")")
	SYSTEM_PATH=$SDCARD_PATH/.system/$PLATFORM
	LOGS_PATH=$SDCARD_PATH/.userdata/$PLATFORM/logs
	NETPLAY_PAK=$pak
	HOME=$SDCARD_PATH/.userdata/$PLATFORM
	LD_LIBRARY_PATH=$pak:$pak/bin:$pak/bin/$PLATFORM:$SYSTEM_PATH/lib:/usr/trimui/lib:${LD_LIBRARY_PATH:-}
	PATH=$SYSTEM_PATH/bin:$PATH
	export SDCARD_PATH PLATFORM SYSTEM_PATH LOGS_PATH NETPLAY_PAK HOME LD_LIBRARY_PATH PATH
fi

mkdir -p "$LOGS_PATH"
cd "$pak" || exit 1
echo "== $(date '+%F %T') ctl $*" >> "$LOGS_PATH/netplay-ctl.txt"
"$pak/bin/$PLATFORM/netplay.elf" --ctl "$@" 2>> "$LOGS_PATH/netplay-ctl.txt"
rc=$?
# What the radio is really doing, which the setting only asks for.
[ "$1" = status ] && echo "radio_powersave=$(sh "$pak/launcher/wifi-platform.sh" powersave-get 2>/dev/null)"
exit $rc
