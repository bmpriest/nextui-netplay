#!/bin/sh
set -eu

ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT
SD="$ROOT/card"
NP="$SD/Tools/tg5040/Netplay.pak"
STATE="$SD/.userdata/shared/Netplay"
mkdir -p "$NP/launcher"
cp "$(dirname "$0")/../launcher/state-path.sh" "$NP/launcher/"
# Netplay ships an overlay, not a pak: the radio and a launcher, plus the hash
# of the one DraStic build the radio's hook supports.
for p in tg5040 tg5050; do
	mkdir -p "$SD/Emus/$p" "$NP/cores/nds/$p/NDS.pak/drastic/libs"
	printf 'radio-%s\n' "$p" > "$NP/cores/nds/$p/NDS.pak/drastic/libs/libdrastic_wifi.so"
	printf '#!/bin/sh\n# netplay\n' > "$NP/cores/nds/$p/NDS.pak/launch.sh"
	printf 'supported-drastic\n' > "$ROOT/drastic.bin"
	sha256sum "$ROOT/drastic.bin" | awk '{print $1}' > "$NP/cores/nds/$p/drastic.sha256"
done
# tg5040: the user's NDS.pak with the supported DraStic and their own settings.
mkdir -p "$SD/Emus/tg5040/NDS.pak/drastic/config" "$SD/Emus/tg5040/NDS.pak/drastic/libs"
cp "$ROOT/drastic.bin" "$SD/Emus/tg5040/NDS.pak/drastic/drastic"
printf '#!/bin/sh\n# upstream\n' > "$SD/Emus/tg5040/NDS.pak/launch.sh"
printf 'mine\n' > "$SD/Emus/tg5040/NDS.pak/drastic/config/drastic.cfg"

run() {
	SDCARD_PATH="$SD" PLATFORM=tg5040 NETPLAY_PAK="$NP" NETPLAY_STATE="$STATE" \
		sh "$PWD/launcher/pak-manage.sh" nds "$@"
}

echo '== NDS.pak local wireless overlay'
OUT=$(run status)
echo "$OUT" | grep -q '^platform.tg5040=missing$'
echo "$OUT" | grep -q '^platform.tg5050=no-base$'
echo "$OUT" | grep -q '^base_installed=1$'
echo '  ok   status needs the user'"'"'s NDS.pak and reports the overlay missing'

run install | grep -q '^ok=1$'
grep -q '^# netplay$' "$SD/Emus/tg5040/NDS.pak/launch.sh"
test "$(cat "$SD/Emus/tg5040/NDS.pak/drastic/libs/libdrastic_wifi.so")" = radio-tg5040
test "$(cat "$SD/Emus/tg5040/NDS.pak/drastic/config/drastic.cfg")" = mine
grep -qx netplay-nds-v1 "$SD/Emus/tg5040/NDS.pak/.netplay-installed"
test ! -e "$SD/Emus/tg5050/NDS.pak"
echo '  ok   adds the radio and launcher, keeps the user'"'"'s settings, skips a card without NDS.pak'

OUT=$(run status); echo "$OUT" | grep -q '^platform.tg5040=ok$'
echo "$OUT" | grep -q '^managed=1$'
echo "$OUT" | grep -q '^mismatch=0$'
echo '  ok   a fresh overlay reports managed and matching'

run reinstall | grep -q '^ok=1$'
run restore 1 0 0 >/dev/null
grep -q '^# upstream$' "$SD/Emus/tg5040/NDS.pak/launch.sh"
test ! -e "$SD/Emus/tg5040/NDS.pak/drastic/libs/libdrastic_wifi.so"
test ! -e "$SD/Emus/tg5040/NDS.pak/.netplay-installed"
test "$(cat "$SD/Emus/tg5040/NDS.pak/drastic/config/drastic.cfg")" = mine
echo '  ok   restore puts the original launcher back (even after a reinstall) and removes what it added'

printf 'other-drastic\n' > "$SD/Emus/tg5040/NDS.pak/drastic/drastic"
OUT=$(run status); echo "$OUT" | grep -q '^platform.tg5040=unsupported$'
echo "$OUT" | grep -q '^unsupported=1$'
! run install >/dev/null 2>&1
grep -q '^# upstream$' "$SD/Emus/tg5040/NDS.pak/launch.sh"
echo '  ok   another DraStic build is reported unsupported and left untouched'

# A card without NDS.pak: Netplay installs the bundled release, then the overlay.
printf 'supported-drastic\n' > "$SD/Emus/tg5040/NDS.pak/drastic/drastic"
mkdir -p "$NP/cores/nds/base/NDS.pak/drastic"
cp "$ROOT/drastic.bin" "$NP/cores/nds/base/NDS.pak/drastic/drastic"
printf '#!/bin/sh\n# upstream release\n' > "$NP/cores/nds/base/NDS.pak/launch.sh"
OUT=$(run status); echo "$OUT" | grep -q '^base_bundled=1$'
echo "$OUT" | grep -q '^present=1$'
run install | grep -q '^ok=1$'
test "$(cat "$SD/Emus/tg5050/NDS.pak/drastic/libs/libdrastic_wifi.so")" = radio-tg5050
grep -q '^# netplay$' "$SD/Emus/tg5050/NDS.pak/launch.sh"
test -d "$SD/Roms/Nintendo DS (NDS)"
grep -q '^base.tg5050=1$' "$STATE/nds/install.manifest"
! grep -q '^base.tg5040=' "$STATE/nds/install.manifest"
test "$(cat "$SD/Emus/tg5040/NDS.pak/drastic/config/drastic.cfg")" = mine
echo '  ok   a platform without NDS.pak gets the bundled release plus local wireless'

run remove | grep -q '^ok=1$'
test ! -e "$SD/Emus/tg5040/NDS.pak"
test ! -e "$SD/Emus/tg5050/NDS.pak"
test ! -e "$STATE/nds/install.manifest"
OUT=$(run status); echo "$OUT" | grep -q '^present=0$'
echo '  ok   remove deletes NDS.pak from the card and forgets the install'
