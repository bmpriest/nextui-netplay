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
		Flip|RGB30|RGB20SX|Miniloong) echo my355 ;;
		*)                       return 1 ;;
	esac
}

# RK3566 bases use Spruce's SDL (KMSDRM on dArkMoss), not the bundled H700
# fbdev build. PyUI names these files without the SONAME's trailing .0.
# Give the native loader aliases without modifying Spruce's installation.
np_spruce_engine_libraries() {
	case "$PLATFORM" in
	my355)
		_np_sdl="${NETPLAY_RUNTIME_ROOT:-/tmp/netplay}/spruce-libs"
		mkdir -p "$_np_sdl" || return 1
		for _np_lib in libSDL2-2.0 libSDL2_image-2.0 libSDL2_ttf-2.0; do
			_np_source="$SDCARD_PATH/App/PyUI/dll/$_np_lib.so"
			[ -f "$_np_source" ] || return 1
			ln -sf "$_np_source" "$_np_sdl/$_np_lib.so.0" || return 1
		done
		printf '%s\n' "$_np_sdl:$NETPLAY_PAK/bin/$PLATFORM:$SDCARD_PATH/spruce/flip/lib:/usr/lib/aarch64-linux-gnu:/usr/lib:/lib:${LD_LIBRARY_PATH:-}"
		;;
	*) printf '%s\n' "$NETPLAY_PAK/bin/$PLATFORM:${LD_LIBRARY_PATH:-}" ;;
	esac
}
