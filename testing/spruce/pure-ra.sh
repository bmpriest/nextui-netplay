#!/bin/sh
#
# Pure RetroArch netplay between the two spruce devices, set up the way a
# player would: through RetroArch's menus. The spruce Brick hosts, the
# RG35XXH connects. No Netplay session may be armed (arm.sh off), and the
# settings come from ra-config.sh set.
#
#   pure-ra.sh TAG SYS ROM [STAGE...]
#
# Stages, all of them in order by default; each ends on a screenshot to check
# before going on (run one stage at a time while a theme or game is new):
#   host   capture port 55435 on both; the Brick opens the game, then Quick
#          Menu > Main Menu > Netplay > Host
#   start  Start Netplay Host (prints the listener count); the RG35XXH opens
#          the game and spruce's in-game menu > RetroArch Menu
#   ip     Main Menu > Netplay > Connect to Netplay Host: the keyboard
#   type   types the Brick's address on the keyboard
#   enter  Enter; both screens once joined
#   lan    instead of ip/type/enter: Refresh Netplay LAN List ...
#   join   ... and pick the first host. On the RG35XXH this failed with "No
#          core found" for Supafaust, so typing the address is the default.
#
# Then play.sh plays a game's test on top, and collect.sh gathers the run.
# WAIT (environment, default 12) is how long a game gets to start.

. "$(dirname "$0")/lib.sh"

[ $# -ge 3 ] || { sed -n '3,22p' "$0" >&2; exit 2; }
tag=$1 sys=$2 rom=$3; shift 3
[ $# -gt 0 ] || set -- host start ip type enter
W=${WAIT:-12}
DIR=$OUT/$tag

# spruce's RetroArch keyboard, as measured on the RG35XXH: the digits 1..0 in
# columns 0..9 of row 0, "." in column 9 of row 3, Enter in column 10 of row
# 1. The cursor opens on "1". Prints keys.sh steps that type TEXT and Enter.
osk() {
	awk -v s="$1" 'function mv(k, n) { while (n-- > 0) out = out " " k ":0.2:0.35" }
	BEGIN {
		r = 0; c = 0
		for (i = 1; i <= length(s); i++) {
			ch = substr(s, i, 1)
			if (ch == ".") { tr = 3; tc = 9 } else { tr = 0; tc = (ch == "0") ? 9 : ch - 1 }
			if (tr < r) mv("up", r - tr); if (tr > r) mv("down", tr - r)
			if (tc < c) mv("left", c - tc); if (tc > c) mv("right", tc - c)
			out = out " a:0.2:0.5"; r = tr; c = tc
		}
		print out
	}'
}
osk_enter() {
	# From wherever typing left the cursor: up to row 0, across to column 10, down to Enter.
	awk -v s="$1" 'function mv(k, n) { while (n-- > 0) out = out " " k ":0.2:0.35" }
	BEGIN {
		ch = substr(s, length(s), 1)
		if (ch == ".") { r = 3; c = 9 } else { r = 0; c = (ch == "0") ? 9 : ch - 1 }
		mv("up", r); mv("right", 10 - c); mv("down", 1); print out " a:0.2:0.5"
	}'
}

for stage in "$@"; do
	case $stage in
	host)
		devbridge_open b h
		capture_start "$DIR" port 55435
		meta "$DIR" protocol pure-ra
		meta "$DIR" game "$(basename "${rom%.*}")"
		# Latency as ra-config.sh set it (the same on both): MIN-(MIN+RANGE).
		lat=$(on b "cat /mnt/SDCARD/Saves/ra-configs/retroarch-*.cfg" | awk -F'"' '
			/^netplay_input_latency_frames_min / && min == "" { min = $2 }
			/^netplay_input_latency_frames_range / && range == "" { range = $2 }
			END { print min "-" min + range }')
		meta "$DIR" latency "$lat"
		meta "$DIR" label "both $lat"
		meta "$DIR" host_ip "$(dev_ip b)"
		meta "$DIR" guest_ip "$(dev_ip h)"
		open_game b "$sys" "$rom"
		sleep "$W"
		keys b menu:0.25:3 b:0.2:1.2 down:0.2:0.8 down:0.2:0.8 a:0.2:1.2 a:0.2:1.2
		shot b "$tag-hostmenu" ;;
	start)
		keys b a:0.2:4
		echo "brick listening on 55435: $(on b 'netstat -tln | grep -c :55435')"
		open_game h "$sys" "$rom"
		sleep "$W"
		keys h menu:0.25:3 down:0.25:0.8 down:0.25:0.8 down:0.25:0.8 down:0.25:0.8 a:0.25:3
		shot h "$tag-ramenu" ;;
	ip)
		keys h b:0.25:1.5 down:0.25:0.8 down:0.25:0.8 a:0.25:1.5 down:0.25:0.7 a:0.25:1.5
		shot h "$tag-osk" ;;
	type)
		keys h $(osk "$(dev_ip b)")
		shot h "$tag-typed" ;;
	enter)
		keys h $(osk_enter "$(dev_ip b)")
		sleep "$W"
		echo "connections on 55435: $(on b 'netstat -tn | grep -c :55435')"
		shot all "$tag-joined" ;;
	lan)
		keys h b:0.25:1.5 down:0.25:0.8 down:0.25:0.8 a:0.25:1.5 down:0.25:0.7 down:0.25:0.7 down:0.25:0.7 down:0.25:0.7 down:0.25:0.7 a:0.25:6
		shot h "$tag-lan" ;;
	join)
		keys h down:0.25:0.8 a:0.25:0
		sleep "$W"
		shot all "$tag-joined" ;;
	*)
		echo "unknown stage $stage" >&2; exit 2 ;;
	esac
done
