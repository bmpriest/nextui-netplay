#!/bin/sh
#
# The alternative the invitation prompt was measured against: spruce's own
# PyUI drawing a message over its paused menu (-msgDisplay). PyUI starts cold
# each time, so this shows what a PyUI-drawn invitation would cost.
#
#   pause-timing.sh
#
# Pauses the menu, shows a 4 s message, resumes. /tmp/stoptest.mark reads
# "shot1" 2.5 s in and "shot2" 1.5 s after resuming: take screenshots then
# to see the message, and whether the menu came back drawn.

P=$(pidof MainUI | awk '{print $1}')
[ -n "$P" ] || { echo "spruce's menu is not running" >&2; exit 1; }
echo "MainUI $P: $(tr '\0' ' ' < /proc/$P/cmdline | cut -c1-160)"
kill -STOP "$P"
s=$(cut -d' ' -f1 /proc/uptime)
/mnt/SDCARD/App/PyUI/launch.sh -msgDisplay "Netplay invitation test" -msgDisplayTimeMs 4000 > /tmp/pyui-msg.log 2>&1 &
L=$!
sleep 2.5; echo shot1 > /tmp/stoptest.mark
wait $L
e=$(cut -d' ' -f1 /proc/uptime)
echo "msgDisplay process: $(echo "$s $e" | awk '{printf "%.2f", $2 - $1}') s, including the 4 s message"
kill -CONT "$P"
echo resumed
sleep 1.5; echo shot2 > /tmp/stoptest.mark
