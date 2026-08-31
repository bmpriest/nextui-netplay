#!/bin/sh

PAK_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
RUNTIME_DIR="$PAK_DIR/drastic"
ROM_PATH=${1:-}

EMU_TAG=$(basename "$PAK_DIR" .pak)
LOG_DIR=${LOGS_PATH:-/tmp}
RUN_ID=$(date +%Y%m%d-%H%M%S)
LOG_FILE="$LOG_DIR/$EMU_TAG-$RUN_ID.txt"
LATEST_LOG="$LOG_DIR/$EMU_TAG.txt"
WATCHDOG_SECONDS=${NDS_WATCHDOG_SECONDS:-0}
EMU_PID=""
BACKLIGHT_PATH=/sys/devices/virtual/disp/disp/attr/lcdbl
BACKLIGHT_HANDOFF=/tmp/nextui-backlight
FRAMEBUFFER_BLANK=/sys/class/graphics/fb0/blank
PID_FILE=/tmp/nextui-nds.pid
HARD_QUIT_FILE=/tmp/nextui-nds-hard-quit

configure_nextui_shoulders() {
	cfg="$RUNTIME_DIR/res/nds.cfg"
	[ -f "$cfg" ] || return 0
	[ "$(wc -c < "$cfg" 2>/dev/null)" = 460 ] || {
		echo "launcher: skipped shoulder migration for unexpected nds.cfg size"
		return 1
	}
	# NextUI reports L1/R1 as 15/14 and L2/R2 as 18/20. The upstream A30
	# layer expects the inverse ordering, represented by these config flags.
	printf '\001\000\000\000' | dd of="$cfg" bs=1 seek=4 conv=notrunc 2>/dev/null
	printf '\001\000\000\000' | dd of="$cfg" bs=1 seek=8 conv=notrunc 2>/dev/null
	echo "launcher: configured NextUI shoulder ordering"
}

unblank_framebuffer() {
	if [ -w "$FRAMEBUFFER_BLANK" ]; then
		printf 0 > "$FRAMEBUFFER_BLANK"
		echo "launcher: unblanked A30 framebuffer"
	else
		echo "launcher: framebuffer blanking control is not writable"
		return 1
	fi
}

restore_backlight_handoff() {
	[ -r "$BACKLIGHT_HANDOFF" ] || return 0
	raw=$(sed -n '1p' "$BACKLIGHT_HANDOFF" 2>/dev/null)
	case "$raw" in
		''|*[!0-9]*)
			echo "launcher: invalid backlight handoff value: $raw"
			return 1
			;;
	esac
	if [ -w "$BACKLIGHT_PATH" ]; then
		echo "$raw" > "$BACKLIGHT_PATH"
		rm -f "$BACKLIGHT_HANDOFF"
		echo "launcher: restored A30 backlight raw=$raw"
	else
		echo "launcher: backlight control is not writable"
		return 1
	fi
}

stop_emulator() {
	if [ -n "$EMU_PID" ] && kill -0 "$EMU_PID" 2>/dev/null; then
		kill -TERM "$EMU_PID" 2>/dev/null
		grace=3
		while kill -0 "$EMU_PID" 2>/dev/null && [ "$grace" -gt 0 ]; do
			sleep 1
			grace=$((grace - 1))
		done
		if kill -0 "$EMU_PID" 2>/dev/null; then
			kill -KILL "$EMU_PID" 2>/dev/null
		fi
	fi
}

finish_run() {
	stop_emulator
	rm -f /tmp/nextui-nds-running "$PID_FILE" "$HARD_QUIT_FILE"
	for cpu in 1 2 3; do
		online="/sys/devices/system/cpu/cpu$cpu/online"
		if [ -w "$online" ] && [ "$(sed -n '1p' "$online" 2>/dev/null)" != 1 ]; then
			echo 1 > "$online"
		fi
	done
	cp "$LOG_FILE" "$LATEST_LOG" 2>/dev/null || true
	sync
}

trap 'finish_run; exit 130' HUP INT TERM

mkdir -p "$LOG_DIR"
if ( : > "$LOG_FILE" ) 2>/dev/null; then
	exec > "$LOG_FILE" 2>&1
else
	exec > "/tmp/nextui-$EMU_TAG.txt" 2>&1
fi

if [ -z "$ROM_PATH" ]; then
	echo "launcher: no ROM path supplied"
	exit 2
fi

if [ ! -x "$RUNTIME_DIR/drastic" ]; then
	echo "launcher: missing $RUNTIME_DIR/drastic"
	exit 1
fi

export SDL_VIDEODRIVER=NDS
unset NDS_DEBUG_LOG

cd "$RUNTIME_DIR" || exit 1
echo "launcher: system=$EMU_TAG emulator=drastic-2.5.0.4 platform=my282 rom=$ROM_PATH"
echo "launcher: backlight-handoff=1 watchdog_seconds=$WATCHDOG_SECONDS run_id=$RUN_ID"
echo "launcher: framebuffer_virtual=$(sed -n '1p' /sys/class/graphics/fb0/virtual_size 2>/dev/null)"
configure_nextui_shoulders
unblank_framebuffer
restore_backlight_handoff
touch /tmp/nextui-nds-running
rm -f "$PID_FILE" "$HARD_QUIT_FILE"

if [ -f "$RUNTIME_DIR/libdrastic_wifi.so" ]; then
	WIFI_TRACE="$LOG_DIR/$EMU_TAG-wifi-$RUN_ID.txt"
	WIFI_PRELOAD="$RUNTIME_DIR/libdrastic_wifi.so"
	WIFI_MODE=passthrough
	WIFI_CYCLE=0
	WIFI_IRQ=0
	WIFI_SESSION="/mnt/SDCARD/Tools/$PLATFORM/Netplay.pak/state/session"
	[ ! -f "$RUNTIME_DIR/wifi_model.enabled" ] || WIFI_MODE=model
	[ ! -f "$RUNTIME_DIR/wifi_cycle.enabled" ] || WIFI_CYCLE=1
	[ "$WIFI_MODE" != model ] || WIFI_IRQ=1
	WIFI_HOOK=$WIFI_MODE
	[ "$WIFI_HOOK" != model ] || WIFI_HOOK=hybrid
	echo "launcher: wifi-hook=${WIFI_HOOK}16 cycle=$WIFI_CYCLE trace=$WIFI_TRACE"
fi

if [ -n "$WIFI_PRELOAD" ]; then
	DRASTIC_WIFI_MODE="$WIFI_MODE" DRASTIC_WIFI_CYCLE="$WIFI_CYCLE" \
		DRASTIC_WIFI_IRQ="$WIFI_IRQ" DRASTIC_WIFI_SESSION="$WIFI_SESSION" \
		DRASTIC_WIFI_TRACE="$WIFI_TRACE" LD_PRELOAD="$WIFI_PRELOAD" \
		./lib/ld-linux-armhf.so.3 --library-path ./lib ./drastic "$ROM_PATH" &
else
	./lib/ld-linux-armhf.so.3 --library-path ./lib ./drastic "$ROM_PATH" &
fi
EMU_PID=$!
printf '%s\n' "$EMU_PID" > "$PID_FILE"

# Mali-fbdev may change blanking state while creating its native EGL window.
# Assert scanout again after that transition without delaying emulation startup.
(sleep 2; unblank_framebuffer) &

elapsed=0
while kill -0 "$EMU_PID" 2>/dev/null; do
	if [ -e "$HARD_QUIT_FILE" ]; then
		echo "hard-quit: request received; stopping pid=$EMU_PID"
		rm -f "$HARD_QUIT_FILE"
		stop_emulator
		break
	fi
	if [ "$WATCHDOG_SECONDS" -gt 0 ] && [ "$elapsed" -ge "$WATCHDOG_SECONDS" ]; then
		echo "watchdog: timeout after $elapsed seconds; stopping pid=$EMU_PID"
		stop_emulator
		break
	fi
	sleep 1
	elapsed=$((elapsed + 1))
done

wait "$EMU_PID" 2>/dev/null
status=$?
EMU_PID=""
echo "launcher: emulator exited status=$status elapsed=$elapsed"
finish_run
exit "$status"
