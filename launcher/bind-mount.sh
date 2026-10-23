#!/bin/sh
#
# Covers the built-in emulator paks by bind mount, writing nothing to the SD
# card's Emus tree.
#
#   bind-mount.sh sync     stage a copy of each covered pak under our own dir
#   bind-mount.sh up       mount the staged copies over the originals
#   bind-mount.sh down     unmount and clean up
#   bind-mount.sh boot     what auto.sh calls - restore mounts after a reboot
#   bind-mount.sh status
#   bind-mount.sh hook-install / hook-remove
#
# WHAT THIS REPLACES
#
# install-stubs.sh created $SDCARD_PATH/Emus/<PLATFORM>/<TAG>.pak/launch.sh for
# every covered system - seven new directories in a tree the user did not ask us
# to touch, which persist until something removes them and which look exactly
# like paks the user installed. This leaves that tree completely alone.
#
# WHERE THE MOUNT GOES, AND WHY THERE
#
# Over $SYSTEM_PATH/paks/Emus/<TAG>.pak - the original's own directory.
#
# Not over the SD path: it does not exist, and creating it is the thing we are
# removing.
#
# Not over the original launch.sh as a file, though file bind mounts do work on
# these kernels. Shadowing launch.sh hides the original *at its own path*, so
# our replacement has no way to reach it, and reaching a stashed copy instead
# breaks $0 - which is where EMU_TAG comes from:
#
#     EMU_TAG=$(basename "$(dirname "$0")" .pak)
#
# Mounting the directory keeps $0 at the pak's real path, so EMU_TAG, and
# anything else derived from it, stays correct with no tricks.
#
#   $NP/mounts/<TAG>.pak/
#   |-- launch.sh       ours - preamble, then runs launch.sh.old
#   |-- launch.sh.old   the original, verbatim
#   `-- ...             everything else the original contained
#
# The original bytes on disk are never modified. `down`, or a reboot, restores
# everything exactly - which is also why this has to run at boot.
#
# Scoped per pak on purpose: if something is wrong, one system is affected
# rather than every game launch.

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"
: "${SYSTEM_PATH:=$SDCARD_PATH/.system/$PLATFORM}"
: "${USERDATA_PATH:=$SDCARD_PATH/.userdata/$PLATFORM}"

NP="$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak"
. "$NP/launcher/state-path.sh"
STAGE="$NP/mounts"
MOUNT_STAGE="$STAGE"
MANIFEST="$NETPLAY_STATE/mounts.list"
SYS_EMUS="$SYSTEM_PATH/paks/Emus"
AUTO="$USERDATA_PATH/auto.sh"

MARKER="Installed by Netplay.pak"
HOOK_TAG="Netplay.pak-on-boot"

log() { echo "[netplay-mount] $*"; }

[ -f "$NP/launcher/mount-common.sh" ] || { log "missing mount-common.sh"; exit 1; }
. "$NP/launcher/mount-common.sh"

# A system pak we can cover: real pak, has a launch.sh, selects a core the shim
# knows how to drive.
#
# Note this deliberately reports paks that are currently mounted too - our
# launch.sh keeps the original's EMU_EXE line in launch.sh.old, but the mounted
# launch.sh does not carry it, so testing the mounted file would make every
# covered pak vanish from the list the moment it came up.
coverable() {
	for _pak in "$SYS_EMUS"/*.pak; do
		[ -d "$_pak" ] || continue
		[ -f "$_pak/launch.sh" ] || continue

		if mount_is_mounted "$_pak"; then echo "$_pak"; continue; fi
		[ -n "$(mount_launch_cores "$_pak/launch.sh")" ] && echo "$_pak"
	done
}

###########################################################

do_sync() {
	# Staging reads the originals, so nothing may be shadowing them - and an
	# rm -rf through a live mount would delete the real pak.
	for _pak in $(coverable); do
		if mount_is_mounted "$_pak"; then
			log "refusing to sync while mounted - run 'down' first"
			return 1
		fi
	done

	mkdir -p "$STAGE"
	_n=0
	for _pak in $(coverable); do
		_tag="$(basename "$_pak" .pak)"
		_dst="$STAGE/$_tag.pak"

		mount_stage_pak "$_pak" || { log "copy failed for $_tag"; continue; }

		_n=$((_n + 1))
		log "staged $_tag ($(mount_launch_cores "$_dst/launch.sh.old"))"
	done
	log "$_n pak(s) staged"
}

# Refuse to mount anything we are not certain about. A bad mount does not fail
# visibly - it makes one system stop launching games, which is worse than not
# covering it.
do_up() {
	command -v mount >/dev/null 2>&1 || { log "no mount available"; return 1; }

	# Legacy stubs win over anything we mount: getEmuPath checks the SD card
	# first, so a leftover Emus/<PLATFORM>/<TAG>.pak would shadow the mount and
	# we would wrap twice. Clearing them is also the migration.
	if [ -f "$NETPLAY_STATE/stubs.list" ] && [ -x "$NP/launcher/install-stubs.sh" ]; then
		log "removing legacy SD stubs (superseded by mounts)"
		SDCARD_PATH="$SDCARD_PATH" PLATFORM="$PLATFORM" SYSTEM_PATH="$SYSTEM_PATH" \
			"$NP/launcher/install-stubs.sh" uninstall
	fi

	mkdir -p "$(dirname "$MANIFEST")"
	_n=0
	_owned=0
	_fail=0
	for _pak in $(coverable); do
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

		if ! mount_staged_ok "$_dst"; then
			log "$_tag not staged - run 'sync' first"
			_fail=$((_fail + 1))
			continue
		fi
		if mount_needs_resync "$_pak"; then
			log "$_tag changed on disk - restaging"
			rm -rf "$_dst"
			do_sync >/dev/null 2>&1
			mount_staged_ok "$_dst" || { log "$_tag restage failed - skipping"; _fail=$((_fail + 1)); continue; }
		fi

		if mount --bind "$_dst" "$_pak" 2>/dev/null; then
			echo "$_pak" >> "$MANIFEST.new"
			_n=$((_n + 1))
			log "up $_tag"
		else
			log "could not mount $_tag (needs root?)"
			_fail=$((_fail + 1))
		fi
	done

	[ -f "$MANIFEST.new" ] && mv "$MANIFEST.new" "$MANIFEST" || : > "$MANIFEST"
	[ "$_fail" -gt 0 ] && log "$_n pak(s) mounted, $_fail not covered" || log "$_n pak(s) mounted"

	# Cover EXTRAS paks that own their SD path too, so one command means "all
	# netplay-capable systems are wrapped" rather than "some of them are".
	if [ -x "$NP/launcher/wrap-pak.sh" ] && ! "$NP/launcher/wrap-pak.sh" up; then
		log "one or more extra paks could not be covered"
		_fail=$((_fail + 1))
	fi

	# Arming is all-or-nothing. A partial success used to report success and
	# silently leave one system launching without the shim. The caller can now
	# roll this set back and choose the launch-stub fallback consistently.
	[ "$_fail" -eq 0 ] && [ $((_n + _owned)) -gt 0 ] && return 0
	return 1
}

do_down() {
	mount_down_owned "$SYS_EMUS"
	_n=$MOUNT_DOWN_COUNT
	rm -f "$MANIFEST"
	log "$_n pak(s) unmounted"

	[ -x "$NP/launcher/wrap-pak.sh" ] && "$NP/launcher/wrap-pak.sh" down

	# Any legacy stubs go too, so "off" means off by either mechanism.
	if [ -f "$NETPLAY_STATE/stubs.list" ] && [ -x "$NP/launcher/install-stubs.sh" ]; then
		SDCARD_PATH="$SDCARD_PATH" PLATFORM="$PLATFORM" SYSTEM_PATH="$SYSTEM_PATH" \
			"$NP/launcher/install-stubs.sh" uninstall
	fi
}

# Called from auto.sh. Mounts do not survive a reboot, so without this every
# covered system silently reverts to stock on the next power cycle.
#
# Deliberately quiet and deliberately unable to stop a boot: staging may be
# stale or missing after a NextUI update, and the correct response to that is an
# unwrapped system, never a device that will not start.
do_boot() {
	[ ! -x "$NP/launcher/session-cleanup.sh" ] || \
		"$NP/launcher/session-cleanup.sh" >/dev/null 2>&1 || :
	[ -d "$STAGE" ] || exit 0
	do_up >/dev/null 2>&1
	exit 0
}

# `up` is safe to repeat. Once our mounts are live the originals are shadowed,
# so syncing is both unnecessary and unsafe; mount only any missing targets.
do_activate() {
	for _pak in "$SYS_EMUS"/*.pak; do
		[ -d "$_pak" ] || continue
		if mount_is_ours "$_pak"; then
			do_up && do_hook_install
			return $?
		fi
	done
	do_sync && do_up && do_hook_install
}

###########################################################
# Boot hook.
#
# auto.sh rather than the boot.d hook directory: run_hooks.sh exists on tg5040
# but not on my282, whereas auto.sh is present on both and is already how
# Syncthing.pak and SSH Server.pak register. One mechanism that works
# everywhere beats two that each work somewhere.

hook_line() {
	echo "test -x \"\$SDCARD_PATH/Tools/\$PLATFORM/Netplay.pak/launcher/bind-mount.sh\" && \"\$SDCARD_PATH/Tools/\$PLATFORM/Netplay.pak/launcher/bind-mount.sh\" boot # $HOOK_TAG"
}

do_hook_install() {
	if [ -f "$AUTO" ] && grep -q "$HOOK_TAG" "$AUTO" 2>/dev/null; then
		log "boot hook already present"
		return 0
	fi
	if [ ! -f "$AUTO" ]; then
		mkdir -p "$(dirname "$AUTO")"
		echo "#!/bin/sh" > "$AUTO"
		echo "" >> "$AUTO"
	fi
	hook_line >> "$AUTO"
	chmod 755 "$AUTO"
	log "boot hook installed in $AUTO"
}

do_hook_remove() {
	[ -f "$AUTO" ] || return 0
	grep -q "$HOOK_TAG" "$AUTO" 2>/dev/null || return 0
	grep -v "$HOOK_TAG" "$AUTO" > "$AUTO.tmp" && mv "$AUTO.tmp" "$AUTO"
	chmod 755 "$AUTO"
	log "boot hook removed"
}

do_status() {
	grep -q "$HOOK_TAG" "$AUTO" 2>/dev/null \
		&& log "boot hook: installed" || log "boot hook: absent"
	for _pak in $(coverable); do
		_tag="$(basename "$_pak" .pak)"
		if mount_is_mounted "$_pak"; then
			log "up       $_tag"
		elif [ -d "$STAGE/$_tag.pak" ]; then
			mount_needs_resync "$_pak" && log "stale    $_tag" || log "staged   $_tag"
		else
			log "unstaged $_tag"
		fi
	done
}

case "$1" in
	sync)        do_sync ;;
	up)          do_activate ;;
	down)        do_down; do_hook_remove ;;
	boot)        do_boot ;;
	status)      do_status ;;
	hook-install) do_hook_install ;;
	hook-remove)  do_hook_remove ;;
	*) echo "usage: $0 sync|up|down|boot|status|hook-install|hook-remove" >&2; exit 2 ;;
esac
