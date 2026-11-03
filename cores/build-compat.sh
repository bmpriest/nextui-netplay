#!/bin/sh
set -eu

arch=${1:?usage: build-compat.sh armv7|aarch64}
root=${CORE_SRC_ROOT:-.cache/cores}
out="dist/compatibility/$arch"
jobs=${JOBS:-4}
start=${COMPAT_START:-}
started=0
[ -z "$start" ] && started=1
mkdir -p "$out"

case "$arch" in
	armv7)
		platform=classic_armv7_a7
		base_flags='-O2 -fPIC -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard'
		base_ldflags='-static-libstdc++ -static-libgcc'
		;;
	aarch64)
		platform=unix
		base_flags='-O2 -fPIC -march=armv8-a -mcpu=cortex-a53'
		base_ldflags='-static-libstdc++ -static-libgcc'
		;;
	*) echo "unsupported compatibility architecture: $arch" >&2; exit 2 ;;
esac

cc="${CROSS_COMPILE}gcc"
cxx="${CROSS_COMPILE}g++"
ar="${CROSS_COMPILE}ar"
strip="${CROSS_COMPILE}strip"

build() {
	name=$1 dir=$2 makefile=$3 platform_name=$4 output=$5
	shift 5
	if [ "$started" -eq 0 ]; then
		if [ "$name" = "$start" ]; then
			started=1
		else
			echo "== compatibility $arch: $name (already built; resuming at $start)"
			return
		fi
	fi
	dir_abs=$(cd "$dir" && pwd)
	echo "== compatibility $arch: $name"
	make -C "$dir" -f "$makefile" PWD="$dir_abs" platform="$platform_name" CC="$cc" CXX="$cxx" AR="$ar" clean >/dev/null 2>&1 || true
	# Not every core's clean removes every object (picodrive keeps
	# pico/sound/mix.o), and an object left from the other architecture
	# breaks the link. Clear untracked build output, keeping the pin stamp.
	git -C "$dir" clean -fdxq -e '.compat-pinned*'
	git -C "$dir" submodule foreach -q --recursive git clean -fdxq
	CFLAGS="$base_flags" CXXFLAGS="$base_flags" LDFLAGS="$base_ldflags" \
		make -C "$dir" -f "$makefile" PWD="$dir_abs" platform="$platform_name" \
		CC="$cc" CXX="$cxx" AR="$ar" -j"$jobs" "$@"
	cp "$dir/$output" "$out/${name}_libretro.so"
	"$strip" --strip-unneeded "$out/${name}_libretro.so"
}

build fceumm "$root/fceumm" Makefile.libretro "$platform" fceumm_libretro.so

# The ARMv7 assembly path computes a different Mega Drive simulation from the
# portable AArch64 path. NO_ARM_ASM on both makes these actual compatibility
# builds rather than merely two binaries with the same version string.
pico_platform=$platform
[ "$arch" = aarch64 ] && pico_platform=aarch64
build picodrive "$root/picodrive" Makefile.libretro "$pico_platform" picodrive_libretro.so NO_ARM_ASM=1

# Full Snes9x 1.63 only reached 46-50 fps on A30, while Snes9x 2005 serializes
# raw native structs and therefore cannot exchange authoritative states between
# 32-bit and 64-bit builds. Supafaust uses Mednafen's explicitly encoded state
# stream and is designed for lower-power ARM devices. Build it once and expose
# that exact binary under both compatibility names. The canonical snes9x name
# lets installed snes9x2005 and snes9x launchers negotiate the same fallback.
build snes9x "$root/mednafen_supafaust" Makefile "$platform" \
	mednafen_supafaust_libretro.so
cp "$out/snes9x_libretro.so" "$out/mednafen_supafaust_libretro.so"

# PSX is intentionally included despite unproven ARMv7/AArch64 determinism.
# Same-architecture Brick testing is valuable, and protocol recovery makes any
# cross-architecture failure visible. Disable the threaded DRC because timing
# between that worker and the emulator thread is itself nondeterministic.
if [ "$arch" = aarch64 ]; then
	build pcsx_rearmed "$root/pcsx_rearmed" Makefile.libretro unix \
		pcsx_rearmed_libretro.so ARCH=arm64 DYNAREC=ari64 BUILTIN_GPU=neon NDRC_THREAD=0
else
	build pcsx_rearmed "$root/pcsx_rearmed" Makefile.libretro "$platform" \
		pcsx_rearmed_libretro.so NDRC_THREAD=0
fi

# The two link-capable cores are already pinned and patched by this pak. Reuse
# those exact architecture builds rather than compiling a second definition of
# "compatibility" through a different rule.
source_platform=tg5040
[ "$arch" = armv7 ] && source_platform=my282
cp "dist/cores/$source_platform/gambatte_libretro.so" "$out/"
cp "dist/cores/$source_platform/gpsp_libretro.so" "$out/"

[ "$started" -eq 1 ] || { echo "unknown COMPAT_START core: $start" >&2; exit 2; }
echo "built seven compatibility cores for $arch"
