#!/bin/sh
set -eu

ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT
SD="$ROOT/card"
NP="$SD/Tools/tg5040/Netplay.pak"
STATE="$SD/.userdata/shared/Netplay"
mkdir -p "$NP/launcher"
cp "$(dirname "$0")/../launcher/state-path.sh" "$NP/launcher/"
# DSP.pak keeps one binary per platform directory inside the pak.
for p in tg5040 h700; do
	mkdir -p "$SD/Emus/$p" "$NP/cores/dsp/$p/DSP.pak/$p"
	printf 'new-%s\n' "$p" > "$NP/cores/dsp/$p/DSP.pak/$p/dsperate"
	printf '#!/bin/sh\n' > "$NP/cores/dsp/$p/DSP.pak/launch.sh"
done
# my282 has an Emus directory but no DSP.pak build: it must be left alone.
mkdir -p "$SD/Emus/my282"
mkdir -p "$SD/Emus/tg5040/DSP.pak/tg5040" "$SD/Saves/DSP" "$SD/.userdata/shared/DSP-dsperate"
printf old > "$SD/Emus/tg5040/DSP.pak/tg5040/dsperate"
printf save-old > "$SD/Saves/DSP/game.sav"
printf state-old > "$SD/.userdata/shared/DSP-dsperate/game.0.dss"

run() {
	SDCARD_PATH="$SD" PLATFORM=tg5040 NETPLAY_PAK="$NP" NETPLAY_STATE="$STATE" \
		sh "$PWD/launcher/pak-manage.sh" dsp "$@"
}

echo '== DSP.pak managed installation'
OUT=$(run status); echo "$OUT" | grep -q '^platform.tg5040=mismatch$'
echo "$OUT" | grep -q '^platform.h700=missing$'
! echo "$OUT" | grep -q 'platform.my282'
echo '  ok   status hashes each platform binary and skips platforms without a build'

run install | grep -q '^ok=1$'
test "$(cat "$SD/Emus/tg5040/DSP.pak/tg5040/dsperate")" = new-tg5040
test "$(cat "$SD/Emus/h700/DSP.pak/h700/dsperate")" = new-h700
test "$(cat "$SD/Emus/tg5040/DSP.pak.bak/tg5040/dsperate")" = old
grep -qx netplay-dsp-v1 "$SD/Emus/h700/DSP.pak/.netplay-installed"
test "$(cat "$STATE/dsp-save-backup"/*/Saves/DSP/game.sav)" = save-old
test "$(cat "$STATE/dsp-save-backup"/*/States/DSP-dsperate/game.0.dss)" = state-old
test -d "$SD/Roms/Nintendo DS (DSP)"
test ! -e "$SD/Emus/my282/DSP.pak"
echo '  ok   installs each built platform, backs up the old pak, saves and states'

OUT=$(run status); echo "$OUT" | grep -q '^mismatch=0$'
echo "$OUT" | grep -q '^managed=1$'
echo '  ok   a fresh install reports managed and matching'

printf save-current > "$SD/Saves/DSP/game.sav"
run restore 1 1 0 >/dev/null
test "$(cat "$SD/Emus/tg5040/DSP.pak/tg5040/dsperate")" = old
test ! -e "$SD/Emus/h700/DSP.pak"
test "$(cat "$SD/Saves/DSP/game.sav")" = save-old
test "$(cat "$STATE/dsp-restore-collisions"/*/saves/game.sav)" = save-current
echo '  ok   restores the original pak and saves, keeping overwritten progress'

# mGBA's bookkeeping is a separate namespace.
test ! -e "$STATE/mgba"
echo '  ok   DSP and mGBA bookkeeping do not overlap'
