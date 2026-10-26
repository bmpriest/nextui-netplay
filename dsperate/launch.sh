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
    MINUI_ROM_FILE="${ROM_PATH##*/}"
    export MINUI_ROM_FILE
    # The profile the config was seeded from, which the pause menu's
    # Shortcuts page restores its defaults from.
    export MINUI_PROFILE_INI="$PAK_DIR/configs/$PROFILE_INI.ini"
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
# without one there is no input at all. The mapping body comes from the
# platform profile; DSperate puts the pad's GUID in front of it as it starts
# (minui_pad.h), because SDL derives the GUID from evdev ids and hashes it
# differently across SDL versions. Reading it here instead would mean
# starting SDL twice per launch.
dsp_export_pad_mapping() {
    if [ -z "$PROFILE_MAP" ]; then
        echo "No pad mapping for $PLATFORM/${DEVICE:-}, leaving SDL_GAMECONTROLLERCONFIG alone"
        return 0
    fi

    MINUI_PAD_MAP="$PROFILE_MAP"
    # NextUI h700-rc11 and later give every H700 model the TrimUI numbers and
    # say so through the GUID, which then takes this body instead. Its SDL
    # also carries a built-in mapping for that GUID, but the pak's takes
    # priority and matches the TrimUI pads.
    MINUI_PAD_MAP_FIXED="${PROFILE_MAP_FIXED:-}"
    MINUI_PAD_FIXED_GUID="$DSP_H700_FIXED_GUID"
    export MINUI_PAD_MAP MINUI_PAD_MAP_FIXED MINUI_PAD_FIXED_GUID

    # What to call the controls the device labels its own way: the Brick's
    # F1 and F2 are SDL's leftstick and rightstick.
    MINUI_BUTTON_LABELS="${PROFILE_LABELS:-}"
    export MINUI_BUTTON_LABELS
}

# A one-off hunt for a device's spare buttons. With probe.txt beside
# device.txt, every control pressed in the next 15 seconds is logged before
# the game starts, and the trigger is used up.
dsp_probe_buttons() {
    [ -f "$DSP_USERDATA_DIR/probe.txt" ] || return 0
    rm -f "$DSP_USERDATA_DIR/probe.txt"
    echo "Probing the controls for 15 seconds"
    "$BIN_DIR/dsp-pad-guid" --probe 15 || echo "dsp-pad-guid could not probe the controls"
}

# The Visual FX values the pause menu marks as slow on this platform, and,
# with shader-timing.txt beside device.txt, DSperate logging what COLOUR and
# PATTERN cost a frame. The trigger stays, so it can be timed over several games.
dsp_export_shaders() {
    MINUI_SLOW_SHADERS="${PROFILE_SLOW_SHADERS:-}"
    export MINUI_SLOW_SHADERS
    if [ -f "$DSP_USERDATA_DIR/shader-timing.txt" ]; then
        MINUI_SHADER_TIMING=1
        export MINUI_SHADER_TIMING
    fi
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

    if [ -f "$DSP_DEVICE_FILE" ]; then
        _seeded=""
        read -r _seeded <"$DSP_DEVICE_FILE" || true
        if [ "$_seeded" = "$PROFILE_INI" ]; then
            return 0
        fi
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

    echo "$PROFILE_INI" >"$DSP_DEVICE_FILE"
    sync
}

# The profiles are seeded once, so a config from an older pak gains what the
# profiles have since added, once:
#   - a bare MENU tap opens the pause menu, as it does in minarch;
#   - MENU+START moves the pip screen, unless MENU+START is already taken,
#     which a config with no pause line counts as, upstream's pause being
#     mod+start;
#   - the state and slot chords are off, as they ship now;
#   - the mic blow is off, rather than upstream's default of L3, which the
#     stick profiles also give the pen's tap.
# Anything the player set for these keys themselves is left alone, and a key
# under [hotkeys] is the keyboard's and does not count. One awk pass, which
# leaves the file untouched when there is nothing to add: this runs on every
# launch, and each process costs the slower devices tens of milliseconds.
dsp_migrate_config() {
    [ -f "$DSP_CONFIG" ] || return 0

    if awk -v out="$DSP_CONFIG.tmp" -v path="$DSP_CONFIG" '
        function trim(s) { gsub(/^[[:space:]]+|[[:space:]]+$/, "", s); return s }
        function want(k, v) {
            if (k in has) return
            has[k] = 1
            adds[++m] = k " = " v
        }
        { lines[++n] = $0 }
        /^[[:space:]]*\[/ {
            section = $0
            gsub(/[[:space:]]/, "", section)
            if (section == "[padhotkeys]" && !header) header = n
            next
        }
        section == "[padhotkeys]" && index($0, "=") {
            eq = index($0, "=")
            has[trim(substr($0, 1, eq - 1))] = 1
            taken[trim(substr($0, eq + 1))] = 1
        }
        END {
            want("pause.alt", "mod")
            if (("pause" in has) && !("mod+start" in taken)) want("pip_corner_next", "mod+start")
            want("save_state", "none")
            want("load_state", "none")
            want("slot_next", "none")
            want("slot_prev", "none")
            want("mic", "none")
            if (!m) exit 1
            for (i = 1; i <= n; i++) {
                print lines[i] > out
                if (i == header) for (j = 1; j <= m; j++) print adds[j] > out
            }
            if (!header) {
                print "" > out
                print "[padhotkeys]" > out
                for (j = 1; j <= m; j++) print adds[j] > out
            }
            for (j = 1; j <= m; j++) printf "Adding %s under [padhotkeys] in %s\n", adds[j], path
        }
    ' "$DSP_CONFIG"; then
        mv -f "$DSP_CONFIG.tmp" "$DSP_CONFIG"
    fi
}

# The pause menu dresses itself as the launcher that started it. NextUI keeps
# its theme in minuisettings.txt and ships font1.ttf/font2.ttf; stock MinUI
# has neither and uses BPreplay. Exports MINUI_STYLE, MINUI_FONT (empty means
# the menu's built-in font) and, on NextUI, MINUI_COLOR1..7.
dsp_detect_style() {
    _res="$SDCARD_PATH/.system/res"
    _settings="$SHARED_USERDATA_PATH/minuisettings.txt"
    MINUI_STYLE="minui"
    MINUI_FONT=""
    # NextUI's "Screenshot saved" notice switch, on unless it says 0. Stock
    # MinUI has no such setting, so the notice always shows there.
    MINUI_NOTIFY_SCREENSHOT=1

    if [ -f "$_settings" ] && [ -f "$_res/font1.ttf" ]; then
        MINUI_STYLE="nextui"
        # Read in the shell, last line winning as NextUI's own reader does,
        # rather than a sed pipeline per key: this runs on every launch.
        _font=""
        _notify=""
        for _i in 1 2 3 4 5 6 7; do
            eval "_colour$_i="
        done
        _cr="$(printf '\r')"
        while IFS= read -r _line || [ -n "$_line" ]; do
            _line="${_line%"$_cr"}"
            case "$_line" in
            font=*) _font="${_line#font=}" ;;
            notifyScreenshot=*) _notify="${_line#notifyScreenshot=}" ;;
            color[1-7]=*)
                _i="${_line%%=*}"
                eval "_colour${_i#color}=\${_line#*=}"
                ;;
            esac
        done <"$_settings"

        # NextUI's own reading of font=: 0 is font2 and 1 is font1 (older
        # releases wrote the number), anything else is a file name.
        case "$_font" in
        0) _font="font2.ttf" ;;
        "" | 1) _font="font1.ttf" ;;
        esac
        [ -f "$_res/$_font" ] || _font="font1.ttf"
        MINUI_FONT="$_res/$_font"

        if [ "$_notify" = "0" ]; then
            MINUI_NOTIFY_SCREENSHOT=0
        fi

        for _i in 1 2 3 4 5 6 7; do
            eval "_colour=\$_colour$_i"
            if [ -n "$_colour" ]; then
                eval "MINUI_COLOR$_i=\$_colour"
                export "MINUI_COLOR$_i"
            fi
        done
    elif [ -f "$_res/BPreplayBold-unhinted.otf" ]; then
        MINUI_FONT="$_res/BPreplayBold-unhinted.otf"
    fi

    export MINUI_STYLE MINUI_FONT MINUI_NOTIFY_SCREENSHOT
    echo "pause menu: $MINUI_STYLE look, font ${MINUI_FONT:-built-in}"
}

# The launcher asks for a state through /tmp/resume_slot.txt, and the file is
# consumed. Its slots are minarch's: 0-7 are the player's, 8 means "plain
# launch, start fresh", 9 is the state written before a sleep -- which here is
# DSperate's own hidden auto slot, so autoload already resumes it.
#
# This matters because the shipped config has autoload on: without translating
# the marker, picking a game fresh from the launcher would resume it anyway.
#
# The state path is kept apart from the flag so dsp_run_emulator can pass it as
# one argument. ROM names nearly always hold spaces, and DSperate takes every
# stray word as the ROM. A slot whose state is gone (a stale marker, a deleted
# file) or is in a format this DSperate no longer reads falls back to autoload:
# an explicit --load-state that cannot be read fails the whole run.
dsp_resume_args() {
    DSP_RESUME_FLAG=""
    DSP_LOAD_STATE=""
    [ -f /tmp/resume_slot.txt ] || return 0

    _slot="$(cat /tmp/resume_slot.txt 2>/dev/null | tr -dc '0-9')"
    rm -f /tmp/resume_slot.txt

    case "$_slot" in
    "" | 9)
        # Let autoload pick up the auto slot.
        ;;
    8)
        DSP_RESUME_FLAG="--no-autoload"
        ;;
    *)
        _stem="$(basename "$ROM_PATH")"
        _stem="${_stem%.*}"
        _state="$DSP_STATES_DIR/$_stem.$_slot.dss"
        if [ ! -f "$_state" ]; then
            echo "resume state $_state not found, falling back to autoload"
        elif dsp_state_readable "$_state"; then
            DSP_LOAD_STATE="$_state"
        fi
        ;;
    esac
}

# Whether this build of DSperate can read a save state. A state is "DSST" and
# a little-endian u32 format version; DSperate refuses anything older than the
# version recorded in dsperate.state-version at build time, and a refused
# --load-state fails the run, so the slot falls back to autoload instead.
# Only a definite mismatch says no: without the recorded version, or without
# an od that can read the header, the state is passed through as before.
dsp_state_readable() {
    _min="$(cat "$PAK_DIR/dsperate.state-version" 2>/dev/null | tr -dc '0-9')"
    [ -n "$_min" ] || return 0

    if [ "$(head -c 4 "$1" 2>/dev/null)" != "DSST" ]; then
        echo "resume state $1 is not a DSperate state, falling back to autoload"
        return 1
    fi

    # The format version is small, so its low byte is the whole value.
    _ver="$(od -An -tu1 -j4 -N1 "$1" 2>/dev/null | tr -dc '0-9')"
    [ -n "$_ver" ] || return 0
    if [ "$_ver" -lt "$_min" ]; then
        echo "resume state $1 predates this DSperate (format $_ver, needs $_min), falling back to autoload"
        return 1
    fi
    return 0
}

# Run from $BIN_DIR. No --config: the seeded file is already at the default
# path DSperate computes from XDG_CONFIG_HOME. No layout or performance flags
# either -- flags override the ini, which would make them uneditable from the
# pause menu.
dsp_run_emulator() {
    set -- "$ROM_PATH" --fullscreen
    if [ -n "$DSP_LOAD_STATE" ]; then
        set -- "$@" --load-state "$DSP_LOAD_STATE"
    elif [ -n "$DSP_RESUME_FLAG" ]; then
        set -- "$@" "$DSP_RESUME_FLAG"
    fi

    dsp_mark "emulator starting"
    ./dsperate "$@"
    _status=$?
    dsp_mark "emulator exited"
    return "$_status"
}

# A "timing:" line in the log: seconds since boot, to the hundredth, then
# what just happened. DSperate writes its own on the same clock, so the two
# line up when working out where a slow start or exit spends its time.
dsp_mark() {
    _up="?"
    if [ -r /proc/uptime ]; then
        read -r _up _ </proc/uptime
    fi
    echo "timing: $_up $1"
}

cleanup() {
    rm -f /tmp/stay_awake

    # Only when the pak itself is being stopped: after a normal exit DSperate
    # is already gone, and waiting for it would only hold up the launcher.
    # DSperate writes the battery save on SIGTERM, so -9 straight away loses
    # the last save. Give it a moment before insisting.
    if pidof dsperate >/dev/null 2>&1; then
        killall -q -15 dsperate 2>/dev/null || true
        sleep 2
        killall -q -9 dsperate 2>/dev/null || true
    fi

    sync
    dsp_mark "synced"
}

main() {
    set -x

    rm -f "$LOGS_PATH/$PAK_NAME.txt"
    exec >>"$LOGS_PATH/$PAK_NAME.txt"
    exec 2>&1

    echo "$0" "$@"
    dsp_mark "launch"

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
    dsp_migrate_config
    dsp_mark "config ready"
    dsp_detect_style
    dsp_mark "style read"
    dsp_export_pad_mapping
    dsp_mark "pad mapped"
    dsp_probe_buttons
    dsp_export_shaders

    # Only on a panel whose rotation SDL cannot tell DSperate about; the fbdev
    # tier writes /dev/fb0 directly. Empty everywhere else.
    if [ -n "$PROFILE_ROTATE" ]; then
        export DS_ROTATE="$PROFILE_ROTATE"
    fi

    dsp_resume_args

    cd "$BIN_DIR" || return 1
    dsp_run_emulator
}

if [ "${DSP_PAK_TEST:-}" != "1" ]; then
    main "$@"
fi
