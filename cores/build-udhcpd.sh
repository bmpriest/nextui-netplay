#!/bin/sh
# Builds BusyBox's udhcpd alone, inside a platform toolchain container, for
# devices whose firmware has no DHCP server: the H700 serves its own ad hoc
# network through wpa_supplicant's AP mode, and its guests need addresses.
#
#   build-udhcpd.sh TARBALL OUTPUT
#
# TARBALL is busybox-<version>.tar, unpacked from the release .tar.bz2 the
# Makefile checks against its SHA-256 (the toolchain image has no bzip2).
# Everything but udhcpd is configured out, so the binary is a BusyBox with
# one applet; it answers to any name, udhcpd included.
set -eu

tarball=$1
out=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

tar -xf "$tarball" -C "$work"
src=$(echo "$work"/busybox-*)
cross=${CROSS_COMPILE:-aarch64-nextui-linux-gnu-}

# BusyBox's build pipes its .config through bzip2 to embed it for
# `busybox --show-config`, which is configured out here; the image has no
# bzip2, and a pass-through stand-in is all that step needs.
mkdir -p "$work/bin"
printf '#!/bin/sh\ncat\n' > "$work/bin/bzip2"
chmod 0755 "$work/bin/bzip2"
PATH="$work/bin:$PATH"

make -s -C "$src" allnoconfig >/dev/null
cfg="$src/.config"
for opt in UDHCPD FEATURE_UDHCPD_WRITE_LEASES_EARLY FEATURE_UDHCPD_BASE_IP_ON_MAC; do
	sed -i "s/^# CONFIG_$opt is not set$/CONFIG_$opt=y/" "$cfg"
done
# Leases go to /tmp; the AP's subnet is fixed by netsetup.c's config file.
sed -i 's|^CONFIG_DHCPD_LEASES_FILE=.*|CONFIG_DHCPD_LEASES_FILE="/tmp/netplay_udhcpd.leases"|' "$cfg"
yes "" | make -s -C "$src" oldconfig >/dev/null
grep -q '^CONFIG_UDHCPD=y' "$cfg" || { echo "build-udhcpd.sh: udhcpd did not stay configured" >&2; exit 1; }

make -s -C "$src" CROSS_COMPILE="$cross" -j"$(nproc)" busybox >/dev/null
"${cross}strip" -o "$out" "$src/busybox"
chmod 0755 "$out"
