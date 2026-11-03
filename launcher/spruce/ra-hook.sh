#!/bin/sh
#
# Puts retroarch.sh in front of spruce's RetroArch, writing nothing to spruce.
#
#   ra-hook.sh up       stash each RetroArch binary, then mount the shadow over it
#   ra-hook.sh down     unmount both
#   ra-hook.sh status
#
# The stash is a bind mount of the real binary in /tmp, made before the shadow
# covers it, so the shadow can still run it. Mounts end at reboot, which is also
# when a session ends; the app runs `up` again whenever it arms one.

. /mnt/SDCARD/App/Netplay/launcher/firmware.sh
NP="$NETPLAY_PAK"
# Physical path: mountinfo records it, and on some devices /mnt/SDCARD is a
# symlink to the real mount (/mnt/sdcard on the RG35XXH).
RA_DIR="$(cd -P "$SDCARD_PATH/RetroArch" 2>/dev/null && pwd)" || RA_DIR="$SDCARD_PATH/RetroArch"
STASH=/tmp/netplay/ra
SHADOW="$NP/launcher/retroarch.sh"
BINS="ra64.universal ra64.h700"

log() { echo "[netplay-ra] $*"; }

mounted() {
	awk -v p="$1" '$5 == p { found = 1 } END { exit !found }' /proc/self/mountinfo
}

do_up() {
	[ -n "$PLATFORM" ] || { log "PLATFORM not set"; return 1; }
	[ -f "$SHADOW" ] || { log "missing $SHADOW"; return 1; }
	mkdir -p "$STASH"
	echo "$PLATFORM" > "$STASH/platform"
	n=0
	for bin in $BINS; do
		target="$RA_DIR/$bin"
		[ -f "$target" ] || continue
		if mounted "$target"; then n=$((n + 1)); continue; fi
		[ -f "$STASH/$bin" ] || : > "$STASH/$bin"
		if ! mounted "$STASH/$bin"; then
			mount --bind "$target" "$STASH/$bin" || { log "cannot stash $bin"; return 1; }
		fi
		if ! mount --bind "$SHADOW" "$target"; then
			umount "$STASH/$bin" 2>/dev/null
			log "cannot cover $bin"
			return 1
		fi
		log "covered $bin"
		n=$((n + 1))
	done
	[ "$n" -gt 0 ] || { log "no RetroArch binary in $RA_DIR"; return 1; }
}

do_down() {
	for bin in $BINS; do
		target="$RA_DIR/$bin"
		mounted "$target" && umount "$target"
		mounted "$STASH/$bin" && umount "$STASH/$bin"
		rm -f "$STASH/$bin"
	done
	rm -f "$STASH/platform"
	return 0
}

do_status() {
	for bin in $BINS; do
		[ -f "$RA_DIR/$bin" ] || continue
		if mounted "$RA_DIR/$bin"; then echo "covered  $bin"; else echo "stock    $bin"; fi
	done
}

case "$1" in
	up)     do_up ;;
	down)   do_down ;;
	status) do_status ;;
	*) echo "usage: ra-hook.sh up|down|status" >&2; exit 2 ;;
esac
