#!/bin/sh
# Install or update the Varnish.pak bundled with Netplay, which draws the status
# icon and the quick menu.
#
#   varnish-install.sh status    prints one of:
#                                  current      installed, at least the bundled version
#                                  outdated     installed, older than the bundled one
#                                  missing      not installed, and Netplay has one for here
#                                  unavailable  not installed, and no bundle for this platform
#   varnish-install.sh install   install or update it, then enable its startup wiring
#
# The bundle (varnish/<platform>/ in this pak) is Varnish's overlay alone: no
# ffmpeg, so Varnish's screen recording needs the full Varnish.pak. Its files
# are copied over an existing install rather than replacing the folder, so a
# full install keeps its ffmpeg runtime when Netplay updates the rest.
#
# Each file goes in aside and is renamed into place: the daemon and the
# preloaded hook in NextUI keep running from the files they opened, and pick
# up the new ones at the next boot - which an install needs anyway, as the
# hook only loads into NextUI when NextUI starts.

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"

NP="${NETPLAY_PAK:-$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak}"
BUNDLE="$NP/varnish/$PLATFORM"
DEST="$SDCARD_PATH/Tools/$PLATFORM/Varnish.pak"

# "v0.2.0" -> "0 2 0"; missing parts count as 0.
version_of() {
	sed -n 's/.*"version"[^"]*"v\{0,1\}\([0-9.]*\)".*/\1/p' "$1" 2>/dev/null | head -n 1 | tr . ' '
}

# True when version $1 is older than $2 (each as printed by version_of).
older() {
	echo "$1 | $2" | awk -F'|' '{
		n = split($1, a, " "); m = split($2, b, " ")
		k = n > m ? n : m
		for (i = 1; i <= k; i++) {
			x = (i <= n) ? a[i] + 0 : 0; y = (i <= m) ? b[i] + 0 : 0
			if (x < y) exit 0
			if (x > y) exit 1
		}
		exit 1
	}'
}

status() {
	if [ ! -x "$DEST/varnish" ]; then
		[ -x "$BUNDLE/varnish" ] && echo missing || echo unavailable
		return
	fi
	if [ -f "$BUNDLE/pak.json" ] &&
	   older "$(version_of "$DEST/pak.json")" "$(version_of "$BUNDLE/pak.json")"; then
		echo outdated
	else
		echo current
	fi
}

install() {
	[ -x "$BUNDLE/varnish" ] || { echo "No Varnish for $PLATFORM in Netplay." >&2; return 3; }
	mkdir -p "$DEST" || return 1
	(cd "$BUNDLE" && find . -type f) | while IFS= read -r f; do
		f=${f#./}
		mkdir -p "$DEST/$(dirname "$f")" &&
		cp "$BUNDLE/$f" "$DEST/$f.netplay-new" &&
		mv "$DEST/$f.netplay-new" "$DEST/$f" || exit 1
	done || { echo "Could not copy Varnish to $DEST." >&2; return 1; }
	chmod 755 "$DEST/varnish" "$DEST/launch.sh" 2>/dev/null
	sync

	# Enables the startup wiring and the boot hook, and starts the daemon.
	(
		cd "$DEST" || exit 1
		export LD_LIBRARY_PATH="$DEST/lib:${LD_LIBRARY_PATH:-}"
		./varnish --install
	) >/dev/null 2>&1 || { echo "Varnish was copied but could not enable itself." >&2; return 2; }
	sync
}

case "$1" in
	status)  status ;;
	install) install ;;
	*)
		echo "usage: $0 status|install" >&2
		exit 2
		;;
esac
