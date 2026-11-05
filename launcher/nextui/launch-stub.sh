#!/bin/sh
#
# Installed by Netplay.pak as /mnt/SDCARD/Emus/<PLATFORM>/<TAG>.pak/launch.sh
#
# getEmuPath (utils.c) checks the SD card before the system paks, so this file
# takes precedence over $SYSTEM_PATH/paks/Emus/<TAG>.pak/launch.sh. All it does
# is put our minarch.elf shadow on PATH and hand off to the original, which is
# left completely untouched.
#
# Identical for every system - the tag comes from our own path - so the
# installer writes the same bytes to each one.
#
# LEGACY. bind-mount.sh supersedes this: it mounts a staged copy over the pak
# in place and writes nothing to the Emus tree at all. This route remains for
# devices where mounting is unavailable, and so that an install predating the
# mounts can still be uninstalled.

DIR="$(dirname "$0")"
TAG="$(basename "$DIR" .pak)"

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"
: "${SYSTEM_PATH:=$SDCARD_PATH/.system/$PLATFORM}"

ORIGINAL="$SYSTEM_PATH/paks/Emus/$TAG.pak/launch.sh"

if [ ! -f "$ORIGINAL" ]; then
	echo "[netplay] no original pak for $TAG at $ORIGINAL" >&2
	exit 1
fi

# Shared with the bind-mount routes - see pre-launch.sh.
NETPLAY_PAK="$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak"
export NETPLAY_PAK
[ -f "$NETPLAY_PAK/launcher/pre-launch.sh" ] && . "$NETPLAY_PAK/launcher/pre-launch.sh"

# The original recomputes EMU_TAG and CORES_PATH from its own $0, so running it
# at its real location keeps every path it derives correct.
exec "$ORIGINAL" "$@"
