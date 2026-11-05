#!/bin/bash
# Assert the configure produced the features we require.
#
# Usage: assert-build.sh <build-dir>
#
# An interpreter-only DSperate still builds, links, boots and plays -- just far
# too slowly -- and upstream downgrades JIT and NEON with message(STATUS), not a
# fatal error. Nothing in a device log would say why.
#
# Not CMakeCache.txt: upstream disables both with a plain set(), not
# set(... CACHE ... FORCE), so the cache still reads ON after a downgrade and
# the NEON path prints nothing at all. The generated ninja graph is the ground
# truth -- src/core/CMakeLists.txt emits the compile definitions only when the
# feature really survived.
set -euo pipefail

BUILD_DIR="$1"
NINJA="$BUILD_DIR/build.ninja"
LOG="$BUILD_DIR/configure.log"
rc=0
die() {
    echo "assert-build: FAIL: $*" >&2
    rc=1
}

if [ ! -f "$NINJA" ]; then
    echo "assert-build: no $NINJA" >&2
    exit 1
fi

grep -q -- '-DDSPERATE_JIT=1' "$NINJA" || die "recompiler is off -- this would ship an interpreter-only build"
grep -q -- '-DDSPERATE_NEON=1' "$NINJA" || die "NEON renderer kernels are off"
grep -q 'cpu/jit/a64/' "$NINJA" || die "no AArch64 JIT objects in the build graph"

grep -q 'DSPERATE_JIT_A32=1' "$NINJA" && die "the ARM32 JIT backend was selected on an AArch64 target"

# display_wl.cpp is the exact file src/frontend/sdl/CMakeLists.txt adds for the
# Wayland dmabuf tier. A fuzzy 'wayland|dmabuf' grep would match the surrounding
# comments and pass no matter what.
grep -q 'display_wl\.cpp' "$NINJA" && die "the Wayland dmabuf tier is in the build graph despite DSPERATE_WAYLAND=OFF"

if [ -f "$LOG" ]; then
    grep -q 'JIT disabled' "$LOG" && die "configure said: $(grep 'JIT disabled' "$LOG")"
    grep -q 'SDL2 not found' "$LOG" && die "configure said: $(grep 'SDL2 not found' "$LOG")"
fi

# On h700 the patched mali-fbdev SDL2 must be the one linked. A wrong-SDL2 link
# is invisible to readelf -- same libSDL2-2.0.so.0 SONAME either way -- so the
# recorded link command is the only place it shows. 'ninja -t commands' reprints
# without rebuilding, so this is free.
if [ -n "${PREFIX_LOCAL:-}" ] && [ -f "$PREFIX_LOCAL/lib/pkgconfig/sdl2.pc" ]; then
    ninja -C "$BUILD_DIR" -t commands dsperate | grep -q -- "-L$PREFIX_LOCAL/lib" ||
        die "dsperate was linked without -L$PREFIX_LOCAL/lib; it would bind the sysroot SDL2"
fi

[ -x "$BUILD_DIR/out/dsperate" ] || die "no dsperate binary at $BUILD_DIR/out/dsperate"

exit $rc
