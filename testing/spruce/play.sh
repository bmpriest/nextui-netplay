#!/bin/sh
#
# Play a game's DevBridge test on games already running on both spruce
# devices (pure-ra.sh or launch.sh started them): the test's menus, its
# random play and its screenshots, without its launch, log checks or quit.
#
#   play.sh TEST TAG      e.g. play.sh sf2 lp-sf2   (testing/devbridge/sf2.test)
#
# The Brick is the test's host (Player 1), the RG35XXH its guest. Reference
# checks that miss are reported and the test goes on (--keep-going): under
# pure RetroArch a title can have rolled into its demo while the menus were
# set up, and the screenshots show where it landed.

. "$(dirname "$0")/lib.sh"

[ $# -eq 2 ] || { sed -n '3,11p' "$0" >&2; exit 2; }
test=$ROOT/testing/devbridge/${1%.test}.test
[ -f "$test" ] || { echo "no $test" >&2; exit 2; }
DEVBRIDGE=$DEVBRIDGE "$ROOT/testing/devbridge/run.sh" --play-only --user spruce \
	--port "$TUNNEL_PORT" "adb:$BRICK_ADB" "$TUNNEL_ADDR" "$test" > "$OUT/$2/play.log" 2>&1
result=$?
cat "$OUT/$2/play.log"
exit "$result"
