#!/usr/bin/env bats

load test_helper

setup() {
    setup_launch tg5040 brick
}

profile_for() {
    dsp_platform_profile "$1" "$2"
}

@test "the Brick is the one TrimUI device with no sticks" {
    profile_for tg5040 brick
    [ "$PROFILE_INI" = "no-sticks" ]
    [ "$PROFILE_MAP" = "$DSP_MAP_TRIMUI_NOSTICK" ]
}

@test "the Smart Pro and Brick Pro have two sticks" {
    profile_for tg5040 smartpro
    [ "$PROFILE_INI" = "two-sticks" ]
    profile_for tg5040 brickpro
    [ "$PROFILE_INI" = "two-sticks" ]
}

@test "tg5050 has two sticks whatever the device says" {
    profile_for tg5050 ""
    [ "$PROFILE_INI" = "two-sticks" ]
    [ "$PROFILE_MAP" = "$DSP_MAP_TRIMUI" ]
}

@test "the h700 two-stick models take the stick mapping" {
    for d in rg35xxh rg35xxpro rg40xxh rgcubexx rg34xxsp; do
        profile_for h700 "$d"
        [ "$PROFILE_INI" = "two-sticks" ] || {
            echo "$d gave $PROFILE_INI"
            return 1
        }
        [ "$PROFILE_MAP" = "$DSP_MAP_H700_STICKS" ]
    done
}

@test "the rg40xxv has a left stick only" {
    profile_for h700 rg40xxv
    [ "$PROFILE_INI" = "one-stick" ]
    [ "$PROFILE_MAP" = "$DSP_MAP_H700_LSTICK" ]
}

@test "the stickless h700 models take the no-stick mapping" {
    for d in rg28xx rg34xx rg35xxplus rg35xxsp rgsp; do
        profile_for h700 "$d"
        [ "$PROFILE_INI" = "no-sticks" ] || {
            echo "$d gave $PROFILE_INI"
            return 1
        }
        [ "$PROFILE_MAP" = "$DSP_MAP_H700_NOSTICK" ]
    done
}

@test "an unknown h700 device falls back to the stickless profile" {
    profile_for h700 ""
    [ "$PROFILE_INI" = "no-sticks" ]
    [ "$PROFILE_MAP_FIXED" = "$DSP_MAP_TRIMUI_NOSTICK" ]
}

@test "the fixed-layout GUID is the one NextUI h700-rc11 reports" {
    [ "$DSP_H700_FIXED_GUID" = "19000000010000000100000000016e01" ]
}

@test "h700 models with a stick take the TrimUI mapping on the fixed layout" {
    for d in rg35xxh rg35xxpro rg40xxh rgcubexx rg34xxsp rg40xxv; do
        profile_for h700 "$d"
        [ "$PROFILE_MAP_FIXED" = "$DSP_MAP_TRIMUI" ] || {
            echo "$d gave $PROFILE_MAP_FIXED"
            return 1
        }
    done
}

@test "stickless h700 models take the stickless TrimUI mapping on the fixed layout" {
    for d in rg28xx rg34xx rg35xxplus rg35xxsp rgsp; do
        profile_for h700 "$d"
        [ "$PROFILE_MAP_FIXED" = "$DSP_MAP_TRIMUI_NOSTICK" ] || {
            echo "$d gave $PROFILE_MAP_FIXED"
            return 1
        }
    done
}

@test "only h700 has a fixed-layout mapping" {
    for p in tg5040:brick tg5040:smartpro tg5050: miyoomini:; do
        profile_for "${p%%:*}" "${p##*:}"
        [ -z "$PROFILE_MAP_FIXED" ] || {
            echo "$p unexpectedly has $PROFILE_MAP_FIXED"
            return 1
        }
    done
}

@test "every profile the table can return exists in the pak" {
    for p in tg5040:brick tg5040:smartpro tg5050: h700:rg28xx h700:rg40xxv h700:rg40xxh h700:; do
        profile_for "${p%%:*}" "${p##*:}"
        [ -f "$PAK_DIR/configs/$PROFILE_INI.ini" ] || {
            echo "$p -> missing configs/$PROFILE_INI.ini"
            return 1
        }
    done
}

@test "only the rg28xx asks for a rotation" {
    profile_for h700 rg28xx
    [ "$PROFILE_ROTATE" = "270" ]
    for p in tg5040:brick tg5050: h700:rg35xxplus h700:rg40xxh; do
        profile_for "${p%%:*}" "${p##*:}"
        [ -z "$PROFILE_ROTATE" ] || {
            echo "$p unexpectedly rotates"
            return 1
        }
    done
}

@test "only TrimUI adds the system library directory" {
    profile_for tg5040 brick
    [ "$PROFILE_LD_DIRS" = "/usr/trimui/lib" ]
    profile_for tg5050 ""
    [ "$PROFILE_LD_DIRS" = "/usr/trimui/lib" ]
    profile_for h700 rg35xxplus
    [ -z "$PROFILE_LD_DIRS" ]
}

@test "an unsupported platform yields no profile at all" {
    profile_for miyoomini ""
    [ -z "$PROFILE_INI" ]
    [ -z "$PROFILE_MAP" ]
}

@test "every mapping uses SDL positional names and binds MENU to guide" {
    for m in "$DSP_MAP_TRIMUI" "$DSP_MAP_TRIMUI_NOSTICK" "$DSP_MAP_H700_STICKS" \
        "$DSP_MAP_H700_LSTICK" "$DSP_MAP_H700_NOSTICK"; do
        [ -n "$m" ]
        # DSperate reads the DS A button from SDL's "b", so both must be present
        # and distinct, and MENU has to be guide for padhotkeys.modifier.
        echo "$m" | grep -q "a:b" || return 1
        echo "$m" | grep -q "b:b" || return 1
        echo "$m" | grep -q "guide:b" || return 1
        echo "$m" | grep -q "back:b" || return 1
        echo "$m" | grep -q "dpup:" || return 1
    done
}

@test "only the stick profiles name stick axes" {
    [ "${DSP_MAP_TRIMUI_NOSTICK#*leftx}" = "$DSP_MAP_TRIMUI_NOSTICK" ]
    [ "${DSP_MAP_H700_NOSTICK#*leftx}" = "$DSP_MAP_H700_NOSTICK" ]
    [ "${DSP_MAP_TRIMUI#*leftx}" != "$DSP_MAP_TRIMUI" ]
    [ "${DSP_MAP_H700_STICKS#*rightx}" != "$DSP_MAP_H700_STICKS" ]
    # The rg40xxv has a left stick but no right one.
    [ "${DSP_MAP_H700_LSTICK#*leftx}" != "$DSP_MAP_H700_LSTICK" ]
    [ "${DSP_MAP_H700_LSTICK#*rightx}" = "$DSP_MAP_H700_LSTICK" ]
}
