#!/bin/sh
. "$(dirname "$0")/lib.sh"
on h 'cat /mnt/SDCARD/App/DevBridge/bin/input; sed -n "1,60p" /mnt/SDCARD/App/DevBridge/bin/spruce-input; printf "event_ms="; cat /tmp/netplay-devbridge/event-ms'
