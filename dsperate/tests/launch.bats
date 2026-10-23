#!/usr/bin/env bats

load test_helper

setup() {
    setup_launch tg5040 brick
    dsp_platform_profile "$PLATFORM" "$DEVICE"
    dsp_init_env
    mkdir -p "$DSP_USERDATA_DIR" "$DSP_CONFIG_DIR"
}

@test "tg3040 with no device is normalized to a tg5040 Brick" {
    PLATFORM="tg3040"
    DEVICE=""
    dsp_normalize_platform
    [ "$PLATFORM" = "tg5040" ]
    [ "$DEVICE" = "brick" ]
}

@test "tg3040 with a device set is left alone" {
    PLATFORM="tg3040"
    DEVICE="smartpro"
    dsp_normalize_platform
    [ "$PLATFORM" = "tg3040" ]
}

@test "platform_in_list matches whole words only" {
    run platform_in_list tg5040 "tg5040 tg5050 h700"
    [ "$status" -eq 0 ]
    run platform_in_list h700 "tg5040 tg5050 h700"
    [ "$status" -eq 0 ]
    # The substring trap the helper exists to avoid.
    run platform_in_list tg50 "tg5040 tg5050 h700"
    [ "$status" -ne 0 ]
    run platform_in_list rg35xxplus "tg5040 tg5050 h700"
    [ "$status" -ne 0 ]
}

@test "the config is seeded from the profile the device maps to" {
    dsp_seed_config
    [ -f "$DSP_CONFIG" ]
    [ "$(cat "$DSP_DEVICE_FILE")" = "no-sticks" ]
    grep -q "stylus_dpad" "$DSP_CONFIG"
}

@test "seeding substitutes every path token" {
    dsp_seed_config
    run grep -c "__" "$DSP_CONFIG"
    [ "$output" = "0" ]
    grep -q "saves = $SDCARD_PATH/Saves/DSP" "$DSP_CONFIG"
    grep -q "states = $SHARED_USERDATA_PATH/DSP-dsperate" "$DSP_CONFIG"
    grep -q "bios9 = $BIOS_PATH/DSP/bios9.bin" "$DSP_CONFIG"
    grep -q "cheats = $SDCARD_PATH/Cheats/DSP/usrcheat.dat" "$DSP_CONFIG"
    grep -q "games = $SDCARD_PATH/Roms/Nintendo DS (DSP)" "$DSP_CONFIG"
}

@test "seeding is idempotent and does not clobber user edits" {
    dsp_seed_config
    echo "# edited by the user" >>"$DSP_CONFIG"
    dsp_seed_config
    grep -q "edited by the user" "$DSP_CONFIG"
}

@test "a changed profile re-seeds and keeps a backup" {
    dsp_seed_config
    echo "# original" >>"$DSP_CONFIG"
    # Pretend the card moved to a device with two sticks.
    dsp_platform_profile tg5040 smartpro
    dsp_seed_config
    [ "$(cat "$DSP_DEVICE_FILE")" = "two-sticks" ]
    grep -q "original" "$DSP_USERDATA_DIR/dsperate.ini.bak"
    ! grep -q "original" "$DSP_CONFIG"
}

@test "deleting the marker re-seeds" {
    dsp_seed_config
    echo "# stale" >>"$DSP_CONFIG"
    rm -f "$DSP_DEVICE_FILE"
    dsp_seed_config
    ! grep -q "stale" "$DSP_CONFIG"
}

@test "a missing template leaves the config alone" {
    PROFILE_INI="does-not-exist"
    run dsp_seed_config
    [ "$status" -eq 0 ]
    [ ! -f "$DSP_CONFIG" ]
}

@test "the pad mapping is assembled from the device GUID and the profile body" {
    dsp_export_pad_mapping
    [ -n "$SDL_GAMECONTROLLERCONFIG" ]
    # guid,name,body,platform:Linux
    case "$SDL_GAMECONTROLLERCONFIG" in
    "030000004c050000c405000011010000,Fake Pad,"*",platform:Linux") ;;
    *)
        echo "unexpected: $SDL_GAMECONTROLLERCONFIG"
        return 1
        ;;
    esac
    [ "${SDL_GAMECONTROLLERCONFIG#*guide:b}" != "$SDL_GAMECONTROLLERCONFIG" ]
}

# Re-root the fake card on an h700 device whose pad reports <guid>.
setup_h700_pad() {
    setup_launch h700 "$1"
    unset LD_LIBRARY_PATH SDL_GAMECONTROLLERCONFIG
    dsp_platform_profile h700 "$1"
    dsp_init_env
    stub_pad_guid "$2" "ANBERNIC-keys"
}

@test "NextUI's fixed h700 layout takes the TrimUI mapping" {
    setup_h700_pad rg40xxh "$DSP_H700_FIXED_GUID"
    dsp_export_pad_mapping
    [ "$SDL_GAMECONTROLLERCONFIG" = "$DSP_H700_FIXED_GUID,ANBERNIC-keys,$DSP_MAP_TRIMUI,platform:Linux" ]
}

@test "NextUI's fixed h700 layout on a stickless model takes the stickless TrimUI mapping" {
    setup_h700_pad rg35xxplus "$DSP_H700_FIXED_GUID"
    dsp_export_pad_mapping
    [ "$SDL_GAMECONTROLLERCONFIG" = "$DSP_H700_FIXED_GUID,ANBERNIC-keys,$DSP_MAP_TRIMUI_NOSTICK,platform:Linux" ]
}

@test "an h700 pad on the old layout keeps the per-model mapping" {
    # Pre-rc11 NextUI, or SDL_JOYSTICK_H700_FIXED_LAYOUT=0.
    setup_h700_pad rg40xxh 19000000010000000100000000010000
    dsp_export_pad_mapping
    [ "$SDL_GAMECONTROLLERCONFIG" = "19000000010000000100000000010000,ANBERNIC-keys,$DSP_MAP_H700_STICKS,platform:Linux" ]

    setup_h700_pad rg35xxplus 19000000010000000100000000010000
    dsp_export_pad_mapping
    [ "$SDL_GAMECONTROLLERCONFIG" = "19000000010000000100000000010000,ANBERNIC-keys,$DSP_MAP_H700_NOSTICK,platform:Linux" ]
}

@test "the fixed-layout GUID changes nothing off h700" {
    stub_pad_guid "$DSP_H700_FIXED_GUID" "Fake Pad"
    dsp_export_pad_mapping
    [ "$SDL_GAMECONTROLLERCONFIG" = "$DSP_H700_FIXED_GUID,Fake Pad,$DSP_MAP_TRIMUI_NOSTICK,platform:Linux" ]
}

@test "no pad means no mapping rather than a broken one" {
    printf '#!/bin/sh\nexit 1\n' >"$PAK_DIR/$PLATFORM/dsp-pad-guid"
    chmod +x "$PAK_DIR/$PLATFORM/dsp-pad-guid"
    unset SDL_GAMECONTROLLERCONFIG
    run dsp_export_pad_mapping
    [ "$status" -eq 0 ]
    [ -z "${SDL_GAMECONTROLLERCONFIG:-}" ]
}

@test "an unsupported platform yields no mapping" {
    dsp_platform_profile miyoomini ""
    unset SDL_GAMECONTROLLERCONFIG
    run dsp_export_pad_mapping
    [ "$status" -eq 0 ]
    [ -z "${SDL_GAMECONTROLLERCONFIG:-}" ]
}

@test "bundled binaries come first on LD_LIBRARY_PATH, then NextUI, then the system" {
    case "$LD_LIBRARY_PATH" in
    "$PAK_DIR/$PLATFORM:$SDCARD_PATH/.system/$PLATFORM/lib:/usr/trimui/lib:"*) ;;
    *)
        echo "unexpected: $LD_LIBRARY_PATH"
        return 1
        ;;
    esac
}

@test "h700 gets no TrimUI library directory" {
    setup_launch h700 rg35xxplus
    # setup() already ran dsp_init_env for tg5040, and dsp_init_env appends the
    # inherited value, so the earlier /usr/trimui/lib would ride along. A real
    # launch inherits MinUI's environment, not a previous run's.
    unset LD_LIBRARY_PATH
    dsp_platform_profile h700 rg35xxplus
    dsp_init_env
    [ "${LD_LIBRARY_PATH#*trimui}" = "$LD_LIBRARY_PATH" ]
    case "$LD_LIBRARY_PATH" in
    "$PAK_DIR/h700:$SDCARD_PATH/.system/h700/lib:"*) ;;
    *)
        echo "unexpected: $LD_LIBRARY_PATH"
        return 1
        ;;
    esac
}

@test "DSperate is pointed at its config through XDG_CONFIG_HOME" {
    # Config::dir() appends /dsperate to XDG_CONFIG_HOME, so the seeded file has
    # to land at that exact path or DSperate silently writes its own defaults.
    [ "$XDG_CONFIG_HOME" = "$DSP_USERDATA_DIR" ]
    [ "$DSP_CONFIG" = "$DSP_USERDATA_DIR/dsperate/dsperate.ini" ]
}

@test "the launcher's file keys are exported, with the extension kept" {
    # minarch keys every .minui artifact on the basename *with* extension.
    [ "$MINUI_ROM_FILE" = "" ] || true
    ROM_PATH="$SDCARD_PATH/Roms/Nintendo DS (DSP)/Some Game (USA).nds"
    dsp_init_env
    [ "$MINUI_ROM_FILE" = "Some Game (USA).nds" ]
    [ "$MINUI_ROM_PATH" = "/Roms/Nintendo DS (DSP)/Some Game (USA).nds" ]
    [ "$MINUI_DIR" = "$SHARED_USERDATA_PATH/.minui/DSP" ]
    [ "$MINUI_SHARED_DIR" = "$SHARED_USERDATA_PATH/.minui" ]
}

@test "auto-resume is skipped rather than wrong for a ROM outside the card" {
    # An absolute path here becomes /mnt/SDCARD/mnt/SDCARD/... in the frontend
    # and the resume silently never fires, so exporting nothing is the safer
    # failure.
    unset MINUI_ROM_PATH
    ROM_PATH="/somewhere/else/Game.nds"
    dsp_init_env
    [ -z "${MINUI_ROM_PATH:-}" ]
}

@test "a plain launch does not resume, even though autoload is on" {
    # minarch writes 8 for "start fresh". Without this the shipped autoload
    # would resume anyway and the launcher's choice would be ignored.
    echo -n "8" >/tmp/resume_slot.txt
    ROM_PATH="$SDCARD_PATH/Roms/Nintendo DS (DSP)/Game.nds"
    dsp_resume_args
    [ "$DSP_RESUME_ARGS" = "--no-autoload" ]
    [ ! -f /tmp/resume_slot.txt ]
}

@test "the sleep slot is left to autoload" {
    echo -n "9" >/tmp/resume_slot.txt
    ROM_PATH="$SDCARD_PATH/Roms/Nintendo DS (DSP)/Game.nds"
    dsp_resume_args
    [ -z "$DSP_RESUME_ARGS" ]
    [ ! -f /tmp/resume_slot.txt ]
}

@test "a user slot resumes from the state named after the ROM" {
    # The stem has to match what emu.state_key = rom produces, or the launcher
    # asks for a file the emulator never wrote.
    grep -q "^state_key = rom" "$PAK_DIR/configs/no-sticks.ini"
    echo -n "3" >/tmp/resume_slot.txt
    ROM_PATH="$SDCARD_PATH/Roms/Nintendo DS (DSP)/Some Game.nds"
    dsp_init_env
    dsp_resume_args
    [ "$DSP_RESUME_ARGS" = "--load-state $DSP_STATES_DIR/Some Game.3.dss" ]
    [ ! -f /tmp/resume_slot.txt ]
}

@test "no resume marker means no resume flags" {
    rm -f /tmp/resume_slot.txt
    ROM_PATH="$SDCARD_PATH/Roms/Nintendo DS (DSP)/Game.nds"
    dsp_resume_args
    [ -z "$DSP_RESUME_ARGS" ]
}
