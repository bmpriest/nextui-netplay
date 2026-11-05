#!/bin/sh
#
# A game invitation end to end: the Brick starts a game, the RG35XXH's menu
# asks, and the RG35XXH answers.
#
#   invite.sh a|b|x [SYS ROM]     default: GB "Pokemon - Blue Version (USA, Europe) (SGB Enhanced).zip"
#
#   a  Play: the RG35XXH starts the same game and joins (its log must say
#      "connected as client")
#   b  Not now: the menu comes back as it was
#   x  Browse: spruce's menu reopens on the game (or the nearest one) in its
#      system's list, several seconds later
#
# Needs an armed session (arm.sh linkplay or retroarch) and the game on the
# Brick. The Brick is put in DevBridge's app (to open the game) and the
# RG35XXH in spruce's game menu: an invitation that arrives in a game or an
# app's menu is dropped. Screenshots: the prompt, and the RG35XXH after the
# answer. Quits the games at the end, so it can run again.

. "$(dirname "$0")/lib.sh"

answer=${1:-}
case $answer in a) btn=A ;; b) btn=B ;; x) btn=X ;; *) sed -n '3,18p' "$0" >&2; exit 2 ;; esac
sys=${2:-GB}
rom=${3:-"Pokemon - Blue Version (USA, Europe) (SGB Enhanced).zip"}
tag=invite-$answer-$(date +%H%M%S)
INVLOG=/mnt/SDCARD/Saves/netplay/invite.log

for d in b h; do on $d "sh $HELPERS/quit.sh retroarch >/dev/null 2>&1"; done
devbridge_open b
spruce_menu h
before=$(on h "wc -l < $INVLOG 2>/dev/null || echo 0")
on h "touch /tmp/netplay-invite-started"
open_game b "$sys" "$rom"
up=
for i in $(seq 1 30); do
	on h 'pidof netplay.elf >/dev/null' && { up=1; break; }
	sleep 0.5
done
[ -n "$up" ] || { echo "no prompt on the RG35XXH within 15 s; its invite.log:"; on h "tail -3 $INVLOG"; exit 1; }
sleep 1
shot h "$tag-prompt"
db exec "h:BTN" "$btn" >/dev/null
sleep 10
shot h "$tag-after"
echo "invite.log:"
on h "tail -n +$((before + 1)) $INVLOG"
if [ $answer = a ]; then
	on h 'f=$(find /mnt/SDCARD/Saves/netplay/logs/netplay-games -name "*.log" -newer /tmp/netplay-invite-started 2>/dev/null | head -1); [ -n "$f" ] && grep -m1 "connected as" "$f" || echo "no game log with a connection yet"'
fi
for d in b h; do on $d "sh $HELPERS/quit.sh retroarch >/dev/null 2>&1"; done
