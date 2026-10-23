#!/bin/sh
PAK_DIR="$(dirname "$0")"
PAK_NAME="$(basename "$PAK_DIR")"
PAK_NAME="${PAK_NAME%.*}"

SUPPORTED_PLATFORMS="tg5040 tg5050 h700"

# Whole word match against a space separated list. A plain `grep -q "$PLATFORM"`
# also accepts every substring, so rg35xx and tg50 would both pass.
platform_in_list() {
    case " $2 " in
    *" $1 "*)
        return 0
        ;;
    esac

    return 1
}

# NextUI reports the Trimui Brick as tg5040 with DEVICE=brick. Older MinUI builds
# reported it as its own tg3040 platform with no DEVICE at all.
dsp_normalize_platform() {
    if [ "${PLATFORM:-}" = "tg3040" ] && [ -z "${DEVICE:-}" ]; then
        PLATFORM="tg5040"
        DEVICE="brick"
        export PLATFORM DEVICE
    fi
}

dsp_init_env() {
    BIN_DIR="$PAK_DIR/$PLATFORM"

    DSP_USERDATA_DIR="$USERDATA_PATH/DSP-dsperate"
    DSP_CONFIG_DIR="$DSP_USERDATA_DIR/dsperate"
    DSP_CONFIG="$DSP_CONFIG_DIR/dsperate.ini"
    DSP_DEVICE_FILE="$DSP_USERDATA_DIR/device.txt"

    DSP_SAVES_DIR="$SDCARD_PATH/Saves/DSP"
    DSP_STATES_DIR="$SHARED_USERDATA_PATH/DSP-dsperate"
    DSP_SCREENSHOTS_DIR="$SDCARD_PATH/Screenshots"
    DSP_CHEATS_DIR="$SDCARD_PATH/Cheats/DSP"
    DSP_BIOS_DIR="$BIOS_PATH/DSP"
    DSP_ROMS_DIR="$SDCARD_PATH/Roms/Nintendo DS (DSP)"

    # DSperate reads its ini from $XDG_CONFIG_HOME/dsperate/, creating the
    # directory and a games/ beside it (Config::dir in src/frontend/sdl).
    export HOME="$DSP_USERDATA_DIR"
    export XDG_CONFIG_HOME="$DSP_USERDATA_DIR"

    export PATH="$BIN_DIR:$PAK_DIR:$PATH"

    # What the launcher's game switcher reads. The key everywhere is the ROM's
    # basename *with* its extension, which is what minarch itself keys on.
    MINUI_SHARED_DIR="$SHARED_USERDATA_PATH/.minui"
    MINUI_DIR="$MINUI_SHARED_DIR/DSP"
    export MINUI_SHARED_DIR MINUI_DIR
    MINUI_ROM_FILE="$(basename "$ROM_PATH")"
    export MINUI_ROM_FILE
    # auto_resume.txt holds the path relative to the card, and the frontend
    # rebuilds it with a bare concat. An absolute path here becomes
    # /mnt/SDCARD/mnt/SDCARD/... and the resume silently never happens, so it
    # is better to export nothing than to export something wrong.
    case "$ROM_PATH" in
    "$SDCARD_PATH"/*)
        export MINUI_ROM_PATH="${ROM_PATH#"$SDCARD_PATH"}"
        ;;
    *)
        echo "ROM is outside $SDCARD_PATH, so auto-resume is unavailable: $ROM_PATH"
        ;;
    esac

    # Bundled first, then the SDL2 and friends NextUI installs, then whatever the
    # platform profile adds (/usr/trimui/lib on TrimUI), then the inherited value.
    DSP_LD_PATH="$BIN_DIR:$SDCARD_PATH/.system/$PLATFORM/lib"
    for dir in $PROFILE_LD_DIRS; do
        DSP_LD_PATH="$DSP_LD_PATH:$dir"
    done
    export LD_LIBRARY_PATH="$DSP_LD_PATH:${LD_LIBRARY_PATH:-}"
}

# DSperate opens the pad with SDL_IsGameController / SDL_GameControllerOpen and
# has no joystick fallback, and MinUI ships no GameController mapping, so
# without this there is no input at all. The GUID is read from the device
# because SDL derives it from evdev ids and hashes it differently across SDL
# versions; the mapping body comes from the platform profile.
dsp_export_pad_mapping() {
    if [ -z "$PROFILE_MAP" ]; then
        echo "No pad mapping for $PLATFORM/${DEVICE:-}, leaving SDL_GAMECONTROLLERCONFIG alone"
        return 0
    fi

    _guid_out="$("$BIN_DIR/dsp-pad-guid" 2>&1)" || {
        echo "dsp-pad-guid found no pad; DSperate will start without controller input"
        echo "$_guid_out"
        return 0
    }

    # One mapping per pad we can vouch for. The built-in pad is the first line.
    # DevBridge's virtual controller copies its ids and button numbering on
    # TrimUI, so it gets the same body; on H700 it is a second "ANBERNIC-keys"
    # with its own numbering. Other pads are left to SDL's own database.
    # DSperate opens every mapped pad.
    _cfg=""
    _first=""
    _tab="$(printf '\t')"
    while IFS="$_tab" read -r _guid _name; do
        [ -n "$_guid" ] || continue
        [ -n "$_name" ] || _name="Controller"
        _map="$PROFILE_MAP"
        if [ -z "$_first" ]; then
            _first="$_guid"
        elif [ "$_guid" = "$_first" ]; then
            continue
        elif [ "${_guid#????????}" = "${_first#????????}" ]; then
            # Same bus, vendor, product and version, different name: on TrimUI
            # that is DevBridge's virtual pad, which copies the built-in one.
            :
        elif [ -n "${PROFILE_MAP_VIRTUAL:-}" ] && [ "$_name" = "ANBERNIC-keys" ]; then
            # The built-in pad was the first line, so a second one by this name
            # is DevBridge's virtual pad.
            _map="$PROFILE_MAP_VIRTUAL"
        else
            echo "pad: leaving $_guid ($_name) to SDL's own mappings"
            continue
        fi

        # NextUI h700-rc11 and later give every H700 model the TrimUI numbers
        # and say so through the GUID. Its SDL also carries a built-in mapping
        # for that GUID, but this one takes priority and matches the TrimUI
        # pads, so it is still exported rather than left to SDL.
        if [ -n "${PROFILE_MAP_FIXED:-}" ] && [ "$_guid" = "$DSP_H700_FIXED_GUID" ]; then
            _map="$PROFILE_MAP_FIXED"
            echo "NextUI fixed h700 pad layout, using the TrimUI button numbers"
        fi
        _line="$_guid,$_name,$_map,platform:Linux"
        _cfg="${_cfg:+$_cfg
}$_line"
        echo "pad mapping: $_guid ($_name)"
    done <<EOF_PADS
$_guid_out
EOF_PADS

    [ -n "$_cfg" ] && export SDL_GAMECONTROLLERCONFIG="$_cfg"
}

# Seed once per profile so remapping done in DSperate's pause menu survives later
# launches. The runtime paths cannot live in the shipped template because
# SDCARD_PATH and friends are only known here.
dsp_seed_config() {
    _template="$PAK_DIR/configs/$PROFILE_INI.ini"

    if [ ! -f "$_template" ]; then
        echo "No config template named $PROFILE_INI, leaving the existing config alone"
        return 0
    fi

    # Bumped when the templates change in a way an existing config must pick
    # up: 2 made the pad mappings positional and turned face_fix off.
    _seed="$PROFILE_INI@2"
    if [ -f "$DSP_DEVICE_FILE" ] && [ "$(cat "$DSP_DEVICE_FILE")" = "$_seed" ]; then
        return 0
    fi

    echo "Seeding DSperate config from the $PROFILE_INI profile"

    if [ -f "$DSP_CONFIG" ]; then
        cp -f "$DSP_CONFIG" "$DSP_USERDATA_DIR/dsperate.ini.bak"
    fi

    sed \
        -e "s|__BIOS_PATH__|$DSP_BIOS_DIR|g" \
        -e "s|__ROMS_PATH__|$DSP_ROMS_DIR|g" \
        -e "s|__SAVES_PATH__|$DSP_SAVES_DIR|g" \
        -e "s|__STATES_PATH__|$DSP_STATES_DIR|g" \
        -e "s|__SCREENSHOTS_PATH__|$DSP_SCREENSHOTS_DIR|g" \
        -e "s|__CHEATS_PATH__|$DSP_CHEATS_DIR/usrcheat.dat|g" \
        "$_template" >"$DSP_CONFIG"

    echo "$_seed" >"$DSP_DEVICE_FILE"
    sync
}

# The launcher asks for a state through /tmp/resume_slot.txt, and the file is
# consumed. Its slots are minarch's: 0-7 are the player's, 8 means "plain
# launch, start fresh", 9 is the state written before a sleep -- which here is
# DSperate's own hidden auto slot, so autoload already resumes it.
#
# This matters because the shipped config has autoload on: without translating
# the marker, picking a game fresh from the launcher would resume it anyway.
dsp_resume_args() {
    DSP_RESUME_ARGS=""
    [ -f /tmp/resume_slot.txt ] || return 0

    _slot="$(cat /tmp/resume_slot.txt 2>/dev/null | tr -dc '0-9')"
    rm -f /tmp/resume_slot.txt

    case "$_slot" in
    "" | 9)
        # Let autoload pick up the auto slot.
        ;;
    8)
        DSP_RESUME_ARGS="--no-autoload"
        ;;
    *)
        _stem="$(basename "$ROM_PATH")"
        _stem="${_stem%.*}"
        DSP_RESUME_ARGS="--load-state $DSP_STATES_DIR/$_stem.$_slot.dss"
        ;;
    esac
}

# Netplay.pak integration. With a Netplay session armed, put this console back
# on the session's network (pre-launch.sh re-joins an ad hoc host) and start
# DSperate's local wireless as the session's host or guest: the host's DS
# answers on the LAN in melonDS's protocol, and the guest joins it by address.
# DSperate keeps a guest trying until the host's emulator is up, so the two
# devices can launch in either order. Nothing here runs without a session.
dsp_netplay_args() {
    DSP_NET_ARGS=""
    DSP_NET_SESSION=""
    _np="${NETPLAY_PAK:-$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak}"
    _state="${NETPLAY_STATE:-$SDCARD_PATH/.userdata/shared/Netplay}"
    [ -f "$_state/session" ] || return 0
    [ -f "$_np/launcher/pre-launch.sh" ] || {
        echo "netplay: a session is armed but $_np is missing; playing offline"
        return 0
    }

    NETPLAY_PAK="$_np"
    NETPLAY_STATE="$_state"
    # shellcheck source=/dev/null
    . "$_np/launcher/pre-launch.sh"
    # session-cleanup.sh (run by pre-launch.sh) drops a previous boot's session.
    [ -f "$NETPLAY_STATE/session" ] || return 0
    DSP_NET_SESSION="$NETPLAY_STATE/session"

    _role="$(sed -n 's/^role=//p' "$DSP_NET_SESSION" | head -n 1)"
    _peer="$(sed -n 's/^peer=//p' "$DSP_NET_SESSION" | head -n 1)"
    case "$_role" in
    host)
        DSP_NET_ARGS="--lan-host Netplay"
        ;;
    client)
        # The app wrote it; checked again because it reaches a command line.
        case "$_peer" in
        *[!0-9.]* | "")
            echo "netplay: session has no usable host address ($_peer); playing offline"
            return 0
            ;;
        esac
        DSP_NET_ARGS="--lan-join $_peer"
        ;;
    *)
        echo "netplay: unknown session role '$_role'; playing offline"
        return 0
        ;;
    esac
    # The auto state is where this console was when it last quit, not where the
    # other one is: a session boots fresh unless the player picked a slot.
    [ -n "$DSP_RESUME_ARGS" ] || DSP_RESUME_ARGS="--no-autoload"
    echo "netplay: $_role session, DSperate args: $DSP_NET_ARGS $DSP_RESUME_ARGS"
}

# NextUI truncates this pak's log on every launch, so with Netplay's verbose
# logs on, keep a copy per process the way Netplay.pak's minarch wrapper does:
# logs/netplay-games/<game>/<session id>/<time>-<role>-<pid>.log.
dsp_archive_netplay_log() {
    [ -n "${DSP_NET_SESSION:-}" ] || return 0
    grep -q '^verbose_logs=1$' "$DSP_NET_SESSION" 2>/dev/null || return 0
    _game="$(basename "$ROM_PATH" | sed 's/[^A-Za-z0-9._-]/_/g')"
    _sid="$(sed -n 's/^session_id=//p' "$DSP_NET_SESSION" | head -n 1)"
    _role="$(sed -n 's/^role=//p' "$DSP_NET_SESSION" | head -n 1)"
    case "$_sid" in *[!0-9A-Fa-f]* | "") _sid=unknown-session ;; esac
    case "$_role" in host | client) ;; *) _role=unknown ;; esac
    _dir="$USERDATA_PATH/logs/netplay-games/${_game:-unknown-game}/$_sid"
    mkdir -p "$_dir" 2>/dev/null || return 0
    cp "$LOGS_PATH/$PAK_NAME.txt" "$_dir/$(date +%Y%m%d-%H%M%S)-$_role-$$.log" 2>/dev/null || :
}

cleanup() {
    rm -f /tmp/stay_awake

    # DSperate writes the battery save on SIGTERM, so -9 straight away loses the
    # last save. Give it a moment before insisting.
    killall -q -15 dsperate 2>/dev/null || true
    sleep 2
    killall -q -9 dsperate 2>/dev/null || true

    dsp_archive_netplay_log
    sync
}

main() {
    set -x

    rm -f "$LOGS_PATH/$PAK_NAME.txt"
    exec >>"$LOGS_PATH/$PAK_NAME.txt"
    exec 2>&1

    echo "$0" "$@"

    dsp_normalize_platform

    if ! platform_in_list "$PLATFORM" "$SUPPORTED_PLATFORMS"; then
        echo "$PLATFORM is not a supported platform"
        return 1
    fi

    # Before dsp_init_env, which derives the launcher's file keys from it.
    # Some builds hand the pak a /media/SDCARD0 path for the same card, and a
    # symlinked Roms folder is common, so resolve both: the MinUI artifacts are
    # keyed on this and the launcher will not match a path it did not write.
    ROM_PATH="$(echo "$1" | sed "s|/media/SDCARD0/|$SDCARD_PATH/|g")"
    if [ -e "$ROM_PATH" ]; then
        ROM_PATH="$(readlink -f "$ROM_PATH")"
        # On H700 /mnt/SDCARD is itself a symlink (to /mnt/sdcard), so the
        # resolved path has to be put back under the name the launcher uses.
        _card="$(readlink -f "$SDCARD_PATH")"
        case "$ROM_PATH" in
        "$_card"/*) ROM_PATH="$SDCARD_PATH${ROM_PATH#"$_card"}" ;;
        esac
    fi

    # shellcheck source=/dev/null
    . "$PAK_DIR/platform.sh"
    dsp_platform_profile "$PLATFORM" "${DEVICE:-}"

    dsp_init_env

    echo "1" >/tmp/stay_awake
    trap "cleanup" EXIT INT TERM HUP QUIT

    mkdir -p "$DSP_USERDATA_DIR" "$DSP_CONFIG_DIR" "$DSP_SAVES_DIR" \
        "$DSP_STATES_DIR" "$DSP_SCREENSHOTS_DIR" "$DSP_CHEATS_DIR" "$DSP_BIOS_DIR" \
        "$MINUI_SHARED_DIR" "$MINUI_DIR"

    dsp_seed_config
    dsp_export_pad_mapping

    # Only on a panel whose rotation SDL cannot tell DSperate about; the fbdev
    # tier writes /dev/fb0 directly. Empty everywhere else.
    if [ -n "$PROFILE_ROTATE" ]; then
        export DS_ROTATE="$PROFILE_ROTATE"
    fi

    dsp_resume_args
    dsp_netplay_args

    # No --config: the seeded file is already at the default path DSperate
    # computes from XDG_CONFIG_HOME. No layout or performance flags either --
    # flags override the ini, which would make them uneditable from the pause
    # menu.
    cd "$BIN_DIR" || return 1
    # shellcheck disable=SC2086 # deliberate word lists
    ./dsperate "$ROM_PATH" --fullscreen $DSP_RESUME_ARGS $DSP_NET_ARGS
}

if [ "${DSP_PAK_TEST:-}" != "1" ]; then
    main "$@"
fi
