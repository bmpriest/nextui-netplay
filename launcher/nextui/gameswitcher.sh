#!/bin/sh
# Netplay-owned Game Switcher integration.
#
# NextUI backs both Recents and the Game Switcher with one recent.txt file.
# While armed we therefore keep the user's list in shared Netplay userdata and expose only the
# hidden Netplay redirect.  Ending the session restores the list atomically.

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"

NP="${NETPLAY_PAK:-$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak}"
. "$NP/launcher/state-path.sh"
STATE="$NETPLAY_STATE"
SETTINGS="$STATE/settings"
SHARED="$SDCARD_PATH/.userdata/shared"
RECENTS="$SHARED/.minui/recent.txt"
BACKUP="$STATE/gameswitcher-recents.txt"
MISSING="$STATE/gameswitcher-recents.missing"
EMU_DIR="$SDCARD_PATH/Emus/$PLATFORM/NETPLAY.pak"
ROM_DIR="$SDCARD_PATH/Roms/.Netplay (NETPLAY)"
ROM_PATH="$ROM_DIR/Netplay"
RECENT_ENTRY="/Roms/.Netplay (NETPLAY)/Netplay"

enabled() {
	grep -q '^add_gameswitcher=1$' "$SETTINGS" 2>/dev/null
}

atomic_filter() {
	_src=$1
	_dst=$2
	_tmp="$_dst.tmp.$$"
	mkdir -p "$(dirname "$_dst")"
	if [ -f "$_src" ]; then
		awk -F '\t' -v shortcut="$RECENT_ENTRY" '$1 != shortcut' "$_src" > "$_tmp" || {
			rm -f "$_tmp"
			return 1
		}
	else
		: > "$_tmp"
	fi
	mv -f "$_tmp" "$_dst"
}

install_redirect() {
	mkdir -p "$STATE" "$EMU_DIR" "$ROM_DIR" "$(dirname "$RECENTS")"
	cp "$NP/launcher/gameswitcher-launch.sh" "$EMU_DIR/launch.sh" || return 1
	chmod 755 "$EMU_DIR/launch.sh"
	: > "$ROM_PATH"
}

remove_redirect() {
	rm -f "$EMU_DIR/launch.sh" "$ROM_PATH"
	rmdir "$EMU_DIR" "$ROM_DIR" 2>/dev/null || true
}

save_normal_recents() {
	if [ -e "$BACKUP" ] || [ -e "$MISSING" ]; then return 0; fi
	mkdir -p "$STATE"
	if [ -f "$RECENTS" ]; then
		atomic_filter "$RECENTS" "$BACKUP"
	else
		: > "$MISSING"
	fi
}

restore_normal_recents() {
	mkdir -p "$(dirname "$RECENTS")"
	if [ -f "$BACKUP" ]; then
		atomic_filter "$BACKUP" "$RECENTS"
	elif [ -f "$MISSING" ]; then
		rm -f "$RECENTS"
	fi
	rm -f "$BACKUP" "$MISSING"
}

prepend_shortcut() {
	_src=$1
	_dst=$2
	_tmp="$_dst.tmp.$$"
	mkdir -p "$(dirname "$_dst")"
	{
		printf '%s\t%s\n' "$RECENT_ENTRY" "Netplay"
		if [ -f "$_src" ]; then
			awk -F '\t' -v shortcut="$RECENT_ENTRY" '$1 != shortcut' "$_src"
		fi
	} > "$_tmp" || { rm -f "$_tmp"; return 1; }
	# Match NextUI's MAX_RECENTS limit.
	awk 'NR <= 24' "$_tmp" > "$_tmp.limit" && mv -f "$_tmp.limit" "$_tmp"
	mv -f "$_tmp" "$_dst"
}

record_rom() {
	_rom=$1
	[ -n "$_rom" ] || return 0
	case "$_rom" in "$SDCARD_PATH"/*) _rel=${_rom#"$SDCARD_PATH"} ;; *) return 0 ;; esac
	[ -f "$BACKUP" ] || return 0
	_line=$(awk -F '\t' -v rom="$_rel" '$1 == rom { print; exit }' "$RECENTS" 2>/dev/null)
	# Multi-disc launches may hand MinArch a disc while NextUI records the m3u.
	# The just-launched entry is still the first non-shortcut row.
	[ -n "$_line" ] || _line=$(awk -F '\t' -v shortcut="$RECENT_ENTRY" '$1 != shortcut { print; exit }' "$RECENTS" 2>/dev/null)
	[ -n "$_line" ] || return 0
	# POSIX shell parameter expansion cannot spell a literal tab portably when
	# it is absent, so ask awk for the key used by the chosen row.
	_key=$(printf '%s\n' "$_line" | awk -F '\t' '{print $1}')
	_tmp="$BACKUP.tmp.$$"
	{
		printf '%s\n' "$_line"
		awk -F '\t' -v rom="$_key" '$1 != rom' "$BACKUP"
	} | awk 'NR <= 24' > "$_tmp" && mv -f "$_tmp" "$BACKUP"
}

case "$1" in
	enable)
		install_redirect || exit 1
		if [ -f "$STATE/session" ]; then
			"$0" active
		else
			prepend_shortcut "$RECENTS" "$RECENTS"
		fi
		;;
	active)
		enabled || exit 0
		install_redirect || exit 1
		save_normal_recents || exit 1
		record_rom "$2"
		prepend_shortcut /dev/null "$RECENTS"
		;;
	idle)
		restore_normal_recents
		if enabled; then
			install_redirect || exit 1
			prepend_shortcut "$RECENTS" "$RECENTS"
		else
			atomic_filter "$RECENTS" "$RECENTS"
			remove_redirect
		fi
		;;
	disable)
		restore_normal_recents
		atomic_filter "$RECENTS" "$RECENTS"
		remove_redirect
		;;
	*)
		echo "usage: $0 enable|active [rom]|idle|disable" >&2
		exit 2
		;;
esac
