#!/bin/sh
#
# How long the invitation prompt takes over spruce's menu, on this device:
# pause the menu, show the prompt with ART for 6 s, resume the menu.
#
#   invite-timing.sh [ART]     ART: a box art .qoi or .png (none: text only)
#
# Prints the prompt's own timings (display up, art decoded, first frame) and
# the whole pause; a screenshot during the 6 s shows what it drew. Run from
# spruce's game menu (MainUI running). A resumed menu repaints only on input,
# so it gets invite.sh's nudge.

menu=$(pidof MainUI | awk '{print $1}')
[ -n "$menu" ] || { echo "spruce's menu is not running" >&2; exit 1; }
kill -STOP "$menu"
s=$(cut -d' ' -f1 /proc/uptime)
sh /tmp/netplay-devbridge/prompt.sh --title "Netplay invitation" --text "Pokemon Blue (gambatte)" \
	${1:+--art "$1"} --accept PLAY --decline "NOT NOW" --browse "BROWSE GB" --timeout 6 \
	> /tmp/prompt.out 2> /tmp/prompt.err
e=$(cut -d' ' -f1 /proc/uptime)
kill -CONT "$menu"
sh /tmp/netplay-devbridge/prompt.sh --nudge
echo "menu $menu paused $(echo "$s $e" | awk '{printf "%.2f", $2 - $1}') s, including the 6 s timeout; answer $(cat /tmp/prompt.out)"
grep "prompt:" /tmp/prompt.err
