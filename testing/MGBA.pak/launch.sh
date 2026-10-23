#!/bin/sh
#
# Netplay-supplied ordinary mGBA pak. The pak deliberately contains only the
# single-player core. Netplay owns the paired core and shim under its own
# cores/override and bin trees, so an ordinary launch remains ordinary and all
# session logging/routing goes through the same minarch shadow as other cores.

EMU_EXE=mgba
CORES_PATH=$(dirname "$0")

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
mkdir -p "$CHEATS_PATH/$EMU_TAG"
mkdir -p "$LOGS_PATH"
export HOME="$USERDATA_PATH"

cd "$HOME" || exit 1

LOG_FILE="$LOGS_PATH/$EMU_TAG.txt"
if ( : > "$LOG_FILE" ) 2>/dev/null; then
	exec > "$LOG_FILE" 2>&1
else
	exec > "/tmp/nextui-$EMU_TAG.txt" 2>&1
fi

CORE="$CORES_PATH/${EMU_EXE}_libretro.so"

NETPLAY_PAK=${NETPLAY_PAK:-"$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak"}
if [ -f "$NETPLAY_PAK/launcher/pre-launch.sh" ]; then
	. "$NETPLAY_PAK/launcher/pre-launch.sh"
fi

echo "launcher: system=$EMU_TAG core=$EMU_EXE rom=$ROM"
exec minarch.elf "$CORE" "$ROM"
