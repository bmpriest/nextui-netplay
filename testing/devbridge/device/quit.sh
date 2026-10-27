#!/bin/sh
#
# Quit an emulator so its launcher archives the netplay log, whatever it
# takes.
#
#   quit.sh dsp|nds|minarch
#
# First through the emulator's own menu, as a player would:
#   dsp  DSperate's pause menu: a MENU tap opens it, UP wraps to Quit
#   nds  DraStic's menu: MENU, UP (wraps to About), UP (Exit DraStic), A.
#        0.2 s holds: 0.3 s auto-repeats there (two rows a press, and A
#        landed on Restart game), and an H700 dropped 0.15 s ones.
#   minarch  MinArch's menu: MENU, DOWN x4 to Quit, A. DevBridge's QUIT_GAME
#        does the same with remote presses, which a tunnel stretches into
#        auto-repeats, and an older NextUI's menu may differ: hence the
#        fallbacks.
# If the emulator is still running 25 s later, SIGTERM, then SIGKILL: DraStic
# on an H700 catches SIGTERM and can hang in its own shutdown. Either way the
# launcher goes on to archive the log. Prints "quit: menu|term|kill"; exits
# non-zero only if the emulator would not die at all.

HERE=$(dirname "$0")
case $1 in
dsp) proc=dsperate; keys="menu:0.12:0.8 up:0.12:0.4 a:0.12:0" ;;
nds) proc=drastic;  keys="menu:0.2:2 up:0.2:0.7 up:0.2:0.7 a:0.2:0" ;;
minarch) proc=minarch.elf; keys="menu:0.12:0.8 down:0.08:0.25 down:0.08:0.25 down:0.08:0.25 down:0.08:0.25 a:0.12:0" ;;
*) echo "usage: quit.sh dsp|nds|minarch" >&2; exit 2 ;;
esac

gone_within() {  # gone_within SECONDS
	_i=0
	while pidof "$proc" >/dev/null 2>&1; do
		[ "$_i" -ge "$1" ] && return 1
		sleep 1; _i=$((_i + 1))
	done
	return 0
}

pidof "$proc" >/dev/null 2>&1 || { echo "quit: $proc was not running"; exit 0; }
# shellcheck disable=SC2086
sh "$HERE/keys.sh" $keys
gone_within 25 && { echo "quit: menu"; exit 0; }
echo "quit: $proc still running 25 s after its menu's quit; SIGTERM" >&2
killall "$proc" 2>/dev/null
gone_within 15 && { echo "quit: term"; exit 0; }
echo "quit: $proc ignored SIGTERM for 15 s; SIGKILL" >&2
killall -9 "$proc" 2>/dev/null
gone_within 10 && { echo "quit: kill"; exit 0; }
echo "quit: $proc survived SIGKILL" >&2
exit 1
