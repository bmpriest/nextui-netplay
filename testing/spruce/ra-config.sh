#!/bin/sh
#
# spruce's RetroArch netplay settings for the pure RetroArch runs, and back.
#
#   ra-config.sh set [MIN RANGE [CHECK]]   back up every Saves/ra-configs/retroarch-*.cfg
#                                          on both devices, then set input latency
#                                          MIN..MIN+RANGE frames and the host's state
#                                          check every CHECK frames (default 1 2 600:
#                                          latency 1-3, a check every 10 s)
#   ra-config.sh restore                   put the backups back
#   ra-config.sh show
#
# Each device's own values differ (on the rig: the Brick latency 1-3 with a
# check every 3600 frames, the RG35XXH 0 and 600). The comparisons use the
# same on both: 1-3, and a check every 10 s so desyncs are measured.

. "$(dirname "$0")/lib.sh"

CFGS=/mnt/SDCARD/Saves/ra-configs
show='grep -H -E "^netplay_(input_latency_frames_min|input_latency_frames_range|check_frames) " '$CFGS'/retroarch-*.cfg'

case ${1:-} in
set)
	min=${2:-1} range=${3:-2} check=${4:-600}
	for d in b h; do
		on $d "for f in $CFGS/retroarch-*.cfg; do [ -f \"\$f.netplay-bak\" ] || cp \"\$f\" \"\$f.netplay-bak\"; done
			sed -i -e 's/^netplay_input_latency_frames_min = .*/netplay_input_latency_frames_min = \"$min\"/' \
			       -e 's/^netplay_input_latency_frames_range = .*/netplay_input_latency_frames_range = \"$range\"/' \
			       -e 's/^netplay_check_frames = .*/netplay_check_frames = \"$check\"/' $CFGS/retroarch-*.cfg
			$show" | sed "s/^/$d: /"
	done
	;;
restore)
	for d in b h; do
		on $d "for f in $CFGS/retroarch-*.cfg.netplay-bak; do [ -f \"\$f\" ] && cp \"\$f\" \"\${f%.netplay-bak}\" && rm \"\$f\"; done; sync; $show" | sed "s/^/$d: /"
	done
	;;
show)
	for d in b h; do on $d "$show" | sed "s/^/$d: /"; done ;;
*)
	sed -n '3,13p' "$0" >&2; exit 2 ;;
esac
