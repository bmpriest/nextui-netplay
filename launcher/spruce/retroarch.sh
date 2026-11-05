#!/bin/sh
#
# Not RetroArch. ra-hook.sh bind-mounts this over spruce's RetroArch binary
# (RetroArch/ra64.universal, ra64.h700), the way nextui/minarch.elf stands in for
# MinArch: spruce's launcher runs `<RA_BIN> ... -L <core.so> <rom>`, and for an
# armed session this runs the same command with the shim in the core's place.
#
# The real binary is still reachable at the stash ra-hook.sh bound it to. With
# no session armed, or a core the shim does not drive, that binary is exec'd
# with the arguments we were given, byte for byte.

SDCARD_PATH=/mnt/SDCARD
. "$SDCARD_PATH/App/Netplay/launcher/firmware.sh"
NP="$NETPLAY_PAK"

STASH=/tmp/netplay/ra
REAL_RA="$STASH/$(basename "$0")"
# Anything unexpected -> behave exactly like stock. Without the stash there is
# nothing to run, which only happens if this file is executed by hand.
[ -x "$REAL_RA" ] || { echo "[netplay] no real RetroArch at $REAL_RA" >&2; exit 127; }

# spruce's PLATFORM names the device for spruce's own scripts, and RetroArch's
# children may read it; Netplay's scripts want its build platform. Both are kept
# and each command gets the one it expects.
SPRUCE_PLATFORM="$PLATFORM"
PLATFORM="$(cat "$STASH/platform" 2>/dev/null)"
[ -n "$PLATFORM" ] || exec "$REAL_RA" "$@"

. "$NP/launcher/state-path.sh"
if [ -z "$NETPLAY_SESSION" ] && [ -f "$NETPLAY_STATE/session" ]; then
	NETPLAY_SESSION="$NETPLAY_STATE/session"
	export NETPLAY_SESSION
fi
if [ -z "$NETPLAY_SESSION" ] &&
   [ "$NETPLAY_FORCE_SHIM" != "1" ] &&
   [ ! -f "$NETPLAY_STATE/force-shim" ]; then
	PLATFORM="$SPRUCE_PLATFORM" exec "$REAL_RA" "$@"
fi

# The core and the rom. spruce always passes `-L <core>` before the rom, and the
# rom is the last argument.
CORE=
prev=
for a in "$@"; do
	[ "$prev" = "-L" ] && CORE="$a"
	prev="$a"
	ROM="$a"
done
case "$(basename "$CORE")" in
	fceumm_libretro.so|snes9x_libretro.so|snes9x2005_libretro.so|\
	mednafen_supafaust_libretro.so|picodrive_libretro.so|pcsx_rearmed_libretro.so|\
	gpsp_libretro.so|gambatte_libretro.so|mgba_libretro.so) ;;
	*) PLATFORM="$SPRUCE_PLATFORM" exec "$REAL_RA" "$@" ;;
esac

SHIM="$NP/bin/$PLATFORM/netplay_shim.so"
if [ ! -f "$SHIM" ]; then
	echo "[netplay] shim missing at $SHIM, launching stock" >&2
	PLATFORM="$SPRUCE_PLATFORM" exec "$REAL_RA" "$@"
fi

# Rejoin the ad hoc network, Wi-Fi power save: as for every NextUI pak.
. "$NP/launcher/pre-launch.sh"

. "$NP/launcher/core-route.sh"
np_route_core "$CORE"
np_stage_shim || PLATFORM="$SPRUCE_PLATFORM" exec "$REAL_RA" "$@"

# The same command line with the shim as the core.
count=$#
prev=
for a in "$@"; do
	if [ "$prev" = "-L" ]; then set -- "$@" "$SHIM_AS"; else set -- "$@" "$a"; fi
	prev="$a"
done
shift "$count"

# RetroArch's settings for the session (retroarch-session.cfg): no save states,
# rewind, run-ahead or fast-forward, and nothing written back to spruce's config
# on exit. RetroArch keeps one --appendconfig, a |-separated list, so ours joins
# spruce's when it passed one.
SESSION_CFG="$NP/launcher/retroarch-session.cfg"
if [ -f "$SESSION_CFG" ]; then
	count=$#
	prev=
	appended=0
	for a in "$@"; do
		if [ "$prev" = "--appendconfig" ]; then
			set -- "$@" "$a|$SESSION_CFG"
			appended=1
		else
			set -- "$@" "$a"
		fi
		prev="$a"
	done
	shift "$count"
	[ "$appended" = 1 ] || set -- --appendconfig "$SESSION_CFG" "$@"
fi

# Offer the game to the other device; its menu asks the player (invite.sh).
# Detached: the launch does not wait on the network.
sh "$NP/launcher/invite.sh" send "$ROM" "$CORE"

# spruce stops a game by signalling RetroArch by name (this shell carries the
# same name), so pass a stop on rather than dying before RetroArch has saved and
# the shim has put the CPU back.
child=
trap '[ -n "$child" ] && kill -TERM "$child" 2>/dev/null' TERM INT HUP
np_frontend() {
	PLATFORM="$SPRUCE_PLATFORM" "$REAL_RA" "$@" &
	child=$!
	wait "$child"
	rc=$?
	# A trapped signal ends the wait early; RetroArch is still shutting down.
	while kill -0 "$child" 2>/dev/null; do
		wait "$child"
		rc=$?
	done
	child=
	return "$rc"
}

np_run_session "$ROM" "$@"
