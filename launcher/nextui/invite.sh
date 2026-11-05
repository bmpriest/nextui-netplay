#!/bin/sh
# Invitations between armed NextUI peers. The broker calls receive in a
# detached process; MinArch calls send just before opening the game.

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"
NP=${NETPLAY_PAK:-$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak}
. "$NP/launcher/state-path.sh"
LOG=$NETPLAY_STATE/invite.log
log() { printf '[%s] %s\n' "$(date '+%F %T')" "$*" >> "$LOG"; }

send() {
	rom=$1 core=$2
	case "$rom" in "$SDCARD_PATH/Roms/"*) rel=${rom#"$SDCARD_PATH/Roms/"} ;; *) return 0 ;; esac
	system=${rel%%/*}
	[ "$system" != "$rel" ] || return 0
	name=$(basename "$core" _libretro.so)
	"$NP/bin/$PLATFORM/netplay-broker.elf" --invite "$system" "$rel" "$name" >> "$LOG" 2>&1 &
}

receive() {
	system=$1 rel=$2 core=$3 nick=$4
	case "$rel" in "$system/"*) ;; *) return 0 ;; esac
	case "$rel" in *..*|/*|*'\'*) return 0 ;; esac
	rom=$SDCARD_PATH/Roms/$rel
	game=$(basename "$rom")
	game=${game%.*}
	menu=$(pidof nextui.elf 2>/dev/null | awk '{print $1}')
	[ -n "$menu" ] || { log "dropped $rel from $nick: NextUI menu is unavailable"; return 0; }
	[ "$(awk '/^State:/ {print $2}' "/proc/$menu/status" 2>/dev/null)" != T ] || return 0
	[ -z "$(pidof minarch.elf 2>/dev/null)" ] || { log "dropped $rel from $nick: game is running"; return 0; }
	[ -d /tmp/netplay-invite.lock ] && return 0
	mkdir /tmp/netplay-invite.lock 2>/dev/null || return 0
	trap 'rmdir /tmp/netplay-invite.lock 2>/dev/null' EXIT
	available=0
	[ -f "$rom" ] && available=1
	"$NP/bin/$PLATFORM/netplay-quickmenu.elf" --invite "$nick" "$game" "$available" >> "$LOG" 2>&1
	answer=$?
	[ "$answer" = 0 ] || { log "declined $rel from $nick ($answer)"; return 0; }
	[ -f "$rom" ] || return 0
	tag=$(printf '%s\n' "$system" | sed -n 's/.*(\([^()]*\))[^()]*$/\1/p')
	[ -n "$tag" ] || tag=$system
	launcher=$SDCARD_PATH/Emus/$PLATFORM/$tag.pak/launch.sh
	[ -f "$launcher" ] || launcher=$SDCARD_PATH/.system/$PLATFORM/paks/Emus/$tag.pak/launch.sh
	[ -f "$launcher" ] || { log "no launcher for $tag"; return 0; }
	log "accepted $rel from $nick"
	# The frontend owns the screen while it runs. Pause NextUI's menu so its
	# render loop cannot draw over the accepted game, then resume it on exit.
	SYSTEM_PATH=$SDCARD_PATH/.system/$PLATFORM
	USERDATA_PATH=$SDCARD_PATH/.userdata/$PLATFORM
	SHARED_USERDATA_PATH=$SDCARD_PATH/.userdata/shared
	BIOS_PATH=$SDCARD_PATH/Bios
	SAVES_PATH=$SDCARD_PATH/Saves
	CHEATS_PATH=$SDCARD_PATH/Cheats
	LOGS_PATH=$USERDATA_PATH/logs
	HOME=$USERDATA_PATH
	NETPLAY_INVITED=1
	PATH=$SYSTEM_PATH/bin:$PATH
	LD_LIBRARY_PATH=$SYSTEM_PATH/lib:${LD_LIBRARY_PATH:-}
	export SYSTEM_PATH USERDATA_PATH SHARED_USERDATA_PATH BIOS_PATH SAVES_PATH CHEATS_PATH LOGS_PATH HOME NETPLAY_INVITED PATH LD_LIBRARY_PATH
	kill -STOP "$menu" 2>/dev/null || return 0
	sh "$launcher" "$rom" >> "$LOG" 2>&1
	kill -CONT "$menu" 2>/dev/null || :
}

case "$1" in
	send) send "$2" "$3" ;;
	receive) receive "$2" "$3" "$4" "$5" ;;
	*) exit 2 ;;
esac
