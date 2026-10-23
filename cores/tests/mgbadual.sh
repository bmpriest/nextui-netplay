#!/bin/sh
set -e
cd "$(dirname "$0")/../.."

CORE=.cache/cores/mgba/mgba_libretro.so
if [ ! -f "$CORE" ]; then
	echo "== mGBA dual libretro ABI"
	echo "  SKIP paired host core not built"
	exit 0
fi

echo "== mGBA dual libretro ABI"
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
