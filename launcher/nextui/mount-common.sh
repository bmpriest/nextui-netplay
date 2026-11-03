#!/bin/sh
# Shared bind-mount primitives for system and SD-card emulator paks.
# Callers set MOUNT_STAGE before using ownership or staleness helpers.

NETPLAY_CORES="fceumm snes9x snes9x2005 mednafen_supafaust picodrive pcsx_rearmed gpsp gambatte"
MOUNTS_FILE="${NETPLAY_MOUNTS_FILE:-/proc/mounts}"
MOUNTINFO_FILE="${NETPLAY_MOUNTINFO_FILE:-/proc/self/mountinfo}"

mount_core_is_supported() {
	_wanted="$1"
	for _core in $NETPLAY_CORES; do
		[ "$_wanted" = "$_core" ] && return 0
	done
	return 1
}

# NextUI launchers assign EMU_EXE either directly or inside a case statement.
mount_launch_cores() {
	_found=""
	for _exe in $(sed -n 's/.*EMU_EXE=\([A-Za-z0-9_]*\).*/\1/p' "$1" | sort -u); do
		mount_core_is_supported "$_exe" && _found="$_found $_exe"
	done
	echo "$_found" | sed 's/^ *//'
}

mount_escape_path() {
	printf '%s\n' "$1" | sed 's/\\/\\134/g; s/ /\\040/g; s/\t/\\011/g'
}

mount_source() {
	_target=$(mount_escape_path "$1")
	awk -v target="$_target" '$2 == target { print $1; exit }' "$MOUNTS_FILE" 2>/dev/null
}

mount_root() {
	_target=$(mount_escape_path "$1")
	awk -v target="$_target" '$5 == target { print $4; exit }' "$MOUNTINFO_FILE" 2>/dev/null
}

# Resolve a path to the root it has inside its backing filesystem. Bind mounts
# on exFAT are reported by /proc/mounts as coming from /dev/mmcblk*, which loses
# the source directory and makes exact ownership checks impossible. mountinfo
# retains that directory in field 4. Find the longest containing mount point so
# this also works when the SD card itself is mounted below another filesystem.
mount_backing_root() {
	_path=$(mount_escape_path "$1")
	awk -v path="$_path" '
		function under(p, m) {
			return p == m || (substr(p, 1, length(m) + 1) == m "/")
		}
		under(path, $5) && length($5) > best {
			best = length($5); root = $4; point = $5
		}
		END {
			if (!best) exit 1
			rel = substr(path, length(point) + 1)
			if (root == "/") print rel == "" ? "/" : rel
			else print root rel
		}' "$MOUNTINFO_FILE" 2>/dev/null
}

mount_is_mounted() {
	if [ -r "$MOUNTINFO_FILE" ]; then
		[ -n "$(mount_root "$1")" ]
	else
		[ -n "$(mount_source "$1")" ]
	fi
}

# Ownership is an exact source/target pairing, not merely "something is mounted
# here". This prevents Turn off from unmounting another tool's bind mount.
mount_is_ours() {
	_target="$1"
	_expected="$MOUNT_STAGE/$(basename "$_target")"
	if [ -r "$MOUNTINFO_FILE" ]; then
		_expected_root=$(mount_backing_root "$_expected") || return 1
		[ "$(mount_root "$_target")" = "$_expected_root" ]
	else
		_expected=$(mount_escape_path "$_expected")
		[ "$(mount_source "$_target")" = "$_expected" ]
	fi
}

mount_fingerprint() {
	( cd "$1" && ls -la ) 2>/dev/null
}

mount_needs_resync() {
	_source="$1"
	_staged="$MOUNT_STAGE/$(basename "$_source" .pak).pak"
	[ -d "$_staged" ] || return 0
	[ -f "$_staged/.source-fingerprint" ] || return 0
	[ "$(mount_fingerprint "$_source")" != "$(cat "$_staged/.source-fingerprint")" ] && return 0
	# EXTRAS paks can keep their core inside the pak. `ls -la` rounds mtime to
	# the minute, so a same-size core replacement can keep the same listing.
	# Compare the actual bytes before trusting the staged executable.
	for _core in "$_source"/*_libretro.so; do
		[ -f "$_core" ] || continue
		cmp -s "$_core" "$_staged/$(basename "$_core")" || return 0
	done
	return 1
}

mount_staged_ok() {
	[ -d "$1" ] && [ -f "$1/launch.sh" ] && [ -f "$1/launch.sh.old" ] \
		&& grep -q "$MARKER" "$1/launch.sh" 2>/dev/null
}

# Copy a pak and replace only its staged launcher. The real pak is never edited.
mount_stage_pak() {
	_source="$1"
	_tag=$(basename "$_source" .pak)
	_staged="$MOUNT_STAGE/$_tag.pak"

	rm -rf "$_staged"
	mkdir -p "$_staged"
	cp -a "$_source"/. "$_staged"/ 2>/dev/null || return 1
	mv "$_staged/launch.sh" "$_staged/launch.sh.old" || return 1
	chmod 755 "$_staged/launch.sh.old"

	{
		echo '#!/bin/sh'
		echo "# $MARKER - wraps $_tag, original preserved as launch.sh.old"
		echo 'DIR="$(dirname "$0")"'
		echo "NETPLAY_PAK=\"$NP\""
		echo 'export NETPLAY_PAK'
		echo '[ -f "$NETPLAY_PAK/launcher/pre-launch.sh" ] && . "$NETPLAY_PAK/launcher/pre-launch.sh"'
		echo 'exec "$DIR/launch.sh.old" "$@"'
	} > "$_staged/launch.sh"
	chmod 755 "$_staged/launch.sh"
	mount_fingerprint "$_source" > "$_staged/.source-fingerprint"
}

mount_down_owned() {
	_root="$1"
	_count=0
	for _target in "$_root"/*.pak; do
		[ -d "$_target" ] || continue
		mount_is_ours "$_target" || continue
		if umount "$_target" 2>/dev/null; then
			_count=$((_count + 1))
			log "down $(basename "$_target" .pak)"
		else
			log "could not unmount $(basename "$_target" .pak)"
		fi
	done
	MOUNT_DOWN_COUNT=$_count
}
