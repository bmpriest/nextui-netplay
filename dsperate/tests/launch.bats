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

@test "the pad mapping body is handed to DSperate, which reads the GUID itself" {
    unset SDL_GAMECONTROLLERCONFIG
    dsp_export_pad_mapping
    [ "$MINUI_PAD_MAP" = "$DSP_MAP_TRIMUI_BRICK" ]
    [ -z "$MINUI_PAD_MAP_FIXED" ]
    [ "$MINUI_PAD_FIXED_GUID" = "$DSP_H700_FIXED_GUID" ]
    # Exported, so DSperate sees them, and SDL is left to DSperate.
    [ "$(sh -c 'echo "$MINUI_PAD_MAP"')" = "$DSP_MAP_TRIMUI_BRICK" ]
    [ -z "${SDL_GAMECONTROLLERCONFIG:-}" ]
}

@test "mapping the pad does not start SDL a second time" {
    printf '#!/bin/sh\necho ran >>"%s"\n' "$BATS_TEST_TMPDIR/pad-guid.log" >"$PAK_DIR/$PLATFORM/dsp-pad-guid"
    chmod +x "$PAK_DIR/$PLATFORM/dsp-pad-guid"
    dsp_export_pad_mapping
    [ ! -f "$BATS_TEST_TMPDIR/pad-guid.log" ]
}

# Re-root the fake card on an h700 device.
setup_h700_pad() {
    setup_launch h700 "$1"
    unset LD_LIBRARY_PATH MINUI_PAD_MAP MINUI_PAD_MAP_FIXED
    dsp_platform_profile h700 "$1"
    dsp_init_env
}

@test "an h700 model passes its own body and NextUI's fixed-layout one" {
    setup_h700_pad rg40xxh
    dsp_export_pad_mapping
    [ "$MINUI_PAD_MAP" = "$DSP_MAP_H700_STICKS" ]
    [ "$MINUI_PAD_MAP_FIXED" = "$DSP_MAP_TRIMUI" ]

    setup_h700_pad rg35xxplus
    dsp_export_pad_mapping
    [ "$MINUI_PAD_MAP" = "$DSP_MAP_H700_NOSTICK" ]
    [ "$MINUI_PAD_MAP_FIXED" = "$DSP_MAP_TRIMUI_NOSTICK" ]
}

@test "an unsupported platform yields no mapping" {
    dsp_platform_profile miyoomini ""
    unset MINUI_PAD_MAP
    run dsp_export_pad_mapping
    [ "$status" -eq 0 ]
    [ -z "${MINUI_PAD_MAP:-}" ]
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
    [ "$DSP_RESUME_FLAG" = "--no-autoload" ]
    [ -z "$DSP_LOAD_STATE" ]
    [ ! -f /tmp/resume_slot.txt ]
}

@test "the sleep slot is left to autoload" {
    echo -n "9" >/tmp/resume_slot.txt
    ROM_PATH="$SDCARD_PATH/Roms/Nintendo DS (DSP)/Game.nds"
    dsp_resume_args
    [ -z "$DSP_RESUME_FLAG" ]
    [ -z "$DSP_LOAD_STATE" ]
    [ ! -f /tmp/resume_slot.txt ]
}

@test "a user slot resumes from the state named after the ROM" {
    # The stem has to match what emu.state_key = rom produces, or the launcher
    # asks for a file the emulator never wrote.
    grep -q "^state_key = rom" "$PAK_DIR/configs/no-sticks.ini"
    echo -n "3" >/tmp/resume_slot.txt
    ROM_PATH="$SDCARD_PATH/Roms/Nintendo DS (DSP)/Some Game.nds"
    dsp_init_env
    mkdir -p "$DSP_STATES_DIR"
    : >"$DSP_STATES_DIR/Some Game.3.dss"
    dsp_resume_args
    [ "$DSP_LOAD_STATE" = "$DSP_STATES_DIR/Some Game.3.dss" ]
    [ -z "$DSP_RESUME_FLAG" ]
    [ ! -f /tmp/resume_slot.txt ]
}

@test "no resume marker means no resume flags" {
    rm -f /tmp/resume_slot.txt
    ROM_PATH="$SDCARD_PATH/Roms/Nintendo DS (DSP)/Game.nds"
    dsp_resume_args
    [ -z "$DSP_RESUME_FLAG" ]
    [ -z "$DSP_LOAD_STATE" ]
}

# Stand in for the emulator in $BIN_DIR: it records its argv, one per line, so
# a test sees exactly how the arguments were split.
stub_dsperate() {
    mkdir -p "$BIN_DIR"
    printf '#!/bin/sh\nprintf "%%s\\n" "$@" >"%s/argv.txt"\n' "$BATS_TEST_TMPDIR" >"$BIN_DIR/dsperate"
    chmod +x "$BIN_DIR/dsperate"
}

# Resume <rom name> from <slot> and run the stub the way main() does.
run_resume() {
    ROM_PATH="$SDCARD_PATH/Roms/Nintendo DS (DSP)/$1"
    dsp_init_env
    mkdir -p "$DSP_STATES_DIR"
    stub_dsperate
    if [ -n "$2" ]; then
        echo -n "$2" >/tmp/resume_slot.txt
    else
        rm -f /tmp/resume_slot.txt
    fi
    dsp_resume_args
    cd "$BIN_DIR"
    dsp_run_emulator
}

@test "a resume passes the state path to DSperate as one argument" {
    # The regression behind issue 4: an unquoted path split on the spaces in
    # the ROM name, DSperate took the last word as the ROM and quit.
    _rom="Some Game (USA) [!].nds"
    dsp_init_env
    mkdir -p "$DSP_STATES_DIR"
    : >"$DSP_STATES_DIR/Some Game (USA) [!].3.dss"
    run_resume "$_rom" 3
    printf '%s\n' "$ROM_PATH" --fullscreen --load-state "$DSP_STATES_DIR/Some Game (USA) [!].3.dss" >"$BATS_TEST_TMPDIR/want.txt"
    diff "$BATS_TEST_TMPDIR/want.txt" "$BATS_TEST_TMPDIR/argv.txt"
}

@test "a resume whose state is gone falls back to autoload" {
    # An explicit --load-state that cannot be read fails the whole run, so a
    # stale marker must not turn into one.
    run run_resume "Some Game (USA).nds" 2
    [ "$status" -eq 0 ]
    [[ "$output" == *"resume state "*"Some Game (USA).2.dss not found, falling back to autoload"* ]]
    [ ! -f /tmp/resume_slot.txt ]
    printf '%s\n' "$SDCARD_PATH/Roms/Nintendo DS (DSP)/Some Game (USA).nds" --fullscreen >"$BATS_TEST_TMPDIR/want.txt"
    diff "$BATS_TEST_TMPDIR/want.txt" "$BATS_TEST_TMPDIR/argv.txt"
}

@test "a plain launch passes --no-autoload as its own argument" {
    run_resume "Some Game (USA).nds" 8
    printf '%s\n' "$ROM_PATH" --fullscreen --no-autoload >"$BATS_TEST_TMPDIR/want.txt"
    diff "$BATS_TEST_TMPDIR/want.txt" "$BATS_TEST_TMPDIR/argv.txt"
}

@test "the sleep slot and no marker pass only the ROM" {
    run_resume "Some Game (USA).nds" 9
    printf '%s\n' "$ROM_PATH" --fullscreen >"$BATS_TEST_TMPDIR/want.txt"
    diff "$BATS_TEST_TMPDIR/want.txt" "$BATS_TEST_TMPDIR/argv.txt"

    run_resume "Some Game (USA).nds" ""
    diff "$BATS_TEST_TMPDIR/want.txt" "$BATS_TEST_TMPDIR/argv.txt"
}

# A state header: "DSST" and a little-endian u32 format version.
write_state() {
    printf "DSST\\$(printf '%03o' "$2")\\000\\000\\000" >"$1"
}

@test "a resume from a state DSperate can no longer read falls back to autoload" {
    # DSperate refuses states older than its oldest readable format, and a
    # refused --load-state holds the panel on an error and then fails the run.
    printf '4' >"$PAK_DIR/dsperate.state-version"
    dsp_init_env
    mkdir -p "$DSP_STATES_DIR"
    write_state "$DSP_STATES_DIR/Some Game (USA).3.dss" 2
    run run_resume "Some Game (USA).nds" 3
    [ "$status" -eq 0 ]
    [[ "$output" == *"Some Game (USA).3.dss predates this DSperate (format 2, needs 4), falling back to autoload"* ]]
    printf '%s\n' "$SDCARD_PATH/Roms/Nintendo DS (DSP)/Some Game (USA).nds" --fullscreen >"$BATS_TEST_TMPDIR/want.txt"
    diff "$BATS_TEST_TMPDIR/want.txt" "$BATS_TEST_TMPDIR/argv.txt"
}

@test "a resume from a current state still passes it as one argument" {
    printf '4' >"$PAK_DIR/dsperate.state-version"
    dsp_init_env
    mkdir -p "$DSP_STATES_DIR"
    write_state "$DSP_STATES_DIR/Some Game (USA).3.dss" 4
    run_resume "Some Game (USA).nds" 3
    printf '%s\n' "$ROM_PATH" --fullscreen --load-state "$DSP_STATES_DIR/Some Game (USA).3.dss" >"$BATS_TEST_TMPDIR/want.txt"
    diff "$BATS_TEST_TMPDIR/want.txt" "$BATS_TEST_TMPDIR/argv.txt"
}

@test "a resume from a file that is not a state falls back to autoload" {
    printf '4' >"$PAK_DIR/dsperate.state-version"
    dsp_init_env
    mkdir -p "$DSP_STATES_DIR"
    printf 'not a state' >"$DSP_STATES_DIR/Some Game (USA).3.dss"
    run run_resume "Some Game (USA).nds" 3
    [ "$status" -eq 0 ]
    [[ "$output" == *"Some Game (USA).3.dss is not a DSperate state, falling back to autoload"* ]]
    printf '%s\n' "$SDCARD_PATH/Roms/Nintendo DS (DSP)/Some Game (USA).nds" --fullscreen >"$BATS_TEST_TMPDIR/want.txt"
    diff "$BATS_TEST_TMPDIR/want.txt" "$BATS_TEST_TMPDIR/argv.txt"
}

@test "without a recorded state version the state is passed through unchecked" {
    # A pak built before dsperate.state-version existed must keep resuming.
    rm -f "$PAK_DIR/dsperate.state-version"
    dsp_init_env
    mkdir -p "$DSP_STATES_DIR"
    write_state "$DSP_STATES_DIR/Some Game (USA).3.dss" 2
    run_resume "Some Game (USA).nds" 3
    printf '%s\n' "$ROM_PATH" --fullscreen --load-state "$DSP_STATES_DIR/Some Game (USA).3.dss" >"$BATS_TEST_TMPDIR/want.txt"
    diff "$BATS_TEST_TMPDIR/want.txt" "$BATS_TEST_TMPDIR/argv.txt"
}

# A fake NextUI: its settings file and the two fonts it ships.
fake_nextui() {
    mkdir -p "$SDCARD_PATH/.system/res"
    : >"$SDCARD_PATH/.system/res/font1.ttf"
    : >"$SDCARD_PATH/.system/res/font2.ttf"
    printf '%s\n' "$@" >"$SHARED_USERDATA_PATH/minuisettings.txt"
}

@test "the pause menu takes NextUI's look, font and theme colours" {
    fake_nextui "font=1" "color1=0xFFFFFFFF" "color2=0x9B2257FF" "color7=0x000000FF"
    dsp_detect_style
    [ "$MINUI_STYLE" = "nextui" ]
    [ "$MINUI_FONT" = "$SDCARD_PATH/.system/res/font1.ttf" ]
    [ "$MINUI_COLOR2" = "0x9B2257FF" ]
    [ "$MINUI_COLOR7" = "0x000000FF" ]
    # Exported, so DSperate sees them.
    [ "$(sh -c 'echo "$MINUI_STYLE $MINUI_COLOR2"')" = "nextui 0x9B2257FF" ]
}

@test "NextUI's font setting is read the way NextUI reads it" {
    fake_nextui "font=0"
    dsp_detect_style
    [ "$MINUI_FONT" = "$SDCARD_PATH/.system/res/font2.ttf" ]

    fake_nextui "brightness=5"
    dsp_detect_style
    [ "$MINUI_FONT" = "$SDCARD_PATH/.system/res/font1.ttf" ]

    : >"$SDCARD_PATH/.system/res/custom.ttf"
    fake_nextui "font=custom.ttf"
    dsp_detect_style
    [ "$MINUI_FONT" = "$SDCARD_PATH/.system/res/custom.ttf" ]

    # A font named but not on the card is not handed on.
    fake_nextui "font=gone.ttf"
    dsp_detect_style
    [ "$MINUI_FONT" = "$SDCARD_PATH/.system/res/font1.ttf" ]
}

@test "a settings line written with a carriage return still reads" {
    printf 'font=0\r\ncolor2=0x112233FF\r\n' >"$SHARED_USERDATA_PATH/minuisettings.txt"
    mkdir -p "$SDCARD_PATH/.system/res"
    : >"$SDCARD_PATH/.system/res/font1.ttf"
    : >"$SDCARD_PATH/.system/res/font2.ttf"
    dsp_detect_style
    [ "$MINUI_FONT" = "$SDCARD_PATH/.system/res/font2.ttf" ]
    [ "$MINUI_COLOR2" = "0x112233FF" ]
}

@test "stock MinUI gets its own look and BPreplay" {
    mkdir -p "$SDCARD_PATH/.system/res"
    : >"$SDCARD_PATH/.system/res/BPreplayBold-unhinted.otf"
    dsp_detect_style
    [ "$MINUI_STYLE" = "minui" ]
    [ "$MINUI_FONT" = "$SDCARD_PATH/.system/res/BPreplayBold-unhinted.otf" ]
}

@test "minuisettings.txt alone is not NextUI" {
    # MinUI forks write the file too; NextUI is the one that ships font1.ttf.
    printf 'brightness=5\n' >"$SHARED_USERDATA_PATH/minuisettings.txt"
    dsp_detect_style
    [ "$MINUI_STYLE" = "minui" ]
}

@test "no font on the card leaves the menu its built-in one" {
    dsp_detect_style
    [ "$MINUI_STYLE" = "minui" ]
    [ -z "$MINUI_FONT" ]
}

@test "every profile opens the pause menu on a MENU tap" {
    dsp_seed_config
    grep -q '^pause.alt = mod$' "$DSP_CONFIG"
    # And migration has nothing to add to it.
    cp "$DSP_CONFIG" "$BATS_TEST_TMPDIR/seeded.ini"
    dsp_migrate_config
    cmp -s "$DSP_CONFIG" "$BATS_TEST_TMPDIR/seeded.ini"
}

@test "an older config gains the MENU tap once, under [padhotkeys]" {
    printf '[pad]\na = b\n\n[padhotkeys]\npause = mod+y\n\n[cheevos]\nenabled = true\n' >"$DSP_CONFIG"
    dsp_migrate_config
    dsp_migrate_config
    [ "$(grep -c '^pause.alt = mod$' "$DSP_CONFIG")" -eq 1 ]
    # In the section, not after it.
    [ "$(sed -n '/^\[padhotkeys\]/,/^\[cheevos\]/p' "$DSP_CONFIG" | grep -c '^pause.alt = mod$')" -eq 1 ]
    grep -q '^pause = mod+y$' "$DSP_CONFIG"
    grep -q '^enabled = true$' "$DSP_CONFIG"
}

@test "a player's own pause.alt is kept, even none" {
    printf '[padhotkeys]\npause = mod+y\npause.alt = none\n' >"$DSP_CONFIG"
    dsp_migrate_config
    ! grep -q '^pause.alt = mod$' "$DSP_CONFIG"
    grep -q '^pause.alt = none$' "$DSP_CONFIG"
}

@test "a keyboard pause.alt does not count as the pad's" {
    printf '[hotkeys]\npause.alt = F1\n\n[padhotkeys]\npause = mod+y\n' >"$DSP_CONFIG"
    dsp_migrate_config
    [ "$(sed -n '/^\[padhotkeys\]/,$p' "$DSP_CONFIG" | grep -c '^pause.alt = mod$')" -eq 1 ]
    grep -q '^pause.alt = F1$' "$DSP_CONFIG"
}

@test "a config with no [padhotkeys] gains one" {
    printf '[pad]\na = b\n' >"$DSP_CONFIG"
    dsp_migrate_config
    [ "$(sed -n '/^\[padhotkeys\]/,$p' "$DSP_CONFIG" | grep -c '^pause.alt = mod$')" -eq 1 ]
}

@test "no config means nothing to migrate" {
    rm -f "$DSP_CONFIG"
    dsp_migrate_config
    [ ! -f "$DSP_CONFIG" ]
}

@test "every profile already has what migration adds" {
    for profile in no-sticks one-stick two-sticks; do
        rm -f "$DSP_DEVICE_FILE"
        PROFILE_INI="$profile"
        dsp_seed_config
        cp "$DSP_CONFIG" "$BATS_TEST_TMPDIR/seeded.ini"
        dsp_migrate_config
        cmp -s "$DSP_CONFIG" "$BATS_TEST_TMPDIR/seeded.ini"
    done
}

@test "an older config gains MENU+START and loses the state and mic chords, once" {
    printf '[padhotkeys]\npause = mod+y\npause.alt = mod\n\n[cheevos]\nenabled = true\n' >"$DSP_CONFIG"
    dsp_migrate_config
    dsp_migrate_config
    section="$(sed -n '/^\[padhotkeys\]/,/^\[cheevos\]/p' "$DSP_CONFIG")"
    [ "$(echo "$section" | grep -c '^pip_corner_next = mod+start$')" -eq 1 ]
    for key in save_state load_state slot_next slot_prev mic; do
        [ "$(echo "$section" | grep -c "^$key = none$")" -eq 1 ]
    done
}

@test "a player's own state and pip chords are kept" {
    printf '[padhotkeys]\npause = mod+y\nsave_state = mod+rightshoulder\nslot_next = none\npip_corner_next = mod+x\n' >"$DSP_CONFIG"
    dsp_migrate_config
    grep -q '^save_state = mod+rightshoulder$' "$DSP_CONFIG"
    [ "$(grep -c '^slot_next' "$DSP_CONFIG")" -eq 1 ]
    grep -q '^pip_corner_next = mod+x$' "$DSP_CONFIG"
    ! grep -q '^pip_corner_next = mod+start$' "$DSP_CONFIG"
    grep -q '^load_state = none$' "$DSP_CONFIG"
}

@test "MENU+START is not added where it is already taken" {
    printf '[padhotkeys]\npause = mod+y\nscreenshot = mod+start\n' >"$DSP_CONFIG"
    dsp_migrate_config
    ! grep -q '^pip_corner_next' "$DSP_CONFIG"
}

@test "with no pause line, upstream's MENU+START pause keeps it" {
    printf '[padhotkeys]\nquit = mod+a\n' >"$DSP_CONFIG"
    dsp_migrate_config
    ! grep -q '^pip_corner_next' "$DSP_CONFIG"
    grep -q '^save_state = none$' "$DSP_CONFIG"
}

@test "the pause menu is told the profile it restores shortcuts from" {
    [ "$MINUI_PROFILE_INI" = "$PAK_DIR/configs/no-sticks.ini" ]
    [ -f "$MINUI_PROFILE_INI" ]
}

@test "the pause menu is told what the Brick calls its F1 and F2" {
    dsp_export_pad_mapping
    [ "$MINUI_BUTTON_LABELS" = "leftstick=F1 rightstick=F2" ]
}

@test "a device that names its buttons as SDL does exports no labels" {
    dsp_platform_profile tg5040 smartpro
    dsp_export_pad_mapping
    [ -z "$MINUI_BUTTON_LABELS" ]
}

@test "the pause menu is told which shaders are slow on the platform" {
    PROFILE_SLOW_SHADERS="colour:natural_vision pattern:beads"
    dsp_export_shaders
    [ "$MINUI_SLOW_SHADERS" = "colour:natural_vision pattern:beads" ]
    PROFILE_SLOW_SHADERS=""
    dsp_export_shaders
    [ -z "$MINUI_SLOW_SHADERS" ]
}

@test "shader-timing.txt turns the shader timing on, and stays" {
    unset MINUI_SHADER_TIMING
    dsp_export_shaders
    [ -z "${MINUI_SHADER_TIMING:-}" ]
    touch "$DSP_USERDATA_DIR/shader-timing.txt"
    dsp_export_shaders
    [ "$MINUI_SHADER_TIMING" = "1" ]
    [ -f "$DSP_USERDATA_DIR/shader-timing.txt" ]
}

@test "probe.txt runs the probe once, and nothing runs without it" {
    printf '#!/bin/sh\necho "$@" >>"%s"\n' "$BATS_TEST_TMPDIR/probe.log" >"$PAK_DIR/$PLATFORM/dsp-pad-guid"
    chmod +x "$PAK_DIR/$PLATFORM/dsp-pad-guid"
    dsp_probe_buttons
    [ ! -f "$BATS_TEST_TMPDIR/probe.log" ]
    touch "$DSP_USERDATA_DIR/probe.txt"
    dsp_probe_buttons
    dsp_probe_buttons
    [ "$(cat "$BATS_TEST_TMPDIR/probe.log")" = "--probe 15" ]
    [ ! -f "$DSP_USERDATA_DIR/probe.txt" ]
}

@test "a probe that fails does not stop the launch" {
    printf '#!/bin/sh\nexit 1\n' >"$PAK_DIR/$PLATFORM/dsp-pad-guid"
    chmod +x "$PAK_DIR/$PLATFORM/dsp-pad-guid"
    touch "$DSP_USERDATA_DIR/probe.txt"
    run dsp_probe_buttons
    [ "$status" -eq 0 ]
}

@test "NextUI's screenshot notice setting reaches the pause menu" {
    mkdir -p "$SDCARD_PATH/.system/res"
    touch "$SDCARD_PATH/.system/res/font1.ttf"
    printf 'font=1\r\n' >"$SHARED_USERDATA_PATH/minuisettings.txt"
    dsp_detect_style
    [ "$MINUI_NOTIFY_SCREENSHOT" = "1" ]
    printf 'font=1\r\nnotifyScreenshot=0\r\n' >"$SHARED_USERDATA_PATH/minuisettings.txt"
    dsp_detect_style
    [ "$MINUI_NOTIFY_SCREENSHOT" = "0" ]
    printf 'notifyScreenshot=0\nnotifyScreenshot=1\n' >"$SHARED_USERDATA_PATH/minuisettings.txt"
    dsp_detect_style
    [ "$MINUI_NOTIFY_SCREENSHOT" = "1" ]
}

@test "stock MinUI always gets the screenshot notice" {
    printf 'notifyScreenshot=0\n' >"$SHARED_USERDATA_PATH/minuisettings.txt"
    dsp_detect_style
    [ "$MINUI_STYLE" = "minui" ]
    [ "$MINUI_NOTIFY_SCREENSHOT" = "1" ]
}

@test "an older config keeps its screenshot chord" {
    printf '[padhotkeys]\npause = mod+y\nscreenshot = mod+b\n' >"$DSP_CONFIG"
    dsp_migrate_config
    grep -q '^screenshot = mod+b$' "$DSP_CONFIG"
}

@test "cleanup does not wait when DSperate has already exited" {
    printf '#!/bin/sh\nexit 1\n' >"$STUB_BIN/pidof"
    printf '#!/bin/sh\necho "sleep $*" >>"%s"\n' "$BATS_TEST_TMPDIR/calls.log" >"$STUB_BIN/sleep"
    printf '#!/bin/sh\necho "killall $*" >>"%s"\n' "$BATS_TEST_TMPDIR/calls.log" >"$STUB_BIN/killall"
    chmod +x "$STUB_BIN/pidof" "$STUB_BIN/sleep" "$STUB_BIN/killall"
    run cleanup
    [ "$status" -eq 0 ]
    [ ! -f "$BATS_TEST_TMPDIR/calls.log" ]
    echo "$output" | grep -q '^timing: .* synced$'
}

@test "cleanup stops a DSperate that is still running" {
    printf '#!/bin/sh\nexit 0\n' >"$STUB_BIN/pidof"
    printf '#!/bin/sh\necho "sleep $*" >>"%s"\n' "$BATS_TEST_TMPDIR/calls.log" >"$STUB_BIN/sleep"
    printf '#!/bin/sh\necho "killall $*" >>"%s"\n' "$BATS_TEST_TMPDIR/calls.log" >"$STUB_BIN/killall"
    chmod +x "$STUB_BIN/pidof" "$STUB_BIN/sleep" "$STUB_BIN/killall"
    run cleanup
    [ "$status" -eq 0 ]
    [ "$(sed -n 1p "$BATS_TEST_TMPDIR/calls.log")" = "killall -q -15 dsperate" ]
    [ "$(sed -n 2p "$BATS_TEST_TMPDIR/calls.log")" = "sleep 2" ]
    [ "$(sed -n 3p "$BATS_TEST_TMPDIR/calls.log")" = "killall -q -9 dsperate" ]
}

@test "timing marks name what happened" {
    run dsp_mark "launch"
    [ "$status" -eq 0 ]
    case "$output" in
    "timing: "*" launch") ;;
    *) return 1 ;;
    esac
}
