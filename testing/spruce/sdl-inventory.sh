#!/bin/sh
. "$(dirname "$0")/lib.sh"
tar -cf - -C "$ROOT/testing/devbridge/device" sdl-input-probe.py | on h "tar -xf - -C $HELPERS"
on h '. /mnt/SDCARD/spruce/scripts/helperFunctions.sh >/dev/null 2>&1; export_sdl_gamecontroller_map positional; export LD_LIBRARY_PATH=/mnt/SDCARD/App/PyUI/dll-mali:/mnt/SDCARD/spruce/flip/lib; /mnt/SDCARD/spruce/flip/bin/MainUI /tmp/netplay-devbridge/sdl-input-probe.py --probe > /tmp/netplay-sdl-probe.log 2>&1 < /dev/null &'
sleep 1
on h '/mnt/SDCARD/App/DevBridge/bin/input tap down 200; sleep 0.5; /mnt/SDCARD/App/DevBridge/bin/input tap up 200'
sleep 8
on h 'cat /tmp/netplay-sdl-probe.log'
