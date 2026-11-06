#!/bin/sh
. "$(dirname "$0")/lib.sh"
for d in b h; do
 echo "DEVICE $d"
 on "$d" 'for p in $(pgrep -f "^/bin/sh /mnt/SDCARD/App/DevBridge/bin/spruce-launch"); do cat /proc/$p/status | head -7; done; echo marker=$(cat /tmp/DevBridge-spruce-ui); pgrep -af "^/bin/sh /mnt/SDCARD/App/DevBridge/bin/spruce-launch"; pgrep -af "^/mnt/SDCARD/spruce/flip/bin/MainUI|^/bin/sh /mnt/SDCARD/App/PyUI/launch.sh"' || :
done
