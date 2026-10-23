#!/bin/sh
set -eu
ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT
PAK="$ROOT/Emus/tg5040/MGBA.pak"
NP="$ROOT/Tools/tg5040/Netplay.pak"
BIN="$ROOT/bin"
mkdir -p "$PAK" "$NP/launcher" "$BIN" "$ROOT/bios" "$ROOT/saves" "$ROOT/cheats" "$ROOT/logs" "$ROOT/userdata"
cp testing/MGBA.pak/launch.sh "$PAK/launch.sh"
: > "$PAK/mgba_libretro.so"
printf '%s\n' 'PATH="$TEST_MINARCH_DIR:$PATH"' 'export PATH' > "$NP/launcher/pre-launch.sh"
printf '%s\n' '#!/bin/sh' 'printf "core=%s rom=%s\n" "$1" "$2"' > "$BIN/minarch.elf"
chmod +x "$BIN/minarch.elf" "$PAK/launch.sh"
TEST_MINARCH_DIR="$BIN"; export TEST_MINARCH_DIR
SDCARD_PATH="$ROOT" PLATFORM=tg5040 BIOS_PATH="$ROOT/bios" SAVES_PATH="$ROOT/saves" \
	CHEATS_PATH="$ROOT/cheats" LOGS_PATH="$ROOT/logs" USERDATA_PATH="$ROOT/userdata" \
	"$PAK/launch.sh" "$ROOT/game.gba"
grep -q "core=$PAK/mgba_libretro.so rom=$ROOT/game.gba" "$ROOT/logs/MGBA.txt"
test ! -e "$PAK/mgba_dual_libretro.so"
echo 'ok - bundled MGBA.pak owns only the ordinary core and enters Netplay pre-launch'
