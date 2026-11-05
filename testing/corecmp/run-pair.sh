#!/bin/sh
#
# Runs one pair of core builds through corecmp on the device, the seven runs
# compare.py reads:
#
#   run-pair.sh <out dir> <core A> <core B> <rom> [frames] [save at]
#
#   a, a2     A from power-on, twice: is A deterministic on its own?
#   b, b2     the same for B
#   aa        A loading its own state from frame <save at>: the same-build
#             exchange Netplay relies on, as a control
#   bb        B the same
#   ab        A loading B's state
#   ba        B loading A's state
#
# Core options come from $CORECMP_VARS, as for corecmp itself.

set -u
out=$1 core_a=$2 core_b=$3 rom=$4
frames=${5:-3600}
save_at=${6:-1800}
here="$(cd "$(dirname "$0")" && pwd)"
cc="$here/corecmp"
mkdir -p "$out"

run() {
	name=$1; shift
	"$cc" "$@" > "$out/$name.log" 2>&1
	echo "$?" > "$out/$name.rc"
}

run a  "$core_a" "$rom" "$frames" "$out/a.trace" --save-at "$save_at" "$out/a.state"
run a2 "$core_a" "$rom" "$frames" "$out/a2.trace"
run b  "$core_b" "$rom" "$frames" "$out/b.trace" --save-at "$save_at" "$out/b.state"
run b2 "$core_b" "$rom" "$frames" "$out/b2.trace"
run aa "$core_a" "$rom" "$frames" "$out/aa.trace" --load "$out/a.state" "$save_at"
run bb "$core_b" "$rom" "$frames" "$out/bb.trace" --load "$out/b.state" "$save_at"
run ab "$core_a" "$rom" "$frames" "$out/ab.trace" --load "$out/b.state" "$save_at"
run ba "$core_b" "$rom" "$frames" "$out/ba.trace" --load "$out/a.state" "$save_at"
printf '%s\n%s\n%s\n%s %s\n' "$core_a" "$core_b" "$rom" "$frames" "$save_at" > "$out/pair.txt"
