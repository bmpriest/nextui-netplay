#!/bin/sh
# Exercises the launcher against a fake SD tree: stub install/uninstall, and
# the minarch.elf shadow's routing decisions. Host-only, no device needed.
#
#   ./test.sh

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT

export SDCARD_PATH="$ROOT"
export PLATFORM=tg5040
export SYSTEM_PATH="$ROOT/.system/$PLATFORM"
export USERDATA_PATH="$ROOT/.userdata/$PLATFORM"

NP="$ROOT/Tools/$PLATFORM/Netplay.pak"
STATE="$ROOT/.userdata/shared/Netplay"
fail=0
ok()   { echo "  ok   $1"; }
bad()  { echo "  FAIL $1"; fail=1; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }

# --- fake SD card ---------------------------------------------------------
mkdir -p "$USERDATA_PATH" "$STATE" "$SYSTEM_PATH/bin" "$SYSTEM_PATH/cores" "$NP/launcher" "$NP/bin/$PLATFORM"
cp "$HERE/launch-stub.sh" "$HERE/install-stubs.sh" "$HERE/minarch.elf" "$HERE/wrap-pak.sh" \
	   "$HERE/bind-mount.sh" "$HERE/mount-common.sh" "$HERE/pre-launch.sh" \
	   "$HERE/adhoc-join.sh" "$HERE/session-cleanup.sh" "$HERE/wifi-platform.sh" \
	   "$HERE/state-path.sh" "$HERE/gameswitcher.sh" "$HERE/gameswitcher-launch.sh" "$NP/launcher/"
chmod 755 "$NP/launcher"/*
: > "$NP/bin/$PLATFORM/netplay_shim.so"

echo "== shared state migration"
mkdir -p "$NP/state"
printf 'legacy\n' > "$NP/state/migration-probe"
. "$NP/launcher/state-path.sh"
[ -f "$STATE/migration-probe" ] && [ ! -e "$NP/state/migration-probe" ] \
	&& ok "pak-local state migrated to shared userdata" \
	|| bad "pak-local state was not migrated"

echo "== ad-hoc launch role routing"
mkdir -p "$ROOT/fake-bin"
cat > "$ROOT/fake-bin/iw" <<EOF
#!/bin/sh
touch "$ROOT/host-ran-client-rejoin"
exit 1
EOF
chmod 755 "$ROOT/fake-bin/iw"
printf 'role=host\nadhoc_ssid=nextui-TEST\nadhoc_psk=playwithme\n' > "$STATE/session"
PATH="$ROOT/fake-bin:$PATH" "$NP/launcher/adhoc-join.sh" "$STATE/session"
[ ! -e "$ROOT/host-ran-client-rejoin" ] \
	&& ok "host treats ad-hoc credentials as broker metadata" \
	|| bad "host ran the client ad-hoc rejoin path"
rm -f "$STATE/session"

echo
echo "== leftover session cleanup"
printf 'boot-now\n' > "$ROOT/boot-id"
printf 'role=host\nboot_id=boot-now\n' > "$STATE/session"
printf 'guest_count=0\n' > "$STATE/broker.status"
NETPLAY_BOOT_ID_PATH="$ROOT/boot-id" "$NP/launcher/session-cleanup.sh" || rc=$?
[ -f "$STATE/session" ] \
	&& ok "empty host remains armed during the same boot" \
	|| bad "empty host was mistaken for a stale session"

printf 'role=host\nboot_id=boot-before\n' > "$STATE/session"
printf '123\n' > "$STATE/broker.pid"
printf 'guest_count=0\n' > "$STATE/broker.status"
rc=0
NETPLAY_BOOT_ID_PATH="$ROOT/boot-id" "$NP/launcher/session-cleanup.sh" || rc=$?
[ "$rc" = 10 ] && [ ! -e "$STATE/session" ] \
	&& [ ! -e "$STATE/broker.pid" ] && [ ! -e "$STATE/broker.status" ] \
	&& ok "previous-boot host session and broker artifacts removed" \
	|| bad "previous-boot host session survived cleanup"

printf 'role=host\n' > "$STATE/session"
NETPLAY_BOOT_ID_PATH="$ROOT/boot-id" "$NP/launcher/session-cleanup.sh" || rc=$?
[ -f "$STATE/session" ] \
	&& ok "legacy session without boot identity is preserved" \
	|| bad "legacy session was removed without proof it was stale"

echo
echo "== Wi-Fi power save setting"
cat > "$ROOT/fake-bin/iw" <<EOF
#!/bin/sh
echo "iw \$*" >> "$ROOT/iw-calls"
EOF
chmod 755 "$ROOT/fake-bin/iw"
prelaunch() {
	rm -f "$ROOT/iw-calls"
	( PATH="$ROOT/fake-bin:$PATH" NETPLAY_PAK="$NP" \
		NETPLAY_BOOT_ID_PATH="$ROOT/boot-id" . "$NP/launcher/pre-launch.sh" ) >/dev/null 2>&1
}
printf 'role=host\n' > "$STATE/session"
rm -f "$STATE/settings"
prelaunch
grep -q "power_save" "$ROOT/iw-calls" 2>/dev/null \
	&& bad "power save touched by default" \
	|| ok "power save left to the platform by default"
printf 'disable_powersave=1\n' > "$STATE/settings"
prelaunch
grep -q "set power_save off" "$ROOT/iw-calls" 2>/dev/null \
	&& ok "setting on disables power save for an armed launch" \
	|| bad "setting on left power save alone"
printf 'disable_powersave=0\n' > "$STATE/settings"
prelaunch
grep -q "power_save" "$ROOT/iw-calls" 2>/dev/null \
	&& bad "power save touched with the setting off" \
	|| ok "setting off leaves power save to the platform"
printf 'disable_powersave=1\n' > "$STATE/settings"
rm -f "$STATE/session"
prelaunch
grep -q "power_save" "$ROOT/iw-calls" 2>/dev/null \
	&& bad "unarmed launch touched power save" \
	|| ok "unarmed launch leaves power save alone"
rm -f "$STATE/settings" "$ROOT/fake-bin/iw"
rm -f "$STATE/session"

# A stand-in minarch that just reports the arguments and env it was handed.
cat > "$SYSTEM_PATH/bin/minarch.elf" <<'EOF'
#!/bin/sh
[ "$NETPLAY_COMPAT_CORE" = "1" ] && echo "compat=1"
[ "$NETPLAY_DUAL_CORE" = "1" ] && echo "dual=1"
[ -n "$NETPLAY_CORE_NOTICE" ] && echo "notice=$NETPLAY_CORE_NOTICE"
echo "minarch core=$1 rom=$2 real=$NETPLAY_REAL_CORE session=$NETPLAY_SESSION"
# Model stock MinArch's MENU+SELECT side effects. The shim rejects the actual
# state, but the frontend has already written its slot marker and preview.
if [ "$FAKE_GAMESWITCHER" = "1" ]; then
	ui="$SDCARD_PATH/.userdata/shared/.minui/GBA"
	name=$(basename "$2")
	mkdir -p "$ui"
	echo 0 > "$ui/$name.txt"
	echo generated > "$ui/$name.0.bmp"
	echo generated-disc > "$ui/$name.0.txt"
	mkdir -p "$SDCARD_PATH/.userdata/shared/.minui"
	echo "$2" > "$SDCARD_PATH/.userdata/shared/.minui/game_switcher.txt"
fi
# Stand in for the shim deciding this pairing cannot run instanced here.
if [ "$FAKE_WRITE_FALLBACK" = "1" ] && [ -n "$NETPLAY_SERIAL_FALLBACK" ]; then
	echo 1 > "$NETPLAY_SERIAL_FALLBACK"
fi
EOF
chmod 755 "$SYSTEM_PATH/bin/minarch.elf"

# System emu paks: two netplay-capable, one not.
for spec in "GBA:gpsp" "GB:gambatte" "VB:mednafen_vb"; do
	tag=${spec%%:*}; exe=${spec##*:}
	mkdir -p "$SYSTEM_PATH/paks/Emus/$tag.pak"
	cat > "$SYSTEM_PATH/paks/Emus/$tag.pak/launch.sh" <<EOF
#!/bin/sh
EMU_EXE=$exe
EMU_TAG=\$(basename "\$(dirname "\$0")" .pak)
minarch.elf "\$CORES_PATH/\${EMU_EXE}_libretro.so" "\$1"
EOF
	chmod 755 "$SYSTEM_PATH/paks/Emus/$tag.pak/launch.sh"
done

# Reports its $0-derived EMU_TAG, so the bind mount can be checked for keeping
# $0 at the pak's real path. Kept separate from the paks above because three
# tests compare their launch output byte for byte.
mkdir -p "$SYSTEM_PATH/paks/Emus/MD.pak"
cat > "$SYSTEM_PATH/paks/Emus/MD.pak/launch.sh" <<'EOF'
#!/bin/sh
EMU_EXE=picodrive
EMU_TAG=$(basename "$(dirname "$0")" .pak)
echo "tag=$EMU_TAG"
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$1"
EOF
chmod 755 "$SYSTEM_PATH/paks/Emus/MD.pak/launch.sh"

# An EXTRAS pak already sitting on the SD path - must be left alone. Bundles
# its own core and locates it via CORES_PATH=$(dirname "$0"), like FBN/SUPA.
mkdir -p "$ROOT/Emus/$PLATFORM/FBN.pak"
cat > "$ROOT/Emus/$PLATFORM/FBN.pak/launch.sh" <<'EOF'
#!/bin/sh
EMU_EXE=fbneo
CORES_PATH=$(dirname "$0")
EMU_TAG=$(basename "$(dirname "$0")" .pak)
echo "tag=$EMU_TAG"
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$1"
EOF
chmod 755 "$ROOT/Emus/$PLATFORM/FBN.pak/launch.sh"
echo "fake fbneo core" > "$ROOT/Emus/$PLATFORM/FBN.pak/fbneo_libretro.so"
FBN_BEFORE=$(cat "$ROOT/Emus/$PLATFORM/FBN.pak/launch.sh")

export CORES_PATH="$SYSTEM_PATH/cores"

# my282 selects the core inside a case statement rather than assigning EMU_EXE
# at the top level. An anchored match finds nothing there and silently installs
# no stubs at all, so keep a pak in that shape in the fixture.
mkdir -p "$SYSTEM_PATH/paks/Emus/GBA282.pak"
cat > "$SYSTEM_PATH/paks/Emus/GBA282.pak/launch.sh" <<'EOF'
#!/bin/sh
EMU_TAG=$(basename "$(dirname "$0")" .pak)
case "$EMU_TAG" in
	FC) EMU_EXE=fceumm ;;
	GB|GBC) EMU_EXE=gambatte ;;
	GBA282) EMU_EXE=gpsp ;;
	*) exit 1 ;;
esac
exec minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$1"
EOF
chmod 755 "$SYSTEM_PATH/paks/Emus/GBA282.pak/launch.sh"

echo "== install"
"$NP/launcher/install-stubs.sh" install > "$ROOT/install.log" 2>&1 || bad "installer exited non-zero"
[ -f "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" ] && ok "GBA stub installed"  || bad "GBA stub missing"
[ -f "$ROOT/Emus/$PLATFORM/GB.pak/launch.sh" ]  && ok "GB stub installed"   || bad "GB stub missing"
[ -f "$ROOT/Emus/$PLATFORM/VB.pak/launch.sh" ]  && bad "VB should be skipped (unsupported core)" || ok "VB skipped"
[ -f "$ROOT/Emus/$PLATFORM/GBA282.pak/launch.sh" ] && ok "my282 case-statement pak detected" \
	|| bad "my282 case-statement pak missed (anchored EMU_EXE match?)"
check "EXTRAS pak untouched" "$(cat "$ROOT/Emus/$PLATFORM/FBN.pak/launch.sh")" "$FBN_BEFORE"
grep -q "skip FBN (fbneo)" "$ROOT/install.log" && ok "EXTRAS pak reported as uncovered" || bad "no report for FBN"

echo
echo "== launch with no session (must be byte-identical to stock)"
OUT=$("$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba 2>&1)
check "routes to real core" "$OUT" "minarch core=$SYSTEM_PATH/cores/gpsp_libretro.so rom=/roms/game.gba real= session="

echo
echo "== launch with a session armed"
OUT=$(NETPLAY_SESSION=/tmp/session "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba 2>&1)
check "routes through shim" "$OUT" \
	"minarch core=$NP/cores/gpsp_libretro.so rom=/roms/game.gba real=$SYSTEM_PATH/cores/gpsp_libretro.so session=/tmp/session"
[ -f "$NP/cores/gpsp_libretro.so" ] && ok "shim staged under the real core's filename" \
	|| bad "shim not staged as gpsp_libretro.so"

# A normal launch must not rewrite the same shared object to the SD card.
SHIM_INODE=$(ls -i "$NP/cores/gpsp_libretro.so" | awk '{print $1}')
OUT=$(NETPLAY_SESSION=/tmp/session "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba 2>&1)
check "unchanged shim is not copied again" \
	"$(ls -i "$NP/cores/gpsp_libretro.so" | awk '{print $1}')" "$SHIM_INODE"

echo
echo "== shim staging preserves core.name"
# minarch: basename truncated at the last underscore -> feeds config_dir/states_dir
NAME=$(basename "$NP/cores/gpsp_libretro.so" | sed 's/_[^_]*$//')
check "core.name unchanged" "$NAME" "gpsp"

echo
echo "== missing shim falls back to stock"
mv "$NP/bin/$PLATFORM/netplay_shim.so" "$NP/bin/$PLATFORM/hidden.so"
OUT=$(NETPLAY_SESSION=/tmp/session "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba 2>&1 | tail -n 1)
check "falls back" "$OUT" "minarch core=$SYSTEM_PATH/cores/gpsp_libretro.so rom=/roms/game.gba real= session=/tmp/session"
mv "$NP/bin/$PLATFORM/hidden.so" "$NP/bin/$PLATFORM/netplay_shim.so"

echo
echo "== force-shim file routes without an env var"
# Game-list launches are started by NextUI, so the force flag has to be a file.
: > "$STATE/force-shim"
OUT=$("$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba 2>&1)
echo "$OUT" | grep -q "core=$NP/cores/gpsp_libretro.so" \
	&& ok "force file routes through shim" || bad "force file ignored: $OUT"
rm -f "$STATE/force-shim"
OUT=$("$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba 2>&1)
check "clearing it restores stock" "$OUT" "minarch core=$SYSTEM_PATH/cores/gpsp_libretro.so rom=/roms/game.gba real= session="

echo
echo "== mandatory link cores and compatibility fallback"
mkdir -p "$NP/cores/override/$PLATFORM" "$NP/cores/compatibility/aarch64"
echo "pretend network-enabled core" > "$NP/cores/override/$PLATFORM/gpsp_libretro.so"
echo "pretend compatibility core" > "$NP/cores/compatibility/aarch64/fceumm_libretro.so"
printf 'role=host\nport=55437\n' > "$ROOT/compat.session"
OUT=$(NETPLAY_SESSION="$ROOT/compat.session" "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba 2>&1)
echo "$OUT" | grep -q "real=$NP/cores/override/$PLATFORM/gpsp_libretro.so" \
	&& ok "packaged gpSP used for link implementation" || bad "packaged gpSP ignored: $OUT"
echo "$OUT" | grep -q "compat=1" \
	&& ok "packaged-core launch is marked" || bad "compatibility environment missing: $OUT"
echo "$OUT" | grep -q "notice=netlink" \
	&& ok "net-enabled core gets its own startup notice" || bad "netlink notice missing: $OUT"
echo "$OUT" | grep -q "core=$NP/cores/gpsp_libretro.so" \
	&& ok "shim still staged under the real core name" || bad "shim name wrong: $OUT"

# Instanced Gambatte is a separate implementation artifact. It must be used
# only for an explicitly enabled link session and remain staged under the
# ordinary gambatte basename so MinArch keeps the user's save/config paths.
echo "pretend network-enabled Gambatte" > "$NP/cores/override/$PLATFORM/gambatte_libretro.so"
echo "pretend paired Gambatte" > "$NP/cores/override/$PLATFORM/gambatte_dual_libretro.so"
# NS_writeSession does not know which game will launch next, so it intentionally
# omits mode=. Gambatte's identity establishes link mode after the core opens;
# the launcher must be able to select the paired implementation before then.
printf 'role=host\nport=55437\ninstanced_gambatte=1\n' > "$ROOT/dual.session"
OUT=$(NETPLAY_SESSION="$ROOT/dual.session" "$ROOT/Emus/$PLATFORM/GB.pak/launch.sh" /roms/game.gb 2>&1)
echo "$OUT" | grep -q "real=$NP/cores/override/$PLATFORM/gambatte_dual_libretro.so" \
	&& echo "$OUT" | grep -q "dual=1" \
	&& ok "instanced GB link selected the paired Gambatte core" \
	|| bad "paired Gambatte selection failed: $OUT"
echo "$OUT" | grep -q "notice=paired" \
	&& ok "paired core is announced as itself, not as a compatibility core" \
	|| bad "paired notice missing: $OUT"

# A pairing neither device can host demotes to the net-enabled core. The shim
# writes the marker and asks to shut down; one relaunch, and only one.
OUT=$(NETPLAY_SESSION="$ROOT/dual.session" FAKE_WRITE_FALLBACK=1 \
	"$ROOT/Emus/$PLATFORM/GB.pak/launch.sh" /roms/game.gb 2>&1)
echo "$OUT" | grep -q "instanced link declined" \
	&& ok "declined pairing is reported" || bad "no demotion message: $OUT"
echo "$OUT" | grep -q "real=$NP/cores/override/$PLATFORM/gambatte_libretro.so" \
	&& ok "demotion relaunched on the network-serial core" || bad "no serial relaunch: $OUT"
echo "$OUT" | grep -q "notice=serial-fallback" \
	&& ok "demotion explains itself to the player" || bad "no fallback notice: $OUT"
[ "$(echo "$OUT" | grep -c 'dual=1')" -eq 1 ] \
	&& ok "demotion relaunches exactly once" || bad "relaunch loop: $OUT"
printf 'role=host\nport=55437\nmode=link\ninstanced_gambatte=0\n' > "$ROOT/serial.session"
OUT=$(NETPLAY_SESSION="$ROOT/serial.session" "$ROOT/Emus/$PLATFORM/GB.pak/launch.sh" /roms/game.gb 2>&1)
echo "$OUT" | grep -q "real=$NP/cores/override/$PLATFORM/gambatte_libretro.so" \
	&& ! echo "$OUT" | grep -q "dual=1" \
	&& ok "ordinary GB link retained the network-serial Gambatte core" \
	|| bad "network-serial Gambatte fallback failed: $OUT"

# mGBA's installed pak carries only the ordinary core. The common minarch
# wrapper selects Netplay's paired artifact, keeping verbose logging and every
# other launch policy in the same path as the built-in emulator paks.
mkdir -p "$ROOT/Emus/$PLATFORM/MGBA.pak"
cp "$PWD/testing/MGBA.pak/launch.sh" "$ROOT/Emus/$PLATFORM/MGBA.pak/launch.sh"
echo "ordinary mGBA" > "$ROOT/Emus/$PLATFORM/MGBA.pak/mgba_libretro.so"
echo "paired mGBA" > "$NP/cores/override/$PLATFORM/mgba_dual_libretro.so"
chmod 755 "$ROOT/Emus/$PLATFORM/MGBA.pak/launch.sh"
printf 'role=host\nport=55437\ninstanced_mgba=1\n' > "$ROOT/mgba-dual.session"
OUT=$(NETPLAY_SESSION="$ROOT/mgba-dual.session" \
	BIOS_PATH="$ROOT/bios" SAVES_PATH="$ROOT/saves" CHEATS_PATH="$ROOT/cheats" \
	LOGS_PATH="$ROOT/logs" "$ROOT/Emus/$PLATFORM/MGBA.pak/launch.sh" /roms/game.gba 2>&1)
MGBA_OUT=$(cat "$ROOT/logs/MGBA.txt")
echo "$MGBA_OUT" | grep -q "real=$NP/cores/override/$PLATFORM/mgba_dual_libretro.so" \
	&& echo "$MGBA_OUT" | grep -q "dual=1" \
	&& ok "installed mGBA pak selected Netplay's paired override" \
	|| bad "paired mGBA selection failed: $MGBA_OUT"

echo
echo "== verbose per-game process logs"
printf 'role=host\nport=55437\nsession_id=0123456789abcdef0123456789abcdef\nverbose_logs=1\n' > "$ROOT/verbose.session"
NETPLAY_SESSION="$ROOT/verbose.session" "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba >/dev/null 2>&1
NETPLAY_SESSION="$ROOT/verbose.session" "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba >/dev/null 2>&1
VERBOSE_COUNT=$(find "$USERDATA_PATH/logs/netplay-games/game.gba/0123456789abcdef0123456789abcdef" \
	-type f -name '*-host-*.log' 2>/dev/null | wc -l)
VERBOSE_LOG=$(find "$USERDATA_PATH/logs/netplay-games/game.gba/0123456789abcdef0123456789abcdef" \
	-type f -name '*-host-*.log' 2>/dev/null | head -n 1)
[ "$VERBOSE_COUNT" -eq 2 ] && ok "each launch received its own game/session log" \
	|| bad "verbose launch log missing"
grep -q '^rom=/roms/game.gba$' "$VERBOSE_LOG" 2>/dev/null \
	&& grep -q '^=== process exited status=0 ===$' "$VERBOSE_LOG" 2>/dev/null \
	&& ok "verbose log retained metadata and process outcome" \
	|| bad "verbose launch log incomplete"

# Other cores stay on the installed build until the setup negotiation selects
# the matching compatibility build on both devices.
OUT=$(NETPLAY_SESSION="$ROOT/compat.session" "$NP/launcher/minarch.elf" \
	"$SYSTEM_PATH/cores/fceumm_libretro.so" /roms/game.nes 2>&1)
echo "$OUT" | grep -q "real=$SYSTEM_PATH/cores/fceumm_libretro.so" \
	&& ok "installed core remains first choice" || bad "compatibility core used eagerly: $OUT"
printf 'compat_core.fceumm=1\n' >> "$ROOT/compat.session"
OUT=$(NETPLAY_SESSION="$ROOT/compat.session" "$NP/launcher/minarch.elf" \
	"$SYSTEM_PATH/cores/fceumm_libretro.so" /roms/game.nes 2>&1)
echo "$OUT" | grep -q "real=$NP/cores/compatibility/aarch64/fceumm_libretro.so" \
	&& ok "selected compatibility core used" || bad "compatibility fallback ignored: $OUT"
echo "$OUT" | grep -q "notice=compatibility" \
	&& ok "fallback startup notice selected" || bad "fallback notice missing: $OUT"

# The A30 SFC pak requests Snes9x 2005, while the cross-platform compatibility
# artifact uses the canonical snes9x basename regardless of its implementation.
echo "pretend canonical snes9x compatibility core" > "$NP/cores/compatibility/aarch64/snes9x_libretro.so"
printf 'role=client\nport=55437\ncompat_core.snes9x2005=1\n' > "$ROOT/snes.session"
OUT=$(NETPLAY_SESSION="$ROOT/snes.session" "$NP/launcher/minarch.elf" \
	"$SYSTEM_PATH/cores/snes9x2005_libretro.so" /roms/game.sfc 2>&1)
echo "$OUT" | grep -q "real=$NP/cores/compatibility/aarch64/snes9x_libretro.so" \
	&& ok "Snes9x 2005 pak routed to canonical compatibility core" \
	|| bad "Snes9x 2005 compatibility alias failed: $OUT"

printf 'role=host\nport=55437\ncore_mismatch.fceumm=1\n' > "$ROOT/mismatch.session"
OUT=$(NETPLAY_SESSION="$ROOT/mismatch.session" "$NP/launcher/minarch.elf" \
	"$SYSTEM_PATH/cores/fceumm_libretro.so" /roms/game.nes 2>&1)
echo "$OUT" | grep -q "real=$SYSTEM_PATH/cores/fceumm_libretro.so" \
	&& echo "$OUT" | grep -q "notice=mismatch" \
	&& ok "differing installed cores launch with a warning" \
	|| bad "installed-core mismatch warning missing: $OUT"
rm -rf "$NP/cores/compatibility"
OUT=$(NETPLAY_SESSION="$ROOT/compat.session" "$NP/launcher/minarch.elf" \
	"$SYSTEM_PATH/cores/fceumm_libretro.so" /roms/game.nes 2>&1)
echo "$OUT" | grep -q "real=$SYSTEM_PATH/cores/fceumm_libretro.so" \
	&& ok "falls back to the system core" || bad "no fallback: $OUT"

echo
echo "== session file is discovered without an env var"
printf 'role=host\nport=55437\n' > "$STATE/session"
OUT=$("$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba 2>&1)
echo "$OUT" | grep -q "core=$NP/cores/gpsp_libretro.so" \
	&& ok "session file routes through shim" || bad "session file ignored: $OUT"
echo "$OUT" | grep -q "session=$STATE/session" \
	&& ok "NETPLAY_SESSION exported to minarch" || bad "session not exported: $OUT"
rm -f "$STATE/session"

echo
echo "== uninstall"
"$NP/launcher/install-stubs.sh" uninstall > /dev/null 2>&1
[ -e "$ROOT/Emus/$PLATFORM/GBA.pak" ] && bad "GBA stub dir left behind" || ok "GBA stub removed"
check "EXTRAS pak still untouched" "$(cat "$ROOT/Emus/$PLATFORM/FBN.pak/launch.sh")" "$FBN_BEFORE"

echo
echo "== uninstall leaves a pak that became someone else's alone"
"$NP/launcher/install-stubs.sh" install > /dev/null 2>&1
echo "#!/bin/sh
# replaced by another pak" > "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh"
"$NP/launcher/install-stubs.sh" uninstall > "$ROOT/uninstall2.log" 2>&1
[ -f "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" ] && ok "foreign launch.sh preserved" || bad "removed a file that was not ours"

"$NP/launcher/install-stubs.sh" uninstall > /dev/null 2>&1

echo
echo "== wrap-pak: stage an EXTRAS pak without touching it"
cp "$HERE/wrap-pak.sh" "$NP/launcher/" && chmod 755 "$NP/launcher/wrap-pak.sh"
"$NP/launcher/wrap-pak.sh" sync > "$ROOT/sync.log" 2>&1 || bad "sync exited non-zero"
check "original still untouched" "$(cat "$ROOT/Emus/$PLATFORM/FBN.pak/launch.sh")" "$FBN_BEFORE"
check "launch.sh.old is the original, verbatim" "$(cat "$NP/wrapped/FBN.pak/launch.sh.old")" "$FBN_BEFORE"
grep -q "Installed by Netplay.pak" "$NP/wrapped/FBN.pak/launch.sh" && ok "staged launch.sh is ours" || bad "staged launch.sh not marked"
[ -f "$NP/wrapped/FBN.pak/fbneo_libretro.so" ] && ok "bundled core copied into staging" || bad "bundled core missing from staging"

echo
echo "== wrap-pak: what the bind mount would present"
# cp -a of the staged dir over the original path presents exactly the contents
# the bind mount does, so $0-derived EMU_TAG and CORES_PATH resolve identically.
rm -rf "$ROOT/Emus/$PLATFORM/FBN.pak"
cp -a "$NP/wrapped/FBN.pak" "$ROOT/Emus/$PLATFORM/FBN.pak"

OUT=$("$ROOT/Emus/$PLATFORM/FBN.pak/launch.sh" /roms/game.zip 2>&1)
echo "$OUT" | grep -qx "tag=FBN" && ok "EMU_TAG still resolves to FBN" || bad "EMU_TAG wrong: $OUT"
echo "$OUT" | grep -q "core=$ROOT/Emus/$PLATFORM/FBN.pak/fbneo_libretro.so" \
	&& ok "CORES_PATH finds the bundled core" || bad "CORES_PATH wrong: $OUT"

OUT=$(NETPLAY_SESSION=/tmp/session "$ROOT/Emus/$PLATFORM/FBN.pak/launch.sh" /roms/game.zip 2>&1)
echo "$OUT" | grep -q "core=$NP/cores/fbneo_libretro.so" \
	&& ok "wrapped pak routes through the shim" || bad "no shim routing: $OUT"
echo "$OUT" | grep -q "real=$ROOT/Emus/$PLATFORM/FBN.pak/fbneo_libretro.so" \
	&& ok "real core passed to the shim" || bad "wrong real core: $OUT"

echo
echo "== bind-mount: stage system paks, writing nothing to the Emus tree"
GBA_BEFORE=$(cat "$SYSTEM_PATH/paks/Emus/GBA.pak/launch.sh")
# Snapshot rather than assert emptiness: earlier install-stubs tests deliberately
# leave a foreign pak behind, so what matters is that we add nothing of our own.
EMUS_BEFORE=$(ls -R "$ROOT/Emus" 2>/dev/null)
"$NP/launcher/bind-mount.sh" sync > "$ROOT/bmsync.log" 2>&1 || bad "sync exited non-zero"
check "system pak untouched by staging" "$(cat "$SYSTEM_PATH/paks/Emus/GBA.pak/launch.sh")" "$GBA_BEFORE"
check "launch.sh.old is the original, verbatim" "$(cat "$NP/mounts/GBA.pak/launch.sh.old")" "$GBA_BEFORE"
grep -q "Installed by Netplay.pak" "$NP/mounts/GBA.pak/launch.sh" \
	&& ok "staged launch.sh is ours" || bad "staged launch.sh not marked"
[ -d "$NP/mounts/VB.pak" ] && bad "staged a pak whose core we cannot drive" || ok "unsupported core not staged"

# The entire point of the rewrite: the Emus tree gains nothing.
check "Emus tree untouched by the mount route" "$(ls -R "$ROOT/Emus" 2>/dev/null)" "$EMUS_BEFORE"

echo
echo "== bind-mount: what the mount would present"
# cp -a of the staged dir over the pak's own path presents exactly what the bind
# mount does, so $0 - and therefore EMU_TAG - resolves identically.
cp -a "$NP/mounts/MD.pak/." "$SYSTEM_PATH/paks/Emus/MD.pak/"

OUT=$("$SYSTEM_PATH/paks/Emus/MD.pak/launch.sh" /roms/g.md 2>&1)
echo "$OUT" | grep -qx "tag=MD" && ok "EMU_TAG still resolves to MD under the mount" \
	|| bad "EMU_TAG wrong: $OUT"

OUT=$(NETPLAY_SESSION=/tmp/session "$SYSTEM_PATH/paks/Emus/MD.pak/launch.sh" /roms/g.md 2>&1)
echo "$OUT" | grep -q "core=$NP/cores/picodrive_libretro.so" \
	&& ok "mounted pak routes through the shim" || bad "no shim routing: $OUT"
echo "$OUT" | grep -q "real=$SYSTEM_PATH/cores/picodrive_libretro.so" \
	&& ok "real core passed to the shim" || bad "wrong real core: $OUT"

# The setup app runs its mandatory minarch-path check before arming a new
# session. When persistent wrappers are already mounted, it must validate the
# preserved original rather than conclude that every emulator pak vanished.
CHECKED=$(for p in "$SYSTEM_PATH/paks/Emus"/*.pak; do
	f="$p/launch.sh"
	if grep -q "Installed by Netplay.pak" "$f" 2>/dev/null && [ -f "$p/launch.sh.old" ]; then
		f="$p/launch.sh.old"
	fi
	grep -q 'minarch.elf' "$f" 2>/dev/null && echo "$f"
done)
echo "$CHECKED" | grep -q '/MD.pak/launch.sh.old$' \
	&& ok "preflight sees original launcher through mounted wrapper" \
	|| bad "preflight missed mounted original launcher"

echo
echo "== bind-mount: staleness is detected"
"$NP/launcher/bind-mount.sh" status 2>&1 | grep -q "stale" && bad "clean staging reported stale" || ok "fresh staging not stale"
rm -rf "$SYSTEM_PATH/paks/Emus/GBA.pak"; mkdir -p "$SYSTEM_PATH/paks/Emus/GBA.pak"
printf '#!/bin/sh\nEMU_EXE=gpsp\necho updated\n' > "$SYSTEM_PATH/paks/Emus/GBA.pak/launch.sh"
chmod 755 "$SYSTEM_PATH/paks/Emus/GBA.pak/launch.sh"
"$NP/launcher/bind-mount.sh" status 2>&1 | grep -q "stale    GBA" \
	&& ok "pak updated underneath is reported stale" || bad "stale staging not detected"

echo
echo "== bind-mount: mount ownership"
# Teardown must distinguish our exact stage root from a foreign bind mount.
# This fixture has the same shape as the Brick: /proc/mounts would name the
# exFAT block device for both, while mountinfo retains each bind root in field 4.
OWN_TARGET="$SYSTEM_PATH/paks/Emus/GB.pak"
OWN_SOURCE="$NP/mounts/GB.pak"
FOREIGN_TARGET="$SYSTEM_PATH/paks/Emus/GBA.pak"
MOUNTINFO_FIXTURE="$ROOT/proc-mountinfo"
printf '30 1 179:33 / %s rw - exfat /dev/mmcblk1p1 rw\n' "$ROOT" > "$MOUNTINFO_FIXTURE"
printf '31 30 179:33 %s %s rw - exfat /dev/mmcblk1p1 rw\n' \
	"${OWN_SOURCE#$ROOT}" "$OWN_TARGET" >> "$MOUNTINFO_FIXTURE"
printf '32 30 179:33 /other/GBA.pak %s rw - exfat /dev/mmcblk1p1 rw\n' \
	"$FOREIGN_TARGET" >> "$MOUNTINFO_FIXTURE"
NETPLAY_MOUNTINFO_FILE="$MOUNTINFO_FIXTURE"
export NETPLAY_MOUNTINFO_FILE
MOUNT_STAGE="$NP/mounts"
MARKER="Installed by Netplay.pak"
. "$NP/launcher/mount-common.sh"
mount_is_ours "$OWN_TARGET" && ok "recognises Netplay-owned mount" || bad "owned mount not recognised"
mount_is_ours "$FOREIGN_TARGET" && bad "foreign mount claimed as ours" || ok "foreign mount left unowned"
unset NETPLAY_MOUNTINFO_FILE

echo
echo "== bind-mount: boot hook"
"$NP/launcher/bind-mount.sh" hook-install > /dev/null 2>&1
grep -q "Netplay.pak-on-boot" "$USERDATA_PATH/auto.sh" && ok "hook added to auto.sh" || bad "hook not added"
"$NP/launcher/bind-mount.sh" hook-install > /dev/null 2>&1
[ "$(grep -c "Netplay.pak-on-boot" "$USERDATA_PATH/auto.sh")" = "1" ] \
	&& ok "hook install is idempotent" || bad "hook added twice"

# Must not disturb hooks other paks registered the same way.
echo 'test -f /other/on-boot && /other/on-boot # Other.pak-on-boot' >> "$USERDATA_PATH/auto.sh"
"$NP/launcher/bind-mount.sh" hook-remove > /dev/null 2>&1
grep -q "Netplay.pak-on-boot" "$USERDATA_PATH/auto.sh" && bad "our hook survived removal" || ok "hook removed"
grep -q "Other.pak-on-boot" "$USERDATA_PATH/auto.sh" && ok "another pak's hook left alone" || bad "removed someone else's hook"

echo
echo "== bind-mount: boot never fails, however broken the staging"
rm -rf "$NP/mounts"
"$NP/launcher/bind-mount.sh" boot; rc=$?
[ "$rc" = "0" ] && ok "boot exits 0 with no staging at all" || bad "boot exited $rc - would break a boot"

echo
echo "== Game Switcher integration"
SHARED="$ROOT/.userdata/shared"
RECENTS="$SHARED/.minui/recent.txt"
mkdir -p "$(dirname "$RECENTS")" "$ROOT/Roms/Game Boy Advance (GBA)"
touch "$ROOT/Roms/Game Boy Advance (GBA)/old.gba" "$ROOT/Roms/Game Boy Advance (GBA)/game.gba"
printf '/Roms/Game Boy Advance (GBA)/old.gba\tOld Game\n' > "$RECENTS"
printf 'add_gameswitcher=1\n' > "$STATE/settings"
"$NP/launcher/gameswitcher.sh" enable
head -n 1 "$RECENTS" | grep -q '^/Roms/.Netplay (NETPLAY)/Netplay' \
	&& ok "inactive shortcut is promoted" || bad "inactive shortcut missing"
[ -x "$ROOT/Emus/$PLATFORM/NETPLAY.pak/launch.sh" ] \
	&& ok "hidden emulator redirect installed" || bad "emulator redirect missing"
[ -f "$ROOT/Roms/.Netplay (NETPLAY)/Netplay" ] \
	&& ok "hidden redirect ROM installed" || bad "redirect ROM missing"

: > "$STATE/session"
"$NP/launcher/gameswitcher.sh" active
[ "$(wc -l < "$RECENTS")" = "1" ] \
	&& ok "armed switcher contains one row" || bad "armed switcher leaked normal recents"
grep -q 'old.gba' "$STATE/gameswitcher-recents.txt" \
	&& ok "normal recents backed up" || bad "normal recents not backed up"

# NextUI promotes the launched game before handing control to MinArch.
{ printf '/Roms/Game Boy Advance (GBA)/game.gba\tCurrent Game\n'; cat "$RECENTS"; } > "$RECENTS.new"
mv "$RECENTS.new" "$RECENTS"
UI="$SHARED/.minui/GBA"
mkdir -p "$UI"
printf '3\n' > "$UI/game.gba.txt"
printf 'original-preview\n' > "$UI/game.gba.3.bmp"
OUT=$(NETPLAY_SESSION="$STATE/session" FAKE_GAMESWITCHER=1 \
	"$NP/launcher/minarch.elf" "$SYSTEM_PATH/cores/gpsp_libretro.so" \
	"$ROOT/Roms/Game Boy Advance (GBA)/game.gba" 2>&1)
check "pre-existing slot marker restored" "$(cat "$UI/game.gba.txt")" "3"
check "pre-existing preview restored" "$(cat "$UI/game.gba.3.bmp")" "original-preview"
[ ! -e "$UI/game.gba.0.bmp" ] && ok "false preview removed" || bad "false preview survived"
[ ! -e "$UI/game.gba.0.txt" ] && ok "false disc metadata removed" || bad "false disc metadata survived"
[ "$(wc -l < "$RECENTS")" = "1" ] && ok "return-to-switcher view remains isolated" \
	|| bad "game leaked into armed switcher"
grep -q 'game.gba' "$STATE/gameswitcher-recents.txt" \
	&& ok "netplay game retained in normal history" || bad "netplay game lost from normal history"

rm -f "$STATE/session"
"$NP/launcher/gameswitcher.sh" idle
head -n 1 "$RECENTS" | grep -q '^/Roms/.Netplay (NETPLAY)/Netplay' \
	&& ok "inactive shortcut restored above history" || bad "inactive shortcut not restored"
grep -q 'game.gba' "$RECENTS" && ok "normal history restored" || bad "normal history missing"

printf 'add_gameswitcher=0\n' > "$STATE/settings"
"$NP/launcher/gameswitcher.sh" disable
grep -q '^/Roms/.Netplay (NETPLAY)/Netplay' "$RECENTS" \
	&& bad "disabled shortcut remains in recents" || ok "disabled shortcut removed"
[ ! -e "$ROOT/Emus/$PLATFORM/NETPLAY.pak/launch.sh" ] \
	&& ok "disabled redirect removed" || bad "disabled redirect remains"

echo
[ "$fail" -eq 0 ] && echo "PASS" || echo "FAIL"
exit $fail
