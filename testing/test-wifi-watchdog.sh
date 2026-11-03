#!/bin/sh
set -eu

ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT
NP="$ROOT/Netplay.pak"
BIN="$ROOT/bin"
STATE="$ROOT/.userdata/shared/Netplay"
mkdir -p "$STATE" "$NP/launcher" "$BIN"
cp "$(dirname "$0")/../launcher/common/wifi-watchdog.sh" \
	"$(dirname "$0")/../launcher/common/wifi-platform.sh" \
	"$(dirname "$0")/../launcher/common/state-path.sh" "$NP/launcher/"

cat > "$BIN/iw" <<EOF
#!/bin/sh
case "\$*" in
  "dev wlan0 link")
    if [ -f "$ROOT/associated" ]; then printf 'Connected to 00:11:22:33:44:55\nSSID: home\n'
    elif [ -f "$ROOT/adhoc" ]; then printf 'Connected to 66:77:88:99:aa:bb\nSSID: nextui-TEST\n'; fi
    ;;
  "dev wlan0 set power_save on") touch "$ROOT/powersave-restored" ;;
esac
EOF
cat > "$BIN/ip" <<EOF
#!/bin/sh
case "\$*" in
  "-4 addr show wlan0")
    if [ -f "$ROOT/associated" ]; then echo 'inet 192.168.0.44/24'
    elif [ -f "$ROOT/adhoc" ]; then echo 'inet 10.0.0.20/24'; fi ;;
esac
EOF
cat > "$BIN/wpa_supplicant" <<EOF
#!/bin/sh
case " \$* " in *" -B "*) touch "$ROOT/daemonized";; esac
touch "$ROOT/associated"
EOF
cat > "$BIN/udhcpc" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$BIN/killall" <<'EOF'
#!/bin/sh
exit 0
EOF
# The host answers its first five pings while $ROOT/host-up exists.
cat > "$BIN/ping" <<PING
#!/bin/sh
n=\$(cat "$ROOT/pings" 2>/dev/null || echo 0); echo \$((n + 1)) > "$ROOT/pings"
[ -f "$ROOT/host-up" ] && [ "\$n" -lt 5 ] && exit 0
exit 1
PING
chmod 755 "$BIN"/* "$NP/launcher/wifi-watchdog.sh"

cat > "$NP/launcher/gameswitcher.sh" <<EOF
#!/bin/sh
touch "$ROOT/gameswitcher-idle"
EOF
chmod 755 "$NP/launcher/gameswitcher.sh"

printf '%s\n' "$BIN/wpa_supplicant -iwlan0 -c/tmp/original.conf" > "$STATE/wifi_restore"
printf 'on\n' > "$STATE/wifi_powersave"
printf 'role=client\npeer=10.0.0.1\nadhoc_ssid=nextui-TEST\nadhoc_psk=testpass\n' > "$STATE/session"

PATH="$BIN:$PATH" SDCARD_PATH="$ROOT" NETPLAY_WATCHDOG_INTERVAL=0 NETPLAY_WATCHDOG_FAILS=1 \
	NETPLAY_WATCHDOG_MAX_CHECKS=2 "$NP/launcher/wifi-watchdog.sh" "$NP"

fail=0
ok() { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
[ ! -e "$STATE/session" ] && ok "recovered guest session ended" || bad "guest session remained armed"
[ -e "$ROOT/gameswitcher-idle" ] && ok "Game Switcher returned to idle" || bad "Game Switcher was not restored"
[ -e "$ROOT/powersave-restored" ] && [ ! -e "$STATE/wifi_powersave" ] &&
	ok "WiFi power-save state restored" || bad "WiFi power-save state was retained"
[ -e "$ROOT/daemonized" ] && ok "captured supplicant daemonized" || bad "supplicant replay lacked -B"

rm -f "$ROOT/associated" "$ROOT/gameswitcher-idle"
printf '%s\n' "$BIN/wpa_supplicant -iwlan0 -c/tmp/original.conf" > "$STATE/wifi_restore"
printf 'role=host\nadhoc_ssid=nextui-TEST\nadhoc_psk=testpass\n' > "$STATE/session"
PATH="$BIN:$PATH" SDCARD_PATH="$ROOT" NETPLAY_WATCHDOG_INTERVAL=0 NETPLAY_WATCHDOG_FAILS=1 \
	NETPLAY_WATCHDOG_MAX_CHECKS=2 "$NP/launcher/wifi-watchdog.sh" "$NP"

[ -f "$STATE/session" ] && grep -q '^role=host$' "$STATE/session" &&
	ok "host session preserved after recovery" || bad "host session was ended"
! grep -q '^adhoc_ssid=' "$STATE/session" &&
	ok "obsolete host ad hoc credentials cleared" || bad "obsolete host credentials remained"
[ ! -e "$ROOT/gameswitcher-idle" ] && ok "host Game Switcher remained armed" || bad "host Game Switcher was reset"

# A session that outlasts the backstop: the host answers for more checks than
# MAX_CHECKS allows, then goes away. The watchdog must still be there to bring
# the guest home (an evening session once outlived the 8 h stop).
rm -f "$ROOT/associated" "$ROOT/gameswitcher-idle" "$ROOT/pings"
touch "$ROOT/adhoc" "$ROOT/host-up"
printf '%s\n' "$BIN/wpa_supplicant -iwlan0 -c/tmp/original.conf" > "$STATE/wifi_restore"
printf 'role=client\npeer=10.0.0.1\nadhoc_ssid=nextui-TEST\nadhoc_psk=testpass\n' > "$STATE/session"
PATH="$BIN:$PATH" SDCARD_PATH="$ROOT" NETPLAY_WATCHDOG_INTERVAL=0 NETPLAY_WATCHDOG_FAILS=1 \
	NETPLAY_WATCHDOG_MAX_CHECKS=2 "$NP/launcher/wifi-watchdog.sh" "$NP"
[ -e "$ROOT/associated" ] && [ ! -e "$STATE/session" ] &&
	ok "guest recovered after its session outlasted the backstop" || bad "watchdog gave up before the host left"
grep -q 'still armed after' "$STATE/watchdog.log" &&
	ok "backstop kept watching while the session was armed" || bad "backstop did not keep watching"

# With no session left, the backstop still ends a watchdog that has nothing
# to protect.
rm -f "$ROOT/associated" "$ROOT/pings" "$STATE/session"
touch "$ROOT/adhoc" "$ROOT/host-up"
printf '%s\n' "$BIN/wpa_supplicant -iwlan0 -c/tmp/original.conf" > "$STATE/wifi_restore"
: > "$STATE/watchdog.log"
PATH="$BIN:$PATH" SDCARD_PATH="$ROOT" NETPLAY_WATCHDOG_INTERVAL=0 NETPLAY_WATCHDOG_FAILS=1 \
	NETPLAY_WATCHDOG_MAX_CHECKS=2 "$NP/launcher/wifi-watchdog.sh" "$NP"
grep -q 'backstop reached' "$STATE/watchdog.log" &&
	ok "backstop still ends a watchdog with no session" || bad "watchdog without a session ran on"

exit "$fail"
