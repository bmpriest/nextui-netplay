#!/bin/sh
#
# corecmp batches against spruce's shipped 64-bit cores, run on a spruce
# device from /tmp/corecmp (corecmp, run-pair.sh, the roms and the cores to
# compare staged there; `make corecmp` builds corecmp).
#
#   batch.sh nextui              NextUI's cores (nextui/<core>_libretro.so, from the
#                                NextUI release for this platform) against spruce's
#   batch.sh compat PAIR...      Netplay's compatibility cores (compat/<core>_libretro.so)
#                                against spruce's; PAIR: fceumm-contra fceumm-bubble
#                                picodrive-ga2 picodrive-sor2 supafaust pcsx-N (line N
#                                of pcsx-roms.txt)
#
# Each pair goes to out/<pair> (run-pair.sh's seven runs; compare.py reads
# them), and progress.txt says which pair is running. Detach it (setsid ...
# &): a full batch takes hours.

cd /tmp/corecmp || exit 1
SP=/mnt/SDCARD/RetroArch/.retroarch/cores64
pair() { echo "$(date +%T) $1" >> progress.txt; sh run-pair.sh "out/$1" "$2/$3_libretro.so" "$SP/$3_libretro.so" "$4"; }
mkdir -p out

case ${1:-} in
nextui)
	rm -rf out/* progress.txt
	pair fceumm-contra nextui fceumm             "roms/Contra (USA).nes"
	pair fceumm-bubble nextui fceumm             "roms/Bubble Bobble (USA).nes"
	pair gambatte-gb   nextui gambatte           "roms/Contra - The Alien Wars (USA) (SGB Enhanced).gb"
	pair gambatte-gbc  nextui gambatte           "roms/Aladdin (USA).gbc"
	pair gpsp          nextui gpsp               "roms/Double Dragon Advance (USA).gba"
	pair mgba          nextui mgba               "roms/Double Dragon Advance (USA).gba"
	pair picodrive     nextui picodrive          "roms/Golden Axe II (World).md"
	pair snes9x        nextui snes9x             "roms/Contra III - The Alien Wars (USA).sfc"
	pair supafaust     nextui mednafen_supafaust "roms/Contra III - The Alien Wars (USA).sfc"
	;;
compat)
	shift
	for p in "$@"; do
		case $p in
		fceumm-contra)  pair $p compat fceumm "roms/Contra (USA).nes" ;;
		fceumm-bubble)  pair $p compat fceumm "roms/Bubble Bobble (USA).nes" ;;
		picodrive-ga2)  pair $p compat picodrive "roms/Golden Axe II (World).md" ;;
		picodrive-sor2) pair $p compat picodrive "roms/Streets of Rage 2 (USA).md" ;;
		supafaust)      pair $p compat mednafen_supafaust "roms/Contra III - The Alien Wars (USA).sfc" ;;
		pcsx-*)         pair $p compat pcsx_rearmed "$(sed -n "${p#pcsx-}p" pcsx-roms.txt)" ;;
		*)              echo "unknown pair $p" >&2 ;;
		esac
	done
	;;
*)
	sed -n '3,16p' "$0" >&2; exit 2 ;;
esac
echo "$(date +%T) done" >> progress.txt
