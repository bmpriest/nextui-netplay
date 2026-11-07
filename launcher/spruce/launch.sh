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
PYUI_BASE_LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}
LD_LIBRARY_PATH=$(np_spruce_engine_libraries) || {
	/mnt/SDCARD/App/PyUI/launch.sh -msgDisplay "Netplay needs Spruce's SDL2 runtime in App/PyUI/dll." -msgDisplayTimeMs 5000
	exit 1
}
export LD_LIBRARY_PATH
NETPLAY_ENGINE_LD_LIBRARY_PATH=$LD_LIBRARY_PATH
export NETPLAY_ENGINE_LD_LIBRARY_PATH
mkdir -p "$USERDATA_PATH" "$SHARED_USERDATA_PATH" "$NETPLAY_STATE" "$NETPLAY_LOGS"

cd "$DIR" || exit 1
LOG="$NETPLAY_LOGS/netplay.txt"
[ -f "$LOG" ] && mv -f "$LOG" "$NETPLAY_LOGS/netplay.1.txt"

# The native binary remains the control plane and still serves headless and
# invitation-prompt calls. The regular Apps entry uses spruce's own PyUI.
case "$1" in
	--*) exec "$DIR/bin/$PLATFORM/netplay.elf" "$@" >> "$LOG" 2>&1 ;;
esac

LD_LIBRARY_PATH=$PYUI_BASE_LD_LIBRARY_PATH
RUN_DIR=$DIR
PYTHON=$(get_python_path)
case "$SPRUCE_PLATFORM" in
	Brick|BrickPro|SmartPro|SmartProS)
		RUN_DIR=/usr/trimui/bin
		PYTHON=$SDCARD_PATH/spruce/flip/bin/MainUI
		PYSDL2_DLL_PATH=$SDCARD_PATH/spruce/brick/sdl2
		case "$SPRUCE_PLATFORM" in
			Brick) NETPLAY_PYUI_DEVICE=TRIMUI_BRICK ;;
			BrickPro) NETPLAY_PYUI_DEVICE=TRIMUI_BRICK_PRO ;;
			SmartProS) NETPLAY_PYUI_DEVICE=TRIMUI_SMART_PRO_S ;;
			*) NETPLAY_PYUI_DEVICE=TRIMUI_SMART_PRO ;;
		esac
		;;
	Anbernic*)
		PYTHON=$SDCARD_PATH/spruce/flip/bin/MainUI
		PYSDL2_DLL_PATH=$SDCARD_PATH/App/PyUI/dll
		[ -f "$SDCARD_PATH/App/PyUI/dll-mali/libSDL2-2.0.so.0" ] &&
			PYSDL2_DLL_PATH=$SDCARD_PATH/App/PyUI/dll-mali
		LD_LIBRARY_PATH="$PYSDL2_DLL_PATH:$SDCARD_PATH/spruce/flip/lib:/usr/lib"
		SDL_JOYSTICK_DISABLE_UDEV=1
		case "$SPRUCE_PLATFORM" in
			AnbernicXX720480*) NETPLAY_PYUI_DEVICE=ANBERNIC_RGXX720480 ;;
			AnbernicRG28XX) NETPLAY_PYUI_DEVICE=ANBERNIC_RG28XX ;;
			AnbernicRGCubeXX) NETPLAY_PYUI_DEVICE=ANBERNIC_RGCUBEXX ;;
			*) NETPLAY_PYUI_DEVICE=ANBERNIC_RGXX640480 ;;
		esac
		case "$PYSDL2_DLL_PATH" in *dll-mali) SDL_VIDEODRIVER=mali ;; esac
		;;
	Flip)
		RUN_DIR=/usr/miyoo/bin
		PYTHON=$SDCARD_PATH/spruce/flip/bin/MainUI
		PYSDL2_DLL_PATH=$SDCARD_PATH/App/PyUI/dll
		NETPLAY_PYUI_DEVICE=MIYOO_FLIP
		;;
	RGB30|RGB20SX|Miniloong)
		PYTHON=$SDCARD_PATH/spruce/flip/bin/MainUI
		PYSDL2_DLL_PATH=$SDCARD_PATH/App/PyUI/dll
		LD_LIBRARY_PATH="$PYSDL2_DLL_PATH:$SDCARD_PATH/spruce/flip/lib:/usr/lib/aarch64-linux-gnu"
		SDL_VIDEODRIVER=kmsdrm
		SDL_AUDIODRIVER=alsa
		case "$SPRUCE_PLATFORM" in
			Miniloong) NETPLAY_PYUI_DEVICE=MINILOONG_POCKET1 ;;
			*) NETPLAY_PYUI_DEVICE=$SPRUCE_PLATFORM ;;
		esac
		;;
esac
export PYSDL2_DLL_PATH LD_LIBRARY_PATH SDL_JOYSTICK_DISABLE_UDEV SDL_VIDEODRIVER SDL_AUDIODRIVER NETPLAY_PYUI_DEVICE
PYTHONPATH=$DIR
export PYTHONPATH
cd "$RUN_DIR" || cd "$DIR" || exit 1
"$PYTHON" -m spruce_ui >> "$LOG" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
	/mnt/SDCARD/App/PyUI/launch.sh -msgDisplay "Netplay could not start. See Saves/netplay/pyui.log." -msgDisplayTimeMs 5000
fi
exit "$rc"
