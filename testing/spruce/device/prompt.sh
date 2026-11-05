#!/bin/sh
#
# Netplay's invitation prompt on its own, with the environment
# launcher/spruce/invite.sh gives it (spruce's App/Netplay).
#
#   prompt.sh --title T --text T [--art FILE] [--accept L] [--decline L]
#             [--browse L] [--timeout S]
#   prompt.sh --nudge      a D-pad down/up on the pad, so a resumed menu repaints
#
# Prints accept|decline|browse|timeout; exits 0, 1, 3 or 2. Its timings go to
# stderr ("prompt: display up after N ms, art N ms, first frame after N ms").

. /mnt/SDCARD/spruce/scripts/helperFunctions.sh >/dev/null 2>&1
. /mnt/SDCARD/App/Netplay/launcher/firmware.sh
pak=$NETPLAY_PAK
PLATFORM=$(np_spruce_platform "$PLATFORM") || { echo "error no Netplay build for this device" >&2; exit 1; }
SYSTEM_PATH=$pak/nextui/.system/$PLATFORM
USERDATA_PATH=$pak/nextui/.userdata/$PLATFORM
SHARED_USERDATA_PATH=$pak/nextui/.userdata/shared
LOGS_PATH=$NETPLAY_LOGS
HOME=$USERDATA_PATH
LD_LIBRARY_PATH=$pak/bin/$PLATFORM:${LD_LIBRARY_PATH:-}
export PLATFORM SYSTEM_PATH USERDATA_PATH SHARED_USERDATA_PATH LOGS_PATH HOME LD_LIBRARY_PATH
mkdir -p "$LOGS_PATH"
cd "$pak" || exit 1
[ "${1:-}" = --nudge ] && exec "$pak/bin/$PLATFORM/netplay.elf" --nudge
exec "$pak/bin/$PLATFORM/netplay.elf" --prompt "$@"
