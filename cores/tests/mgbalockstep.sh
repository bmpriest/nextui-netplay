#!/bin/sh
# Determinism harness for mGBA's GBA lockstep cable.
#
# The paired mGBA core will hold two consoles and the cable between them, and
# two handhelds must each reproduce that pair identically from the same inputs -
# so the cable's answers are emulated state, and have to be a pure function of
# it. This drives two mCores and one coordinator through a fixed input script
# with the same cooperative scheduler the paired core will use, and asserts that
# repeated runs hash identically.
#
# Needs the pinned mGBA source. `make cores-mgba` (or the $(MGBA_STAMP) rule)
# fetches it; without it this skips rather than failing, matching run.sh.
set -e
cd "$(dirname "$0")/../.."

CORE=.cache/cores/mgba
if [ ! -f "$CORE/src/gba/sio/lockstep.c" ]; then
	echo "== mGBA lockstep determinism"
	echo "  SKIP mGBA source not fetched (run 'make cores-mgba')"
	exit 0
fi

echo "== mGBA lockstep determinism"

CC="${CC:-gcc}"
HOSTDIR=.cache/cores/.mgba-host
LIB="$HOSTDIR/libmgba-host.a"
FLAGFILE="$HOSTDIR/cflags"

# Build the core for the host once and keep it. The harness must be compiled
# with the library's *exact* defines: struct mCore has members behind
# ENABLE_VFS / ENABLE_DIRECTORIES / MINIMAL_CORE, so a harness built with a
# different set sees different field offsets and calls a garbage function
# pointer. Capturing the flags from the build rather than restating them here is
# what keeps that from rotting.
if [ ! -f "$LIB" ] || [ "$CORE/src/gba/sio/lockstep.c" -nt "$LIB" ]; then
	mkdir -p "$HOSTDIR"
	echo "  .... building mGBA for the host (once; cached in $HOSTDIR)"
	( cd "$CORE" && make -f Makefile.libretro platform=unix clean >/dev/null 2>&1 || true )
	( cd "$CORE" && make -f Makefile.libretro platform=unix -j"$(nproc 2>/dev/null || echo 4)" ) \
		> "$HOSTDIR/build.log" 2>&1 || {
			echo "  MISS mGBA host build failed"; tail -20 "$HOSTDIR/build.log"; exit 1; }

	# Everything except the libretro entry points: that translation unit owns a
	# single global mCore, which is the assumption the paired core has to shed.
	find "$CORE" -name '*.o' | grep -v 'platform/libretro/libretro.o' > "$HOSTDIR/objs"
	ar rcs "$LIB" $(cat "$HOSTDIR/objs")

	grep -m1 'core/core\.c' "$HOSTDIR/build.log" \
		| tr ' ' '\n' | grep -E '^-D' | grep -v GIT_VERSION | tr '\n' ' ' > "$FLAGFILE"

	# Leave the tree neutral so the next cross build does not link host objects.
	( cd "$CORE" && make -f Makefile.libretro platform=unix clean >/dev/null 2>&1 || true )
fi

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

INC="-I$CORE/src -I$CORE/src/arm -I$CORE/include -I$CORE/src/platform/libretro"
$CC -O1 -g -o "$OUT/mgbalockstep" cores/tests/mgbalockstep.c "$LIB" \
	$(cat "$FLAGFILE") $INC -lm

RUNS="${MGBA_LOCKSTEP_RUNS:-4}"
FRAMES="${MGBA_LOCKSTEP_FRAMES:-120}"

if "$OUT/mgbalockstep" "$RUNS" "$FRAMES" > "$OUT/log" 2>&1; then
	steps=$(sed -n 's/.*steps=\([0-9]*\).*/\1/p' "$OUT/log" | head -1)
	sleeps=$(sed -n 's/.*sleeps=\([0-9]*\).*/\1/p' "$OUT/log" | head -1)
	transfers=$(sed -n 's/.*transfers=\([0-9]*\).*/\1/p' "$OUT/log" | head -1)
	echo "  ok   $RUNS runs of $FRAMES frames hashed identically on both consoles"
	echo "  ok   scheduler ran $steps slices and the cable slept $sleeps times"
	echo "  ok   completed $transfers multiplayer transfers after attach"
	fatal=$(sed -n 's/.*fatal=\([0-9]*\)(.*/\1/p' "$OUT/log" | head -1)
	if [ "${fatal:-0}" -gt 0 ]; then
		echo "  note $fatal FATAL lines were explicitly allowed for diagnostics"
	fi
else
	if grep -q "FATAL lines" "$OUT/log"; then
		echo "  MISS mGBA reports the pair desynchronized (reproducibly)"
	else
		echo "  MISS the paired run was not reproducible"
	fi
	cat "$OUT/log"
	exit 1
fi
