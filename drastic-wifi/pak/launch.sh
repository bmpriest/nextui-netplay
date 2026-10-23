#!/bin/sh
set -euxo pipefail
rm -f "$LOGS_PATH/NDS.txt"
exec >>"$LOGS_PATH/NDS.txt" 2>&1

echo "$0" "$@"

EMU_DIR="$SDCARD_PATH/Emus/$PLATFORM/NDS.pak/drastic"
PACK_DIR="$SDCARD_PATH/Emus/$PLATFORM/NDS.pak"
BRICK_DEVICE_DIR="$EMU_DIR/devices/trimui-brick"

SYSTEM_CPU_DIR="/sys/devices/system/cpu/cpufreq"
# NOTE:(2026-03-29 11:08:18 +07)Most low-end handled devices using share frequency on all core(policy0 affect all available cores). Setting everything here is more than enough
SYSTEM_CPU_POLICY0="$SYSTEM_CPU_DIR/policy0"
# NOTE:(2026-03-29 11:08:09 +07)Instead of hardcoding the min/max frequency, we can read it from the system then using awk to pick our desired frequency
AVAILABLE_CPU_FREQS=$(cat "$SYSTEM_CPU_POLICY0/scaling_available_frequencies")
MIN_CPU_FREQ=$(echo "$AVAILABLE_CPU_FREQS" | awk '{print $1}')
MAX_CPU_FREQ=$(echo "$AVAILABLE_CPU_FREQS" | awk '{print $NF}')

# NOTE:(2026-03-29 10:43:28 +07) After intense testing for best cpu freq, 1608000 come with perfect balance for efficient and performance. For any device with cpu freq below 1608000, we will use the max freq instead
PREFER_CPU_FREQ=$(echo "$AVAILABLE_CPU_FREQS" | awk '{for(i=1;i<=NF;i++) if($i<=1608000) val=$i} END{print val}')

NDS_MINUI_SAVE="$SDCARD_PATH/Saves/NDS"
NDS_MINUI_CHEAT="$SDCARD_PATH/Cheats/NDS"
NDS_USERDATA_NAME="NDS-advanced-drastic"
NDS_USERDATA_DIR="$USERDATA_PATH/$NDS_USERDATA_NAME"
NDS_SHARE_USERDATA_DIR="$SHARED_USERDATA_PATH/$NDS_USERDATA_NAME"

TEMP_PREFIX=system_
CPU_SCALING_GOVERNOR=scaling_governor
CPU_SCALING_MIN_FREQ=scaling_min_freq
CPU_SCALING_MAX_FREQ=scaling_max_freq
TEMP_SCALING_FILE="$NDS_USERDATA_DIR/$TEMP_PREFIX$CPU_SCALING_GOVERNOR.txt"
TEMP_SCALING_MIN_FREQ="$NDS_USERDATA_DIR/$TEMP_PREFIX$CPU_SCALING_MIN_FREQ.txt"
TEMP_SCALING_MAX_FREQ="$NDS_USERDATA_DIR/$TEMP_PREFIX$CPU_SCALING_MAX_FREQ.txt"

TEMP_ROM_DIR=$(mktemp -d /tmp/nds_rom_XXXXXX)

export PATH="$EMU_DIR:$PACK_DIR/bin:$PATH"
export LD_LIBRARY_PATH="$EMU_DIR/libs:$PACK_DIR/lib:$LD_LIBRARY_PATH"
export HOME="$EMU_DIR"

# NOTE: (2026-03-29 10:49:44 +07)For future researcher, if you have better idea for cpu governor, feel free to add it here. trngaje-advance-drastic current implementation with heavier game or normal game will never use that much cpu load(mostly highest will be ~50%) except when fast forward is toggle. Beside that, especially when using anything but performance governor, when you access menu and wait for a while(cool down cpu load, the current freq now will be the MIN_CPU_FREQ) and resume back, the game cpu freq will be stuck at that $MIN_CPU_FREQ until you reset the game -> stick to one freq and the highest one, which mean performance governor is the best match.
nds_cpu_configure() {
    echo "Custom setting for $1 governor"
    case $1 in
        performance)
            echo "$1" >"$SYSTEM_CPU_POLICY0/$CPU_SCALING_GOVERNOR" || true
            echo "$PREFER_CPU_FREQ" >"$SYSTEM_CPU_POLICY0/scaling_max_freq" || true
            ;;
        ondemand)
            echo "$1" >"$SYSTEM_CPU_POLICY0/$CPU_SCALING_GOVERNOR" || true
            echo "$MIN_CPU_FREQ" >"$SYSTEM_CPU_POLICY0/scaling_min_freq" || true
            echo "$PREFER_CPU_FREQ" >"$SYSTEM_CPU_POLICY0/scaling_max_freq" || true
            ;;
        *)
            echo "Unsupported governor: $1"
            ;;
    esac
}

nds_buffer_size_patch() {
    echo "Custom setting for $PLATFORM"
    case $PLATFORM in
        tg5040)
            export ALSA_CONFIG_PATH="$BRICK_DEVICE_DIR/alsa/nds_alsa.conf"
            export ALSA_ASOUNDRC="$BRICK_DEVICE_DIR/alsa/.asoundrc"
            ;;
        *)
            echo "Unsupported platform: $PLATFORM"
            ;;
    esac

}

# ---- local wireless (Netplay.pak) ---------------------------------------------
# With a Netplay session armed, DraStic's DS radio is carried to the other
# device by libdrastic_wifi.so (drastic-wifi/README.md): DSperate's Wi-Fi block
# and melonDS's LAN protocol, so either end can also be DSperate or melonDS.
# The host is the session's host; the guest joins it by the session's address.
NDS_NET_LAN=""
NDS_NET_SESSION=""

nds_netplay_env() {
    _np="${NETPLAY_PAK:-$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak}"
    _state="${NETPLAY_STATE:-$SDCARD_PATH/.userdata/shared/Netplay}"
    if [ ! -f "$_state/session" ]; then
        # Development: drastic/lan holds "host" or "join:<IPv4>" for a session
        # without the Netplay app.
        [ -f "$EMU_DIR/lan" ] && NDS_NET_LAN=$(cat "$EMU_DIR/lan")
        return 0
    fi
    [ -f "$_np/launcher/pre-launch.sh" ] || {
        echo "netplay: a session is armed but $_np is missing; playing offline"
        return 0
    }
    NETPLAY_PAK="$_np"
    NETPLAY_STATE="$_state"
    # pre-launch.sh is written for plain sh, not this script's -eu.
    set +eu
    # shellcheck source=/dev/null
    . "$_np/launcher/pre-launch.sh"
    set -eu
    [ -f "$NETPLAY_STATE/session" ] || return 0
    NDS_NET_SESSION="$NETPLAY_STATE/session"
    _role="$(sed -n 's/^role=//p' "$NDS_NET_SESSION" | head -n 1)"
    _peer="$(sed -n 's/^peer=//p' "$NDS_NET_SESSION" | head -n 1)"
    case "$_role" in
    host) NDS_NET_LAN=host ;;
    client)
        case "$_peer" in
        *[!0-9.]* | "")
            echo "netplay: session has no usable host address ($_peer); playing offline"
            return 0
            ;;
        esac
        NDS_NET_LAN="join:$_peer"
        ;;
    *)
        echo "netplay: unknown session role '$_role'; playing offline"
        return 0
        ;;
    esac
    # One MP exchange may hold a frame this long: 25 ms (melonDS's) holds on
    # Netplay's ad hoc network; through an access point it takes 100 ms (as
    # DSP.pak measured). The session may carry its own value.
    _timeout="$(sed -n 's/^dsp_lan_timeout_ms=//p' "$NDS_NET_SESSION" | head -n 1)"
    if [ -z "$_timeout" ] && ! grep -q '^adhoc_ssid=' "$NDS_NET_SESSION"; then
        _timeout=100
    fi
    case "$_timeout" in
    "" | *[!0-9]*) ;;
    *) export DRASTIC_WIFI_LAN_TIMEOUT_MS="$_timeout" ;;
    esac
    if grep -q '^verbose_logs=1$' "$NDS_NET_SESSION" 2>/dev/null; then
        export DS_LAN_LOG=1
    fi
    echo "netplay: $_role session, DraStic radio $NDS_NET_LAN"
}

# Every MP exchange blocks the emulation thread on the network, and a
# utilisation-driven governor reads those waits as idle and clocks down (a
# third of the speed, measured with DSperate). A session runs at the top
# frequency on the performance governor; cleanup() restores what was there.
nds_netplay_cpu() {
    echo performance >"$SYSTEM_CPU_POLICY0/$CPU_SCALING_GOVERNOR" || true
    echo "$MAX_CPU_FREQ" >"$SYSTEM_CPU_POLICY0/$CPU_SCALING_MAX_FREQ" || true
}

# NextUI truncates the pak log on every launch. With Netplay's verbose logs on,
# keep a copy per process as Netplay.pak's other cores do:
# logs/netplay-games/<game>/<session id>/<time>-<role>-<pid>.log.
nds_archive_netplay_log() {
    [ -n "$NDS_NET_SESSION" ] || return 0
    grep -q '^verbose_logs=1$' "$NDS_NET_SESSION" 2>/dev/null || return 0
    _game="$(basename "${ROM_PATH:-}" | sed 's/[^A-Za-z0-9._-]/_/g')"
    _sid="$(sed -n 's/^session_id=//p' "$NDS_NET_SESSION" | head -n 1)"
    _role="$(sed -n 's/^role=//p' "$NDS_NET_SESSION" | head -n 1)"
    case "$_sid" in *[!0-9A-Fa-f]* | "") _sid=unknown-session ;; esac
    case "$_role" in host | client) ;; *) _role=unknown ;; esac
    _dir="$USERDATA_PATH/logs/netplay-games/${_game:-unknown-game}/$_sid"
    mkdir -p "$_dir" 2>/dev/null || return 0
    cp "$LOGS_PATH/NDS.txt" "$_dir/$(date +%Y%m%d-%H%M%S)-$_role-$$.log" 2>/dev/null || :
}

cleanup() {
    rm -f /tmp/stay_awake

    if [ -n "$TEMP_ROM_DIR" ] && [ -d "$TEMP_ROM_DIR" ]; then
        rm -rf "$TEMP_ROM_DIR"
    fi

    if [ -f "$TEMP_SCALING_FILE" ]; then
        cat "$TEMP_SCALING_FILE" >"$SYSTEM_CPU_POLICY0/$CPU_SCALING_GOVERNOR" || true
        rm -f "$TEMP_SCALING_FILE"
    fi
    if [ -f "$TEMP_SCALING_MIN_FREQ" ]; then
        cat "$TEMP_SCALING_MIN_FREQ" >"$SYSTEM_CPU_POLICY0/$CPU_SCALING_MIN_FREQ" || true
        rm -f "$TEMP_SCALING_MIN_FREQ"
    fi
    if [ -f "$TEMP_SCALING_MAX_FREQ" ]; then
        cat "$TEMP_SCALING_MAX_FREQ" >"$SYSTEM_CPU_POLICY0/$CPU_SCALING_MAX_FREQ" || true
        rm -f "$TEMP_SCALING_MAX_FREQ"
    fi

    umount "$EMU_DIR/backup" || true
    umount "$EMU_DIR/cheats" || true
    umount "$EMU_DIR/savestates" || true
    nds_archive_netplay_log || true
}

main() {
    echo "1" >/tmp/stay_awake
    trap "cleanup" EXIT INT TERM HUP QUIT

    # Create all required directories if they don't exist
    mkdir -p "$NDS_MINUI_SAVE"
    mkdir -p "$NDS_MINUI_CHEAT"
    mkdir -p "$NDS_USERDATA_DIR"
    mkdir -p "$NDS_SHARE_USERDATA_DIR"
    mkdir -p "$EMU_DIR/backup"
    mkdir -p "$EMU_DIR/savestates"

    cat "$SYSTEM_CPU_POLICY0/$CPU_SCALING_GOVERNOR" >"$TEMP_SCALING_FILE"
    cat "$SYSTEM_CPU_POLICY0/$CPU_SCALING_MIN_FREQ" >"$TEMP_SCALING_MIN_FREQ"
    cat "$SYSTEM_CPU_POLICY0/$CPU_SCALING_MAX_FREQ" >"$TEMP_SCALING_MAX_FREQ"

    # Predefined cpu profile for drastic
    nds_cpu_configure ondemand
    nds_netplay_env
    [ -n "$NDS_NET_LAN" ] && nds_netplay_cpu
    nds_buffer_size_patch

    if [ -d "$EMU_DIR/cheats" ]; then
        if ls -A "$EMU_DIR/cheats" | grep -q .; then
            mv "$EMU_DIR/cheats/*" "$NDS_MINUI_CHEAT/" || true
        fi
    fi

    mount -o bind "$NDS_MINUI_SAVE" "$EMU_DIR/backup"
    mount -o bind "$NDS_MINUI_CHEAT" "$EMU_DIR/cheats"
    mount -o bind "$NDS_SHARE_USERDATA_DIR" "$EMU_DIR/savestates"

    # Extract zip ROMs to a temp directory before launching
    ROM_PATH="$*"
    case "$(echo "$ROM_PATH" | tr '[:upper:]' '[:lower:]')" in
        *.zip)
            "$PACK_DIR/bin/unzip" -o "$ROM_PATH" -d "$TEMP_ROM_DIR"
            ROM_PATH=$(find "$TEMP_ROM_DIR" -name "*.nds" | head -1)
            ;;
    esac

    # Trigger custom minui-power-control and launch the emulator, make sure to be in the current directory
    cd "$EMU_DIR"
    minui-power-control drastic &
    if [ -n "$NDS_NET_LAN" ] && [ -f "$EMU_DIR/libs/libdrastic_wifi.so" ]; then
        # The auto state is where this console last quit, not where the other
        # one is: a session boots the game fresh (slots 1+ are kept).
        _rom_name=$(basename "$ROM_PATH")
        rm -f "$NDS_SHARE_USERDATA_DIR/${_rom_name%.*}_0.dss"
        DRASTIC_WIFI_LAN="$NDS_NET_LAN" DRASTIC_WIFI_LAN_NAME=Netplay \
            LD_PRELOAD="$EMU_DIR/libs/libdrastic_wifi.so:$EMU_DIR/libs/libadvdrastic.so" \
            "$EMU_DIR/drastic" "$ROM_PATH"
    else
        LD_PRELOAD=$EMU_DIR/libs/libadvdrastic.so "$EMU_DIR/drastic" "$ROM_PATH"
    fi
}

main "$@"
