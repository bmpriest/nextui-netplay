#!/bin/sh
#
# Where Netplay lives on spruce, and which NextUI platform build this device
# runs. Sourced by every spruce entry point - the app's launch.sh, the RetroArch
# shadow and the mount hook - before anything from common/.
#
# spruce names devices itself (Brick, AnbernicXX640480, ...); Netplay's binaries
# are built per NextUI platform, and one of those covers each device spruce runs
# on the same hardware. A device not listed here has no build.

: "${SDCARD_PATH:=/mnt/SDCARD}"
NETPLAY_FIRMWARE=spruce
NETPLAY_PAK="$SDCARD_PATH/App/Netplay"
NETPLAY_STATE="$SDCARD_PATH/Saves/netplay"
NETPLAY_LOGS="$NETPLAY_STATE/logs"
export SDCARD_PATH NETPLAY_FIRMWARE NETPLAY_PAK NETPLAY_STATE NETPLAY_LOGS

# spruce's own device name, from its helperFunctions.sh, to Netplay's platform.
np_spruce_platform() {
	case "$1" in
		Brick|BrickPro|SmartPro) echo tg5040 ;;
		SmartProS)               echo tg5050 ;;
		Anbernic*)               echo h700 ;;
		Flip)                    echo my355 ;;
		*)                       return 1 ;;
	esac
}
