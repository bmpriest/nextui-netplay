#!/bin/sh
set -eu

ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT
SD="$ROOT/card"
NP="$SD/Tools/tg5040/Netplay.pak"
STATE="$SD/.userdata/shared/Netplay"
mkdir -p "$NP/launcher"
cp "$(dirname "$0")/../launcher/state-path.sh" "$NP/launcher/"
for p in tg5040 h700 my282; do
	mkdir -p "$SD/Emus/$p" "$NP/cores/mgba/$p/MGBA.pak"
	printf 'new-%s\n' "$p" > "$NP/cores/mgba/$p/MGBA.pak/mgba_libretro.so"
	printf '#!/bin/sh\n' > "$NP/cores/mgba/$p/MGBA.pak/launch.sh"
done
mkdir -p "$SD/Emus/tg5040/MGBA.pak" "$SD/Emus/tg5040/MGBA.pak.bak" \
	"$SD/Saves/MGBA/game" "$SD/.userdata/shared/MGBA-mgba/game"
printf old > "$SD/Emus/tg5040/MGBA.pak/mgba_libretro.so"
printf occupied > "$SD/Emus/tg5040/MGBA.pak.bak/file"
printf save-old > "$SD/Saves/MGBA/game/save.sav"
printf state-old > "$SD/.userdata/shared/MGBA-mgba/game/state.st0"

run() {
	SDCARD_PATH="$SD" PLATFORM=tg5040 NETPLAY_PAK="$NP" NETPLAY_STATE="$STATE" \
		sh "$PWD/launcher/mgba-manage.sh" "$@"
}

echo '== mGBA managed installation'
run install | grep -q '^ok=1$'
test "$(cat "$SD/Emus/tg5040/MGBA.pak/mgba_libretro.so")" = new-tg5040
test "$(cat "$SD/Emus/tg5040/MGBA.pak.bak.1/mgba_libretro.so")" = old
test "$(cat "$STATE/mgba-save-backup"/*/Saves/MGBA/game/save.sav)" = save-old
test -f "$SD/Emus/h700/MGBA.pak/.netplay-installed"
echo '  ok   installs all represented platforms and avoids .bak collision'

printf changed > "$SD/Emus/tg5040/MGBA.pak/mgba_libretro.so"
OUT=$(run status); echo "$OUT" | grep -q '^platform.tg5040=mismatch$'
run ignore >/dev/null
OUT=$(run status); echo "$OUT" | grep -q '^platform.tg5040=ignored$'
printf changed-again > "$SD/Emus/tg5040/MGBA.pak/mgba_libretro.so"
OUT=$(run status); echo "$OUT" | grep -q '^platform.tg5040=mismatch$'
echo '  ok   ignores only one exact installed/expected hash pair'

run install >/dev/null
test "$(cat "$SD/Emus/tg5040/MGBA.pak.bak.1/mgba_libretro.so")" = old
echo '  ok   reinstall preserves the original backup'

printf save-current > "$SD/Saves/MGBA/game/save.sav"
printf state-current > "$SD/.userdata/shared/MGBA-mgba/game/state.st0"
run restore 1 1 1 >/dev/null
test "$(cat "$SD/Emus/tg5040/MGBA.pak/mgba_libretro.so")" = old
test "$(cat "$SD/Saves/MGBA/game/save.sav")" = save-old
test "$(cat "$SD/.userdata/shared/MGBA-mgba/game/state.st0")" = state-old
test "$(cat "$STATE/mgba-restore-collisions"/*/saves/game/save.sav)" = save-current
test "$(cat "$STATE/mgba-restore-collisions"/*/states/game/state.st0)" = state-current
test ! -e "$SD/Emus/h700/MGBA.pak"
echo '  ok   restores originals and preserves overwritten progress'
