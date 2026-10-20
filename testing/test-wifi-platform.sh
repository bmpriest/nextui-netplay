#!/bin/sh
# Platform dispatch for the WiFi layer, against stub binaries.
#
# The point of these is regression cover for the specific shape of the H700
# failure: a tool that is simply absent, where the old code could not tell
# "missing" from "not associated" and looped until it gave up. Each platform
# gets a fake rootfs containing only the tools that platform actually has.
#
#   ./test-wifi-platform.sh

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WP="$HERE/../launcher/wifi-platform.sh"
fail=0
ok()   { echo "  ok   $1"; }
bad()  { echo "  FAIL $1"; fail=1; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }

ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT

# --- stub factory ---------------------------------------------------------
# Each stub appends its argv to $ROOT/calls so a test can assert on what was
# actually run, which is the whole question here.
mkstub() {  # mkstub <name> <body>
	cat > "$BIN/$1" <<EOF
#!/bin/sh
echo "$1 \$*" >> "$ROOT/calls"
$2
EOF
	chmod 755 "$BIN/$1"
}

newenv() {  # newenv <platform>
	PLATFORM="$1"
	BIN="$ROOT/$1-bin"
	SYSTEM_PATH="$ROOT/$1-system"
	rm -rf "$BIN" "$SYSTEM_PATH"
	mkdir -p "$BIN" "$SYSTEM_PATH/etc/wifi"
	: > "$ROOT/calls"
	export PLATFORM SYSTEM_PATH
	# Nothing from the host leaks in: an absent tool must be absent.
	PATH="$BIN:/usr/bin:/bin"
	export PATH
	mkstub ip 'case "$*" in "-4 addr show wlan0") [ -f "'"$ROOT"'/ip-up" ] && echo "    inet 192.168.0.44/24 brd";; esac'
	mkstub killall 'exit 0'
	mkstub basename '
		_b=${1##*/}
		[ "$1" = "--" ] && _b=${2##*/}
		echo "$_b"'
}

run() { sh "$WP" "$@"; }
called() { grep -q "^$1" "$ROOT/calls"; }

echo "== tg5040: iw + udhcpc, stock /etc/wifi path"
newenv tg5040
mkstub iw 'case "$*" in "dev wlan0 link") echo "Connected to aa:bb"; echo "	SSID: home";; esac'
mkstub udhcpc 'exit 0'
check "assoc-ssid via iw" "$(run assoc-ssid)" "home"
run assoc home && ok "assoc matches SSID" || bad "assoc did not match SSID"
run assoc other && bad "assoc matched the wrong SSID" || ok "assoc rejects other SSID"
check "dhcp client" "$(run dhcp-kind)" "udhcpc"
run dhcp-start; sleep 0.2
called udhcpc && ok "udhcpc used" || bad "udhcpc not used"
check "wifi_init path stays stock" "$(run wifi-init-path)" ""
: > "$SYSTEM_PATH/etc/wifi/wifi_init.sh"
check "SYSTEM_PATH copy is not substituted on tg5040" "$(run wifi-init-path)" ""

echo
echo "== my282: udhcpc needs the platform lease script"
newenv my282
mkstub iw 'case "$*" in "dev wlan0 link") echo "	SSID: home";; esac'
mkstub udhcpc 'exit 0'
printf '#!/bin/sh\n' > "$SYSTEM_PATH/etc/wifi/wifi_init.sh"
check "wifi_init found under SYSTEM_PATH" "$(run wifi-init-path)" "$SYSTEM_PATH/etc/wifi/wifi_init.sh"
run dhcp-start; sleep 0.2
grep -q -- "-s $SYSTEM_PATH/etc/wifi/udhcpc.script" "$ROOT/calls" \
	&& ok "udhcpc passed the platform lease script" \
	|| bad "udhcpc ran without -s (lease would not be applied)"

echo
echo "== my355: dhcpcd, never udhcpc"
newenv my355
mkstub iw 'case "$*" in "dev wlan0 link") echo "	SSID: home";; esac'
mkstub udhcpc 'exit 0'      # present, and must still not be chosen
mkstub dhcpcd 'exit 0'
check "dhcp client" "$(run dhcp-kind)" "dhcpcd"
run dhcp-start; sleep 0.2
called dhcpcd  && ok "dhcpcd used" || bad "dhcpcd not used"
called udhcpc  && bad "udhcpc ran alongside dhcpcd" || ok "udhcpc left alone"
run dhcp-stop
grep -q "dhcpcd -k wlan0" "$ROOT/calls" && ok "dhcpcd released by -k" || bad "dhcpcd not released"

echo
echo "== h700: no iw, no udhcpc - the reported failure"
newenv h700
mkstub dhclient 'exit 0'
mkstub wpa_cli 'case "$*" in *status*) echo "bssid=aa:bb"; echo "ssid=home"; echo "wpa_state=COMPLETED";; esac'
printf '#!/bin/sh\n' > "$SYSTEM_PATH/etc/wifi/wifi_init.sh"
check "assoc-ssid via wpa_cli with no iw" "$(run assoc-ssid)" "home"
run assoc home && ok "assoc works without iw" || bad "assoc failed without iw"
check "dhcp client" "$(run dhcp-kind)" "dhclient"
run dhcp-start; sleep 0.2
called dhclient && ok "dhclient used" || bad "dhclient not used"
check "wifi_init found under SYSTEM_PATH" "$(run wifi-init-path)" "$SYSTEM_PATH/etc/wifi/wifi_init.sh"
# 8821cs: nl80211 power save is accepted and ignored, so state is the driver's
# own ps_info and "off" is only reachable by reloading the module.
RTW="$ROOT/rtw"; mkdir -p "$RTW/net"
export NP_RTW_PS_INFO="$RTW/ps_info" NP_RTW_KO="$RTW/8821cs.ko" NP_SYS_NET="$RTW/net" \
	NP_RTW_RF="$RTW/no-rf" NP_RTW_SLOW_FLAG="$RTW/slow"
printf ' LPS mode: MAX\n' > "$RTW/ps_info"
check "power save read from the driver" "$(run powersave-get)" "on"
run powersave-needs-reload && ok "power save on needs a reload" || bad "reload not requested"
run powersave off && bad "runtime off claimed success on 8821cs" || ok "runtime off refused while in LPS"
mkstub iw 'echo "Power save: off"'
check "iw's answer is not trusted on 8821cs" "$(run powersave-get)" "on"
rm -f "$BIN/iw"
: > "$RTW/8821cs.ko"
mkstub sleep 'exit 0'
mkstub rfkill 'exit 0'
mkstub rmmod 'rm -rf "'"$RTW"'/net/wlan0"; printf " LPS mode: MAX\n" > "'"$RTW"'/ps_info"'
mkstub insmod 'mkdir -p "'"$RTW"'/net/wlan0"; case "$*" in *rtw_power_mgnt=0*) printf " LPS mode: NO LPS\n" > "'"$RTW"'/ps_info";; esac'
cat > "$SYSTEM_PATH/etc/wifi/wifi_init.sh" <<EOF
#!/bin/sh
echo "wifi_init \$1" >> "$ROOT/calls"
case "\$1" in start) touch "$ROOT/ip-up" ;; stop) rm -f "$ROOT/ip-up" ;; esac
EOF
: > "$ROOT/calls"
run powersave-reload off 2>/dev/null && ok "reload succeeded" || bad "reload failed"
check "stop, unload, load with rtw_power_mgnt=0, start - in that order" \
	"$(sed -n -e 's/^wifi_init \(.*\)/\1/p' -e 's/^rmmod .*/unload/p' \
		-e 's/^insmod .*rtw_power_mgnt=0$/load0/p' "$ROOT/calls" | tr '\n' ' ')" \
	"stop unload load0 start "
check "driver now reports power save off" "$(run powersave-get)" "off"
run powersave-needs-reload && bad "reload requested again" || ok "no second reload needed"
run powersave off && ok "runtime off is a no-op success once reloaded" || bad "off refused after reload"
# A radio that does not enumerate on the first load gets one longer power-off.
mkstub insmod 'n=$(cat "'"$RTW"'/tries" 2>/dev/null || echo 0); echo $((n+1)) > "'"$RTW"'/tries"
[ "$n" -ge 1 ] && mkdir -p "'"$RTW"'/net/wlan0"; exit 0'
: > "$ROOT/calls"
run powersave-reload off 2>/dev/null && ok "slow radio recovered on the retry" || bad "retry did not recover"
check "driver loaded twice for a slow radio" "$(cat "$RTW/tries")" "2"
mkstub insmod 'exit 1'
run powersave-reload off 2>/dev/null; check "radio that never returns is a failure" "$?" "1"
rm -f "$BIN/sleep" "$BIN/rmmod" "$BIN/insmod" "$BIN/rfkill"
printf '#!/bin/sh\n' > "$SYSTEM_PATH/etc/wifi/wifi_init.sh"
unset NP_RTW_PS_INFO NP_RTW_KO NP_SYS_NET NP_RTW_RF NP_RTW_SLOW_FLAG
# The regression this whole change exists for: a supplicant that reports
# ASSOCIATED but has not finished the handshake must not count as associated,
# or DHCP goes out into a gap where it is dropped.
mkstub wpa_cli 'case "$*" in *status*) echo "ssid=home"; echo "wpa_state=ASSOCIATED";; esac'
run assoc && bad "ASSOCIATED counted as ready" || ok "ASSOCIATED is not treated as ready"

echo
echo "== no iw: the bundled nl80211 helper stands in"
newenv my355
# Pointed at nothing: this checkout's own bin/ may hold a real build.
check "power save without iw or helper is a no-op" "$(NP_WIFIPS="$ROOT/none" run powersave off; echo $?)" "1"
check "power save state unknown without iw or helper" "$(NP_WIFIPS="$ROOT/none" run powersave-get; echo $?)" "1"
# Found next to the launcher when run as a program, which is how the app calls this file.
PAK="$ROOT/my355-pak"
mkdir -p "$PAK/launcher" "$PAK/bin/my355"
cp "$WP" "$PAK/launcher/"
cat > "$PAK/bin/my355/netplay-wifips.elf" <<EOF
#!/bin/sh
echo "wifips \$*" >> "$ROOT/calls"
[ -n "\${2:-}" ] && echo "\$2" > "$ROOT/ps-state"
cat "$ROOT/ps-state" 2>/dev/null || echo on
EOF
chmod 755 "$PAK/bin/my355/netplay-wifips.elf"
check "power save read through the helper" "$(sh "$PAK/launcher/wifi-platform.sh" powersave-get)" "on"
sh "$PAK/launcher/wifi-platform.sh" powersave off &&
	ok "power save set through the helper" || bad "helper power save failed"
called "wifips wlan0 off" && ok "helper given iface and state" || bad "helper not called with wlan0 off"
check "helper reports the new state" "$(sh "$PAK/launcher/wifi-platform.sh" powersave-get)" "off"
check "sourced callers find it through NETPLAY_PAK" \
	"$(NETPLAY_PAK="$PAK" sh -c '. "$1"; np_powersave_get' sh "$WP")" "off"
run powersave-needs-reload && bad "nl80211 platform asked for a reload" || ok "nl80211 platform never reloads"
mkstub iw 'case "$*" in *"get power_save"*) echo "Power save: on";; esac'
: > "$ROOT/calls"
check "iw still preferred where present" "$(sh "$PAK/launcher/wifi-platform.sh" powersave-get)" "on"
called wifips && bad "helper ran although iw exists" || ok "helper unused when iw exists"

echo
echo "== h700: ctrl_interface is read from -C, not guessed"
newenv h700
mkstub wpa_cli 'exit 1'
check "falls back to the platform's socket dir" "$(run ctrl-dir)" "/tmp/wifi/sockets"
newenv my282
mkstub wpa_cli 'exit 1'
check "my282 socket dir" "$(run ctrl-dir)" "/tmp/nextui-wifi"
newenv my355
mkstub wpa_cli 'exit 1'
check "my355 socket dir" "$(run ctrl-dir)" "/var/run/wpa_supplicant"

echo
echo "== ctrl_interface is discovered from a running supplicant"
# Fake /proc entries, one per spelling that occurs across the five platforms.
mkproc() {  # mkproc <pid> <comm> <arg>...
	_p="$ROOT/proc/$1"; shift
	mkdir -p "$_p"
	printf '%s\n' "$1" > "$_p/comm"; shift
	for _a in "$@"; do printf '%s\0' "$_a"; done > "$_p/cmdline"
}

newenv h700
rm -rf "$ROOT/proc"
mkproc 101 wpa_supplicant wpa_supplicant -B -i wlan0 -c /sd/wpa.conf -C /tmp/wifi/sockets
check "h700 -C split form" "$(NP_PROC=$ROOT/proc run ctrl-dir)" "/tmp/wifi/sockets"

newenv tg5040
rm -rf "$ROOT/proc"
mkproc 101 wpa_supplicant wpa_supplicant -B -Dnl80211 -iwlan0 -O/etc/wifi/sockets
check "tg5040 -O joined form" "$(NP_PROC=$ROOT/proc run ctrl-dir)" "/etc/wifi/sockets"

newenv my282
rm -rf "$ROOT/proc"
mkdir -p "$ROOT/conf"
printf 'ctrl_interface=/tmp/nextui-wifi\nupdate_config=1\n' > "$ROOT/conf/wpa.conf"
mkproc 101 wpa_supplicant wpa_supplicant -B -i wlan0 -c "$ROOT/conf/wpa.conf"
check "my282 -c split, read from the config" "$(NP_PROC=$ROOT/proc run ctrl-dir)" "/tmp/nextui-wifi"

echo
echo "== the supplicant DHCP hook is captured and replayed"
newenv h700
rm -rf "$ROOT/proc"
mkproc 101 wpa_supplicant wpa_supplicant -B -i wlan0 -C /tmp/wifi/sockets
mkproc 102 wpa_cli wpa_cli -B -p /tmp/wifi/sockets -i wlan0 -a /tmp/wifi/wpa_action.sh
NP_PROC=$ROOT/proc run hook-capture "$ROOT/hook" \
	&& ok "hook captured" || bad "hook not captured"
grep -q -- "-a /tmp/wifi/wpa_action.sh" "$ROOT/hook" \
	&& ok "hook records the action script" || bad "hook missing the action script"
# A wpa_cli with no -a is not part of bring-up and must not be replayed.
rm -rf "$ROOT/proc" "$ROOT/hook"
mkproc 103 wpa_cli wpa_cli -p /tmp/wifi/sockets -i wlan0 status
NP_PROC=$ROOT/proc run hook-capture "$ROOT/hook" \
	&& bad "a one-shot wpa_cli was captured as a hook" \
	|| ok "one-shot wpa_cli ignored"
# Every other platform has no such process: capture must simply say no.
newenv tg5040
rm -rf "$ROOT/proc"; mkdir -p "$ROOT/proc"
NP_PROC=$ROOT/proc run hook-capture "$ROOT/hook2" \
	&& bad "captured a hook that does not exist" || ok "no hook, no capture"

echo
echo "== unknown platform degrades to probing"
newenv somethingnew
mkstub iw 'case "$*" in "dev wlan0 link") echo "	SSID: home";; esac'
mkstub dhclient 'exit 0'
check "assoc still works" "$(run assoc-ssid)" "home"
check "picks whatever DHCP client exists" "$(run dhcp-kind)" "dhclient"
check "no wifi_init claimed" "$(run wifi-init-path)" ""

echo
echo "== missing expected client is reported, not silently skipped"
newenv h700
mkstub udhcpc 'exit 0'     # dhclient absent; only udhcpc present
err=$(run dhcp-kind 2>&1 >/dev/null)
case "$err" in
*"expects dhclient"*) ok "missing dhclient is reported" ;;
*) bad "missing dhclient was silent (got '$err')" ;;
esac
check "still falls back to something usable" "$(run dhcp-kind 2>/dev/null)" "udhcpc"

echo
echo "== assoc-kind reports what the device can actually do"
newenv tg5040
mkstub iw 'exit 0'
mkstub udhcpc 'exit 0'
check "tg5040 healthy" "$(run assoc-kind)" "iw"
newenv h700
mkstub wpa_cli 'exit 0'
mkstub dhclient 'exit 0'
check "h700 uses wpa_cli" "$(run assoc-kind)" "wpa_cli"
newenv h700               # a rootfs with none of the wifi tools at all
check "no probe at all is reported as none" "$(run assoc-kind)" "none"
check "no DHCP client at all is reported as none" "$(run dhcp-kind 2>/dev/null)" "none"
# The table names iw, but only wpa_cli exists: fall through rather than lie.
newenv tg5040
mkstub wpa_cli 'exit 0'
check "named probe missing, falls through" "$(run assoc-kind)" "wpa_cli"

echo
echo "== sourcing does not trigger the CLI dispatcher"
newenv tg5040
cat > "$ROOT/sourcer.sh" <<EOF
#!/bin/sh
. "$WP"
echo "sourced-ok:\$1"
EOF
chmod 755 "$ROOT/sourcer.sh"
check "source with args is inert" "$(sh "$ROOT/sourcer.sh" some-session-file 2>&1)" "sourced-ok:some-session-file"

echo
[ $fail -eq 0 ] && echo "all wifi-platform tests passed" || echo "wifi-platform tests FAILED"
exit $fail
