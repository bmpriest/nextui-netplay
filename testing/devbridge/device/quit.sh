#!/bin/sh
#
# Quit an emulator so its launcher archives the netplay log, whatever it
# takes.
#
#   quit.sh dsp|nds|minarch|retroarch
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
#   retroarch  spruce's RetroArch: its QUIT network command (DevBridge's
#        spruce-ra-command). `minarch` means this too where no MinArch runs,
#        so the same test quits a game on NextUI and on spruce.
# If the emulator is still running 25 s later, SIGTERM, then SIGKILL: DraStic
# on an H700 catches SIGTERM and can hang in its own shutdown. Either way the
# launcher goes on to archive the log. Prints "quit: menu|term|kill"; exits
# non-zero only if the emulator would not die at all.

HERE=$(dirname "$0")
mode=$1
# RetroArch by name (ra64.universal, ra64.h700): Netplay's hook runs the real
# binary from /tmp under the same name. Plain BusyBox regex, no alternation.
ra_pids() { pgrep 'ra[36][24]\.' 2>/dev/null; }
[ "$mode" = minarch ] && ! pidof minarch.elf >/dev/null 2>&1 && ra_pids >/dev/null && mode=retroarch
case $mode in
dsp) proc=dsperate; keys="menu:0.12:0.8 up:0.12:0.4 a:0.12:0" ;;
nds) proc=drastic;  keys="menu:0.2:2 up:0.2:0.7 up:0.2:0.7 a:0.2:0" ;;
minarch) proc=minarch.elf; keys="menu:0.12:0.8 down:0.08:0.25 down:0.08:0.25 down:0.08:0.25 down:0.08:0.25 a:0.12:0" ;;
retroarch) proc=retroarch ;;
*) echo "usage: quit.sh dsp|nds|minarch|retroarch" >&2; exit 2 ;;
esac

alive() {
	if [ "$proc" = retroarch ]; then ra_pids >/dev/null; else pidof "$proc" >/dev/null 2>&1; fi
}

stop() {  # stop [-9]
	if [ "$proc" = retroarch ]; then
		# shellcheck disable=SC2046
		kill $1 $(ra_pids) 2>/dev/null
	else
		killall $1 "$proc" 2>/dev/null
	fi
}

gone_within() {  # gone_within SECONDS
	_i=0
	while alive; do
		[ "$_i" -ge "$1" ] && return 1
		sleep 1; _i=$((_i + 1))
	done
	return 0
}

alive || { echo "quit: $proc was not running"; exit 0; }
if [ "$proc" = retroarch ]; then
	# RetroArch may close its command socket before replying; the exit below
	# is the acknowledgement.
	/mnt/SDCARD/App/DevBridge/bin/spruce-ra-command QUIT >/dev/null 2>&1
else
	# shellcheck disable=SC2086
	sh "$HERE/keys.sh" $keys
fi
gone_within 25 && { echo "quit: menu"; exit 0; }
echo "quit: $proc still running 25 s after its menu's quit; SIGTERM" >&2
stop
gone_within 15 && { echo "quit: term"; exit 0; }
echo "quit: $proc ignored SIGTERM for 15 s; SIGKILL" >&2
stop -9
gone_within 10 && { echo "quit: kill"; exit 0; }
echo "quit: $proc survived SIGKILL" >&2
exit 1
