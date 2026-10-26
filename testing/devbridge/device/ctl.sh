#!/bin/sh
#
# Drive the Netplay app headless, for unattended test runs.
#
#   ctl.sh status
#   ctl.sh host wifi|adhoc
#   ctl.sh join IP | adhoc | discover [SECONDS]
#   ctl.sh end
#   ctl.sh powersave on|off
#
# The same environment launch.sh gives the app, then `netplay.elf --ctl`.
# Its stdout is the answer ("ok ..." / "error ..." last); its log goes to
# netplay-ctl.txt next to the app's own.

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

mkdir -p "$LOGS_PATH"
cd "$pak" || exit 1
echo "== $(date '+%F %T') ctl $*" >> "$LOGS_PATH/netplay-ctl.txt"
"$pak/bin/$PLATFORM/netplay.elf" --ctl "$@" 2>> "$LOGS_PATH/netplay-ctl.txt"
rc=$?
# What the radio is really doing, which the setting only asks for.
[ "$1" = status ] && echo "radio_powersave=$(sh "$pak/launcher/wifi-platform.sh" powersave-get 2>/dev/null)"
exit $rc
