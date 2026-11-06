#!/bin/sh
. "$(dirname "$0")/lib.sh"
for d in b h; do
    echo "DEVICE $d"
    on "$d" 'pgrep -af "drastic|dsperate|ra64|ra32|retroarch|Emu/NDS"; for name in dsperate drastic drastic64 ra64.universal ra64.h700 ra32.universal; do pgrep -x "$name" | while read p; do printf "pid=%s " "$p"; tr "\000" " " < /proc/$p/cmdline; echo; done; done; cat /tmp/DevBridge-spruce-ui 2>/dev/null' || :
done
db exec all:STATUS
