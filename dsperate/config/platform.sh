#!/bin/sh
# Per-platform and per-device facts, kept out of launch.sh so tests/platform.bats
# can assert them without a device: dsp_platform_profile does no I/O. It reads
# its two arguments and sets the PROFILE_* variables everything else consumes.
#
# The button indices come from NextUI's own pad enumeration, by way of
# minui-n64-pak's config/shared/platform.sh, which follows
# workspace/h700/platform/platform.c. TrimUI pads start at button 0; NextUI's
# h700 SDL2 enumerates in ascending evdev keycode order and the Anbernic pad
# reports ESC (1) and two volume keys (114/115) before the gamepad codes
# (304-316), so its gamepad buttons start at index 3. Stick-click codes 313 (L3)
# and 316 (R3) exist only where a stick does, which shifts L2/R2 again -- hence
# three h700 classes.
#
# That per-model numbering is pre-rc11 only. From NextUI h700-rc11, its SDL
# presents every H700 model with the TrimUI numbers and a new GUID, so on that
# GUID the TrimUI bodies apply instead (PROFILE_MAP_FIXED, chosen in launch.sh).

# The mapping bodies below are SDL GameController mappings in SDL's POSITIONAL
# convention: "a" is the South button, "b" East, "x" West, "y" North. On these
# Nintendo-layout pads that is a = the button marked B, b = A, x = Y, y = X.
#
# This matters and the wrong choice is not a visible break. DSperate's default
# [pad] block reads the DS A button from SDL's "b" (East), which on a
# Nintendo-layout pad is the button physically marked A. Label-named mappings
# would give a symmetric A/B and X/Y swap instead -- easy to misread as a
# broken mapping rather than the wrong convention.
#
# The button numbers follow what NextUI itself reads: on TrimUI (and on h700
# with the rc11 fixed layout) 0/1 are B/A and 2/3 are Y/X. The kernel codes
# behind them are not positional (the Brick's Y reports BTN_NORTH), so
# DSperate's pad.face_fix, which trusts those codes, is turned off in the
# shipped configs: these bodies are already positional.
#
# MENU becomes "guide" so DSperate's padhotkeys.modifier lands on it, and
# Select becomes "back".

# TrimUI pads: buttons from 0, analog triggers on axes 2 and 5.
DSP_MAP_TRIMUI='a:b0,b:b1,x:b2,y:b3,back:b6,guide:b8,start:b7,leftshoulder:b4,rightshoulder:b5,lefttrigger:a2,righttrigger:a5,leftstick:b9,rightstick:b10,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,rightx:a3,righty:a4'

# TrimUI Brick: no sticks at all, so no leftstick/rightstick and no axes.
DSP_MAP_TRIMUI_NOSTICK='a:b0,b:b1,x:b2,y:b3,back:b6,guide:b8,start:b7,leftshoulder:b4,rightshoulder:b5,lefttrigger:a2,righttrigger:a5,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2'

# h700 with two sticks: L3 at 12 shifts L2/R2 to 13/14, R3 takes 15.
DSP_MAP_H700_STICKS='a:b4,b:b3,x:b5,y:b6,back:b9,guide:b11,start:b10,leftshoulder:b7,rightshoulder:b8,lefttrigger:b13,righttrigger:b14,leftstick:b12,rightstick:b15,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,righty:a3'

# h700 with a left stick only: L3 exists, R3 does not.
DSP_MAP_H700_LSTICK='a:b4,b:b3,x:b5,y:b6,back:b9,guide:b11,start:b10,leftshoulder:b7,rightshoulder:b8,lefttrigger:b13,righttrigger:b14,leftstick:b12,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1'

# h700 with no sticks: no stick clicks, so L2/R2 sit at 12/13.
DSP_MAP_H700_NOSTICK='a:b4,b:b3,x:b5,y:b6,back:b9,guide:b11,start:b10,leftshoulder:b7,rightshoulder:b8,lefttrigger:b12,righttrigger:b13,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2'

# DevBridge's virtual pad on h700 (a second "ANBERNIC-keys", keys only, used
# for scripted testing). SDL numbers its buttons from KEY_UP: the d-pad is
# 0-3, then 304-316 in order. Measured with an SDL joystick probe on NextUI
# h700-rc11. On TrimUI the virtual pad copies TRIMUI Player1 and SDL's own
# Xbox 360 mapping already fits it.
DSP_MAP_H700_VIRTUAL='a:b5,b:b4,x:b6,y:b7,leftshoulder:b8,rightshoulder:b9,back:b10,start:b11,guide:b12,leftstick:b13,lefttrigger:b14,righttrigger:b15,rightstick:b16,dpup:b0,dpleft:b1,dpright:b2,dpdown:b3'

# NextUI's h700 SDL reports the built-in pad with this GUID only while its
# fixed (TrimUI-numbered) layout is active: rc11 and later, unless
# SDL_JOYSTICK_H700_FIXED_LAYOUT=0. Older builds, that override and any other
# SDL report a different GUID and keep the per-model numbering above. This is
# the check NextUI's "Porting NextUI Paks to H700" wiki page recommends.
# shellcheck disable=SC2034 # read by launch.sh
DSP_H700_FIXED_GUID='19000000010000000100000000016e01'

# The PROFILE_* variables are read by launch.sh, which sources this file, so
# their use is not visible from here.
# shellcheck disable=SC2034
# dsp_platform_profile <platform> <device>
#
# Sets:
#   PROFILE_INI      which configs/<name>.ini to seed from
#   PROFILE_MAP        the SDL GameController mapping body
#   PROFILE_MAP_FIXED  the body to use instead when the pad reports
#                      DSP_H700_FIXED_GUID, empty where that cannot happen
#   PROFILE_MAP_VIRTUAL the body for DevBridge's virtual pad, empty where SDL
#                      maps it already
#   PROFILE_ROTATE     DS_ROTATE value, empty for none
#   PROFILE_LD_DIRS    extra LD_LIBRARY_PATH entries, space separated
dsp_platform_profile() {
    _platform="$1"
    _device="$2"

    PROFILE_MAP_FIXED=""
    PROFILE_MAP_VIRTUAL=""
    PROFILE_ROTATE=""
    PROFILE_LD_DIRS=""

    case "$_platform" in
    tg5040)
        # SDL2 lives outside the pak on TrimUI.
        PROFILE_LD_DIRS="/usr/trimui/lib"
        case "$_device" in
        # The Brick is the one TrimUI device with no sticks at all.
        brick)
            PROFILE_INI="no-sticks"
            PROFILE_MAP="$DSP_MAP_TRIMUI_NOSTICK"
            ;;
        *)
            PROFILE_INI="two-sticks"
            PROFILE_MAP="$DSP_MAP_TRIMUI"
            ;;
        esac
        ;;
    tg5050)
        PROFILE_LD_DIRS="/usr/trimui/lib"
        PROFILE_INI="two-sticks"
        PROFILE_MAP="$DSP_MAP_TRIMUI"
        ;;
    h700)
        case "${_device:-rg35xxplus}" in
        rg35xxh | rg35xxpro | rg40xxh | rgcubexx | rg34xxsp)
            PROFILE_INI="two-sticks"
            PROFILE_MAP="$DSP_MAP_H700_STICKS"
            PROFILE_MAP_FIXED="$DSP_MAP_TRIMUI"
            ;;
        rg40xxv)
            PROFILE_INI="one-stick"
            PROFILE_MAP="$DSP_MAP_H700_LSTICK"
            # The fixed layout reports every axis on every model; the right
            # stick's simply stay centred here.
            PROFILE_MAP_FIXED="$DSP_MAP_TRIMUI"
            ;;
        *)
            # rg28xx, rg34xx, rg35xxplus, rg35xxsp, rgsp.
            PROFILE_INI="no-sticks"
            PROFILE_MAP="$DSP_MAP_H700_NOSTICK"
            PROFILE_MAP_FIXED="$DSP_MAP_TRIMUI_NOSTICK"
            ;;
        esac
        PROFILE_MAP_VIRTUAL="$DSP_MAP_H700_VIRTUAL"
        # The rg28xx panel is mounted portrait. NextUI exports SDL_ROTATION=1 so
        # applications see 640x480 landscape, but DSperate's fbdev tier writes
        # /dev/fb0 directly and never goes through SDL, so that rotation does not
        # reach it. DS_ROTATE is an environment variable, not a flag, and is read
        # only on the disp and fbdev paths.
        if [ "$_device" = "rg28xx" ]; then
            PROFILE_ROTATE=270
        fi
        ;;
    *)
        PROFILE_INI=""
        PROFILE_MAP=""
        ;;
    esac

    unset _platform _device
}
