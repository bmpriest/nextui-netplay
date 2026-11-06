#!/bin/sh
. "$(dirname "$0")/lib.sh"
on h 'p=$(pgrep -f "^/tmp/netplay/nds/dsperate( |$)" | head -1); [ -n "$p" ] || exit 1; echo "PID=$p"; grep SDL /proc/$p/maps; tr "\000" "\n" < /proc/$p/environ | grep -E "^(LD_LIBRARY_PATH|SDL_|PLATFORM|MINUI_)"'
