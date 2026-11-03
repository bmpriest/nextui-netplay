#!/bin/sh
#
# Netplay's entry in spruce's Apps menu (App/Netplay/launch.sh). spruce runs it
# after PyUI exits and restarts PyUI when it returns, so the screen and the pad
# are ours until then.

DIR="$(cd "$(dirname "$0")" && pwd)"
. /mnt/SDCARD/spruce/scripts/helperFunctions.sh
SPRUCE_PLATFORM="$PLATFORM"
. "$DIR/launcher/firmware.sh"

if ! PLATFORM="$(np_spruce_platform "$SPRUCE_PLATFORM")" ||
   [ ! -x "$DIR/bin/$PLATFORM/netplay.elf" ]; then
	/mnt/SDCARD/App/PyUI/launch.sh -msgDisplay "Netplay has no build for this device ($SPRUCE_PLATFORM) yet." -msgDisplayTimeMs 5000
	exit 0
fi
export PLATFORM SPRUCE_PLATFORM

# NextUI's UI code reads its fonts and images from a card root fixed at build
# time; the spruce build moves that root to nextui/ here (see app/makefile).
SYSTEM_PATH="$DIR/nextui/.system/$PLATFORM"
USERDATA_PATH="$DIR/nextui/.userdata/$PLATFORM"
SHARED_USERDATA_PATH="$DIR/nextui/.userdata/shared"
HOME="$USERDATA_PATH"
export SYSTEM_PATH USERDATA_PATH SHARED_USERDATA_PATH HOME
export LD_LIBRARY_PATH="$DIR/bin/$PLATFORM:$LD_LIBRARY_PATH"
mkdir -p "$USERDATA_PATH" "$SHARED_USERDATA_PATH" "$NETPLAY_STATE" "$NETPLAY_LOGS"

cd "$DIR" || exit 1
LOG="$NETPLAY_LOGS/netplay.txt"
[ -f "$LOG" ] && mv -f "$LOG" "$NETPLAY_LOGS/netplay.1.txt"
"$DIR/bin/$PLATFORM/netplay.elf" "$@" > "$LOG" 2>&1
