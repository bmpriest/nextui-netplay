#!/bin/sh
# Builds mGBA's libretro core inside a platform toolchain container, the way
# NextUI 6.15.0 builds it (CMake, LIBMGBA_ONLY, the same per-CPU flags), so the
# core Netplay installs is the one NextUI ships plus our patches.
#
#   build-mgba.sh PLATFORM SOURCE OUTPUT [dual]
#
# "dual" builds the paired frontend (NETPLAY_DUAL_INSTANCE). Each platform and
# kind gets its own build directory inside SOURCE, so nothing is shared
# between cross builds.
set -eu

platform=$1
src=$2
out=$3
kind=${4:-single}

common="-fomit-frame-pointer -ffast-math -fno-common -ftree-vectorize -funswitch-loops"
case $platform in
	tg5040|h700) proc=aarch64; cpu="-mtune=cortex-a53 -mcpu=cortex-a53 -march=armv8-a" ;;
	tg5050|my355) proc=aarch64; cpu="-mcpu=cortex-a55" ;;
	my282) proc=arm; cpu="-marm -mtune=cortex-a7 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard" ;;
	*) echo "build-mgba.sh: no flags for platform '$platform'" >&2; exit 2 ;;
esac

dual=OFF
[ "$kind" = dual ] && dual=ON
build="$src/build-$platform-$kind"

rm -rf "$build"
cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=Release \
	-DLIBMGBA_ONLY=ON -DBUILD_LIBRETRO=ON -DLIBRETRO_STATIC=OFF \
	-DNETPLAY_DUAL_INSTANCE=$dual \
	-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=$proc \
	-DCMAKE_C_COMPILER="${CROSS_COMPILE}gcc" -DCMAKE_CXX_COMPILER="${CROSS_COMPILE}g++" \
	-DCMAKE_C_FLAGS="$common $cpu" -DCMAKE_CXX_FLAGS="$common $cpu" >/dev/null
cmake --build "$build" --target mgba_libretro -- -j"$(nproc 2>/dev/null || echo 4)" >/dev/null
mkdir -p "$(dirname "$out")"
cp "$build/mgba_libretro.so" "$out"
