#!/bin/sh
set -e
cd "$(dirname "$0")/../.."

SRC=.cache/cores/mgba
HOSTDIR=.cache/cores/.mgba-host-dual
CORE=$HOSTDIR/build/mgba_libretro.so
if [ ! -f "$SRC/src/platform/libretro/libretro_dual.c" ]; then
	echo "== mGBA dual libretro ABI"
	echo "  SKIP mGBA source not fetched (run 'make cores-mgba')"
	exit 0
fi

echo "== mGBA dual libretro ABI"
if [ ! -f "$CORE" ] || [ "$SRC/src/platform/libretro/libretro_dual.c" -nt "$CORE" ]; then
	echo "  .... building the paired core for the host (once; cached in $HOSTDIR)"
	rm -rf "$HOSTDIR/build"; mkdir -p "$HOSTDIR"
	{ cmake -S "$SRC" -B "$HOSTDIR/build" -DCMAKE_BUILD_TYPE=Release \
		-DLIBMGBA_ONLY=ON -DBUILD_LIBRETRO=ON -DLIBRETRO_STATIC=OFF -DNETPLAY_DUAL_INSTANCE=ON &&
	  cmake --build "$HOSTDIR/build" --target mgba_libretro -- -j"$(nproc 2>/dev/null || echo 4)"; } \
		> "$HOSTDIR/build.log" 2>&1 || {
			echo "  MISS paired host build failed"; tail -20 "$HOSTDIR/build.log"; exit 1; }
fi
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
${CC:-cc} -std=c99 -O2 -Wall -Wextra \
	-I.cache/cores/mgba/src/platform/libretro \
	-o "$OUT/mgbadual" cores/tests/mgbadual.c -ldl

if ! "$OUT/mgbadual" "$CORE" > "$OUT/log.1" 2>&1; then
	cat "$OUT/log.1"
	exit 1
fi
if ! "$OUT/mgbadual" "$CORE" > "$OUT/log.2" 2>&1; then
	cat "$OUT/log.2"
	exit 1
fi
if [ "$(sed -n 's/.*state=\([0-9a-f]*\).*/\1/p' "$OUT/log.1")" != \
	 "$(sed -n 's/.*state=\([0-9a-f]*\).*/\1/p' "$OUT/log.2")" ]; then
	echo "  MISS independent paired-core processes produced different states"
	diff -u "$OUT/log.1" "$OUT/log.2" || true
	exit 1
fi
if grep -q 'Multiplayer desynchronized' "$OUT/log.1" "$OUT/log.2"; then
	echo "  MISS mGBA asserted that the paired scheduler desynchronized"
	cat "$OUT/log.1"
	exit 1
fi
cat "$OUT/log.1"
echo "  ok   independent processes produced the same paired state hash"
