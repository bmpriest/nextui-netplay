#!/bin/bash
# Cross-compile environment for the toolchain containers, then exec the command.
#
# Referenced as $(DOCKER_SCRIPT) by every `docker run` in the Makefile. Lives in
# the repo (rather than being generated into src/) so it survives `make clean`
# and shows up in review.
set -e

# The toolchain images export their cross-compile env from ~/.bashrc. Tolerate its
# absence so the script stays runnable outside a container (see tests/makefile.bats).
# shellcheck source=/dev/null
[ -f ~/.bashrc ] && . ~/.bashrc

SYSROOT_DIR=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc
export PKG_CONFIG_PATH="$SYSROOT_DIR/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT_DIR"

# The h700 toolchain ships a patched mali-fbdev SDL2 under $PREFIX_LOCAL, and
# that is the build NextUI installs on the device. Prefer it over the TrimUI SDK
# copy in the sysroot. Its .pc file records an absolute in-image prefix, so
# PKG_CONFIG_SYSROOT_DIR has to be cleared for that lookup or pkg-config would
# rewrite every path under the sysroot. Toolchains that leave $PREFIX_LOCAL
# empty (tg5040, tg5050, my355) fall through to the sysroot unchanged.
if [ -f "${PREFIX_LOCAL:-/nonexistent}/lib/pkgconfig/sdl2.pc" ]; then
    # PKG_CONFIG_SYSROOT_DIR= is a deliberate empty prefix assignment, not a typo.
    # shellcheck disable=SC1007
    SDL_CFLAGS="$(PKG_CONFIG_SYSROOT_DIR= PKG_CONFIG_PATH="$PREFIX_LOCAL/lib/pkgconfig" pkg-config --cflags sdl2)"
    # shellcheck disable=SC1007
    SDL_LDLIBS="$(PKG_CONFIG_SYSROOT_DIR= PKG_CONFIG_PATH="$PREFIX_LOCAL/lib/pkgconfig" pkg-config --libs sdl2)"
    echo "docker-env: SDL2 from PREFIX_LOCAL ($PREFIX_LOCAL)" >&2
elif [ "$UNION_PLATFORM" = "h700" ]; then
    # h700 binaries must link the patched SDL2, because that is the one NextUI
    # installs on the device. Silently falling back to the TrimUI SDK copy would
    # still build, so fail loudly instead.
    echo "docker-env: error: h700 toolchain is missing the patched SDL2 at ${PREFIX_LOCAL:-<unset>}/lib/pkgconfig/sdl2.pc" >&2
    exit 1
else
    SDL_CFLAGS="$(pkg-config --cflags sdl2)"
    SDL_LDLIBS="$(pkg-config --libs sdl2)"
    echo "docker-env: SDL2 from sysroot ($SYSROOT_DIR)" >&2
fi
export SDL_CFLAGS SDL_LDLIBS

# ── CMake-facing SDL2 wiring ──────────────────────────────────────────────────
# The block above solves this for autotools consumers, which read $SDL_CFLAGS and
# $SDL_LDLIBS. DSperate is CMake and reads neither, so it needs both halves again
# in a form CMake sees, and there are two separate holes.
#
# Discovery: pkg_check_modules(SDL2 QUIET sdl2) reads the process environment,
# and the block above only narrowed PKG_CONFIG_PATH inside a subshell. Left
# alone, h700 would configure against the sysroot's TrimUI SDK sdl2.pc rather
# than the patched mali-fbdev build NextUI actually installs on the device.
#
# The link, which survives fixing discovery: src/frontend/sdl/CMakeLists.txt
# links ${SDL2_LIBRARIES} and never references ${SDL2_LIBRARY_DIRS}, and there is
# no link_directories() anywhere. pkg_check_modules puts bare names in
# SDL2_LIBRARIES, so the -L is dropped and -lSDL2 resolves through the default
# search path into the sysroot. Fixing PKG_CONFIG_PATH alone would give patched
# headers over an unpatched library, and it would link without complaint.
DSP_SDL_LDFLAGS=""
if [ -f "${PREFIX_LOCAL:-/nonexistent}/lib/pkgconfig/sdl2.pc" ]; then
    # sdl2 is the only pkg-config lookup DSperate performs -- ALSA and libcurl
    # are dlopen'd and miniz/rcheevos are vendored -- so narrowing the whole
    # process to $PREFIX_LOCAL costs nothing. The .pc records an absolute
    # in-image prefix, so PKG_CONFIG_SYSROOT_DIR has to go or pkg-config would
    # rewrite every path under the sysroot.
    export PKG_CONFIG_PATH="$PREFIX_LOCAL/lib/pkgconfig"
    unset PKG_CONFIG_SYSROOT_DIR
    # -L lands ahead of the objects on the link line, so it wins over the
    # sysroot's default search path. -rpath-link lets ld resolve libSDL2's own
    # NEEDED entries without baking a runtime path into the binary.
    DSP_SDL_LDFLAGS="-L$PREFIX_LOCAL/lib -Wl,-rpath-link,$PREFIX_LOCAL/lib"
fi
export DSP_SDL_LDFLAGS

# input.cpp reads SDL_TouchFingerEvent.windowID unguarded, which landed in
# 2.0.12. With DSPERATE_WAYLAND=OFF the 2.0.22 SDL_SysWMinfo::wl.xdg_toplevel
# requirement does not apply, so 2.0.12 is the real floor. Fail here rather than
# at a compile error fifteen minutes into the build.
if ! pkg-config --atleast-version=2.0.12 sdl2; then
    echo "docker-env: error: sdl2 is $(pkg-config --modversion sdl2 2>/dev/null || echo missing), need >= 2.0.12" >&2
    exit 1
fi

exec "$@"
