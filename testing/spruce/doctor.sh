#!/bin/sh
. "$(dirname "$0")/lib.sh"
for d in b h; do
    echo "DEVICE $d"
    ON_TIMEOUT=30 on "$d" 'find /mnt/SDCARD/Saves/dsperate -maxdepth 4 -type f; grep -iE "gpsp|Advance_Wars|Advance Wars|dsperate|mariokart|error|failed" /mnt/SDCARD/Saves/spruce/spruce.log | tail -45' || :
    ON_TIMEOUT=30 on "$d" 'pgrep -af "ra64|ra32|drastic|dsperate|MainUI|DevBridge"; ip -4 addr show wlan0; ls /tmp/DevBridge* /tmp/netplay-suite-snapshot 2>/dev/null; find /mnt/SDCARD/Emu/NDS -maxdepth 3 -type f -name "*.ini"; find /mnt/SDCARD/Saves -maxdepth 4 -name "*.log" -o -name "mariokart.ini" -o -iname "*dsperate*"' || :
done
db exec all:STATUS
