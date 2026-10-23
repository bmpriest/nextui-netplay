#!/usr/bin/env bash
# Shared scaffolding: a hermetic fake SD card plus stubs for everything launch.sh
# would otherwise reach for on a real device.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export REPO_ROOT

# Read a make variable without a toolchain, through the print-% target.
mk() {
    run make -C "$REPO_ROOT" --no-print-directory "print-$1"
    output="${output#"$1"=}"
}

# The platforms pak.json declares, space separated.
pak_platforms() {
    jq -r '.platforms | join(" ")' "$REPO_ROOT/pak.json"
}

# Replace the current platform's dsp-pad-guid with one reporting <guid> <name>.
stub_pad_guid() {
    printf '#!/bin/sh\nprintf "%%s\\n" "%s" "%s"\n' "$1" "$2" >"$PAK/$PLATFORM/dsp-pad-guid"
    chmod +x "$PAK/$PLATFORM/dsp-pad-guid"
}

# Build a fake pak tree and source launch.sh with main() suppressed.
setup_launch() {
    SDCARD_PATH="$BATS_TEST_TMPDIR/SDCARD"
    PLATFORM="${1:-tg5040}"
    DEVICE="${2:-brick}"
    PAK="$SDCARD_PATH/Emus/$PLATFORM/DSP.pak"

    USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
    SHARED_USERDATA_PATH="$SDCARD_PATH/.userdata/shared"
    LOGS_PATH="$USERDATA_PATH/logs"
    BIOS_PATH="$SDCARD_PATH/Bios"
    export SDCARD_PATH PLATFORM DEVICE USERDATA_PATH SHARED_USERDATA_PATH LOGS_PATH BIOS_PATH

    mkdir -p "$PAK/$PLATFORM" "$USERDATA_PATH" "$SHARED_USERDATA_PATH" "$LOGS_PATH" "$BIOS_PATH"
    cp "$REPO_ROOT/launch.sh" "$PAK/launch.sh"
    cp "$REPO_ROOT/config/platform.sh" "$PAK/platform.sh"
    mkdir -p "$PAK/configs"
    cp "$REPO_ROOT"/config/dsperate/*.ini "$PAK/configs/"

    # Stubs on PATH: none of these exist on a build machine.
    STUB_BIN="$BATS_TEST_TMPDIR/stubbin"
    mkdir -p "$STUB_BIN"
    printf '#!/bin/sh\nexit 0\n' >"$STUB_BIN/killall"
    printf '#!/bin/sh\nexit 0\n' >"$STUB_BIN/minui-power-control"
    printf '#!/bin/sh\nexit 0\n' >"$STUB_BIN/sync"
    chmod +x "$STUB_BIN"/*
    PATH="$STUB_BIN:$PATH"
    export PATH STUB_BIN

    # A pad-guid that reports a known GUID, so the mapping assembly is testable.
    stub_pad_guid "030000004c050000c405000011010000" "Fake Pad"

    PAK_DIR="$PAK"
    export PAK_DIR

    DSP_PAK_TEST=1
    export DSP_PAK_TEST
    # launch.sh sources platform.sh inside main(), which the guard suppresses, so
    # the profile table has to be pulled in separately for the helpers to exist.
    # shellcheck source=/dev/null
    . "$PAK/platform.sh"
    # shellcheck source=/dev/null
    . "$PAK/launch.sh"
    # launch.sh derives these from $0 when run for real; set them for sourcing.
    PAK_DIR="$PAK"
    PAK_NAME="DSP"
}
