#!/bin/sh
. "$(dirname "$0")/lib.sh"
tar -cf - -C "$ROOT/testing/devbridge/device" input-abs-probe.py | on h "tar -xf - -C $HELPERS"
on h 'LD_LIBRARY_PATH=/mnt/SDCARD/spruce/flip/lib /mnt/SDCARD/spruce/flip/bin/MainUI /tmp/netplay-devbridge/input-abs-probe.py'
