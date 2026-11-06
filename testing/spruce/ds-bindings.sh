#!/bin/sh
. "$(dirname "$0")/lib.sh"
on h 'for f in /mnt/SDCARD/Saves/dsperate/no-sticks.ini /mnt/SDCARD/Saves/dsperate/two-sticks.ini /mnt/SDCARD/Saves/dsperate/games/mariokart.ini; do echo "$f"; cat "$f"; done'
