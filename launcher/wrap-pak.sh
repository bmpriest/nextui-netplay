#!/bin/sh
#
# Covers emulator paks that already own their SD path, without writing to them.
#
#   wrap-pak.sh sync     stage a copy of each wrappable pak under our own dir
#   wrap-pak.sh up       bind-mount the staged copies over the originals
#   wrap-pak.sh down     unmount
#   wrap-pak.sh status
#
# install-stubs.sh handles paks under $SYSTEM_PATH by dropping an override at
# the free SD path. An EXTRAS pak occupies that path itself, so there is nothing
# to override. Rather than modify a pak we do not own, we stage a copy:
#
#   $NP/wrapped/<TAG>.pak/
#   |-- launch.sh       ours - puts the shim on PATH, then runs launch.sh.old
#   |-- launch.sh.old   the original, verbatim
#   `-- ...             everything else the original contained
#
# and bind-mount that directory over the original. The original bytes on disk
# are never touched, and `down` (or a reboot) restores everything.
#
# Why the copy has to include the pak's other files: the mount replaces the
# whole directory, and paks like FBN and SUPA bundle their core and locate it
# with CORES_PATH=$(dirname "$0"). Under the mount that resolves into our staged
# directory, so the core has to be there. Paks that use the system cores dir
# stage as just a couple of shell scripts.
#
# Scoped per pak on purpose. If something is wrong, one system is affected
# rather than every game launch.

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"
: "${SYSTEM_PATH:=$SDCARD_PATH/.system/$PLATFORM}"

NP="$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak"
STAGE="$NP/wrapped"
MOUNT_STAGE="$STAGE"
SD_EMUS="$SDCARD_PATH/Emus/$PLATFORM"
MARKER="Installed by Netplay.pak"

log() { echo "[netplay-wrap] $*"; }

[ -f "$NP/launcher/mount-common.sh" ] || { log "missing mount-common.sh"; exit 1; }
. "$NP/launcher/mount-common.sh"

# Paks on the SD path that we could wrap: real pak, supported core, not already
# one of our own stubs.
wrappable() {
	for _pak in "$SD_EMUS"/*.pak; do
		[ -d "$_pak" ] || continue
		_launch="$_pak/launch.sh"
		[ -f "$_launch" ] || continue
		grep -q "$MARKER" "$_launch" 2>/dev/null && continue
		mount_is_mounted "$_pak" && { echo "$_pak"; continue; }

		[ -n "$(mount_launch_cores "$_launch")" ] && echo "$_pak"
	done
}

do_sync() {
	# Staging reads the originals, so nothing may be shadowing them.
	for _pak in $(wrappable); do
		if mount_is_mounted "$_pak"; then
			log "refusing to sync while mounted - run 'down' first"
			return 1
		fi
	done

	mkdir -p "$STAGE"
	_n=0
	for _pak in $(wrappable); do
		_tag="$(basename "$_pak" .pak)"
		_dst="$STAGE/$_tag.pak"

		mount_stage_pak "$_pak" || { log "copy failed for $_tag"; continue; }

		_n=$((_n + 1))
		log "staged $_tag ($(du -sh "$_dst" 2>/dev/null | cut -f1))"
	done
	log "$_n pak(s) staged"
}

do_up() {
	command -v mount >/dev/null 2>&1 || { log "no mount available"; return 1; }

	_n=0
	_owned=0
	_fail=0
	for _pak in $(wrappable); do
		_tag="$(basename "$_pak" .pak)"
		_dst="$STAGE/$_tag.pak"

		if mount_is_mounted "$_pak"; then
			if mount_is_ours "$_pak"; then
				_owned=$((_owned + 1)); log "$_tag already up"
			else
				_fail=$((_fail + 1)); log "$_tag mounted by another tool - leaving it alone"
			fi
			continue
		fi

		# Not mounted, so the original is what we read: restage it here. A pak
		# that changed on disk (a core update) used to be skipped, and with no
		# pak mounted the whole session fell back to launch stubs.
		if [ ! -d "$_dst" ] || mount_needs_resync "$_pak"; then
			if mount_stage_pak "$_pak"; then
				log "staged $_tag"
			else
				log "copy failed for $_tag"
				_fail=$((_fail + 1))
				continue
			fi
		fi

		if mount --bind "$_dst" "$_pak"; then
			_n=$((_n + 1))
			log "up $_tag"
		else
			log "could not mount $_tag (needs root?)"
			_fail=$((_fail + 1))
		fi
	done
	[ "$_fail" -gt 0 ] && log "$_n pak(s) mounted, $_fail not covered" || log "$_n pak(s) mounted"
	# No extra paks is not a failure; bind-mount.sh judges its own set.
	[ "$_fail" -eq 0 ]
}

do_down() {
	mount_down_owned "$SD_EMUS"
	_n=$MOUNT_DOWN_COUNT
	log "$_n pak(s) unmounted"
}

do_status() {
	for _pak in $(wrappable); do
		_tag="$(basename "$_pak" .pak)"
		if mount_is_mounted "$_pak"; then
			log "up      $_tag"
		elif [ -d "$STAGE/$_tag.pak" ]; then
			mount_needs_resync "$_pak" && log "stale   $_tag" || log "staged  $_tag"
		else
			log "unstaged $_tag"
		fi
	done
}

do_activate() {
	for _pak in "$SD_EMUS"/*.pak; do
		[ -d "$_pak" ] || continue
		if mount_is_ours "$_pak"; then
			do_up
			return $?
		fi
	done
	do_sync && do_up
}

case "$1" in
	sync)   do_sync ;;
	up)     do_activate ;;
	down)   do_down ;;
	status) do_status ;;
	*)      echo "usage: $0 sync|up|down|status" >&2; exit 2 ;;
esac
