#!/usr/bin/env bats

load test_helper

@test "the pak metadata drives the artifact and install paths" {
    mk PAK_NAME
    [ "$output" = "DSP" ]
    mk PAK_FOLDER
    [ "$output" = "Emus" ]
}

@test "the Makefile, pak.json and launch.sh agree on the platform list" {
    mk PLATFORMS
    make_platforms="$output"
    [ "$make_platforms" = "$(pak_platforms)" ]

    # SUPPORTED_PLATFORMS is what launch.sh actually gates on.
    launch_platforms="$(sed -n 's/^SUPPORTED_PLATFORMS="\(.*\)"$/\1/p' "$REPO_ROOT/launch.sh")"
    [ "$launch_platforms" = "$make_platforms" ]
}

@test "every platform has an image, CPU flags, a docker runner and a glibc ceiling" {
    for p in $(pak_platforms); do
        u="$(echo "$p" | tr '[:lower:]' '[:upper:]')"
        mk "${u}_IMAGE"
        [ -n "$output" ] || return 1
        mk "${u}_CPUFLAGS"
        [ -n "$output" ] || return 1
        mk "${u}_GLIBC_MAX"
        [ -n "$output" ] || return 1
        mk "DOCKER_RUN_${p}"
        [ -n "$output" ] || return 1
    done
}

@test "every platform has build, stage and dist targets" {
    for p in $(pak_platforms); do
        grep -q "^build-$p:" "$REPO_ROOT/Makefile" || return 1
        grep -q "^stage-$p:" "$REPO_ROOT/Makefile" || return 1
        grep -q "^dist-$p:" "$REPO_ROOT/Makefile" || return 1
    done
}

@test "build and dist cover every platform" {
    for p in $(pak_platforms); do
        grep -q "MAKE) stage-$p\$" "$REPO_ROOT/Makefile" || return 1
        grep -q "MAKE) dist-$p\$" "$REPO_ROOT/Makefile" || return 1
    done
}

@test "the toolchains come from the LoveRetro registry" {
    for p in $(pak_platforms); do
        u="$(echo "$p" | tr '[:lower:]' '[:upper:]')"
        mk "${u}_IMAGE"
        case "$output" in
        ghcr.io/loveretro/*-toolchain:*) ;;
        *) return 1 ;;
        esac
    done
}

@test "h700 builds for the Cortex-A53 like tg5040, not the A55" {
    mk H700_CPUFLAGS
    h700="$output"
    mk TG5040_CPUFLAGS
    [ "$h700" = "$output" ]
    mk TG5050_CPUFLAGS
    [ "$h700" != "$output" ]
}

@test "the upstream pin is a tag, not a branch" {
    mk DSPERATE_TAG
    [[ "$output" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]
}

@test "the README names the pinned upstream version" {
    mk DSPERATE_TAG
    grep -qF "(version ${output#v})" "$REPO_ROOT/README.md"
}

@test "the pak records the oldest state format its DSperate reads" {
    # launch.sh checks a state's header against it before asking for
    # --load-state. It is read from upstream's source rather than hardcoded, so
    # a format bump on a tag update needs no change here.
    grep -q 'OLDEST_READABLE_VERSION' "$REPO_ROOT/Makefile"
    grep -q 'src/core/state/state.h' "$REPO_ROOT/Makefile"
    grep -q 'dsperate.state-version' "$REPO_ROOT/Makefile"
    grep -q 'dsperate.state-version' "$REPO_ROOT/launch.sh"
}

@test "the shipping build never compiles the Wayland tier" {
    mk DSP_CMAKE_COMMON
    [[ "$output" == *"-DDSPERATE_WAYLAND=OFF"* ]]
}

@test "netplay is built in explicitly" {
    # Upstream defaults it on; spelling it out keeps an upstream default change
    # from flipping it without a diff here.
    mk DSP_CMAKE_COMMON
    [[ "$output" == *"-DDSPERATE_NET=ON"* ]]
}

@test "the Vulkan tier is never left to header detection" {
    # Upstream enables it whenever the build host has vulkan headers, so leaving
    # it unset would make the binary depend on the container image.
    mk DSP_CMAKE_COMMON
    [[ "$output" == *"-DDSPERATE_VULKAN=OFF"* ]]
}

@test "the shipping build requires the recompiler and the NEON kernels" {
    mk DSP_CMAKE_COMMON
    [[ "$output" == *"-DDSPERATE_JIT=ON"* ]]
    [[ "$output" == *"-DDSPERATE_NEON=ON"* ]]
    # Asking is not enough: upstream downgrades both with a plain set(), so the
    # build graph is checked after the fact.
    [ -x "$REPO_ROOT/scripts/assert-build.sh" ]
    grep -q -- '-DDSPERATE_JIT=1' "$REPO_ROOT/scripts/assert-build.sh"
    grep -q -- '-DDSPERATE_NEON=1' "$REPO_ROOT/scripts/assert-build.sh"
}

@test "assert-build reads the build graph and not the CMake cache" {
    # CMakeCache.txt still reads ON after upstream's plain set() downgrade, so
    # checking it would pass on an interpreter-only build.
    grep -q "build.ninja" "$REPO_ROOT/scripts/assert-build.sh"
    # Comments name CMakeCache to explain why it is not used, so only code counts.
    ! grep -v '^[[:space:]]*#' "$REPO_ROOT/scripts/assert-build.sh" | grep -q "CMakeCache"
}

@test "test binaries never reach a shipping build" {
    mk DSP_CMAKE_COMMON
    [[ "$output" == *"-DDSPERATE_TESTS=OFF"* ]]
    # ctest builds somewhere no dist target reads from.
    grep -q "build/ctest" "$REPO_ROOT/Makefile"
    ! grep -q "DSP_DIST.*ctest" "$REPO_ROOT/Makefile"
}

@test "the headless harness is neither built nor shipped" {
    mk DSP_CMAKE_COMMON
    [[ "$output" == *"-DDSPERATE_HEADLESS=OFF"* ]]
    # The comment explaining the choice names it, so only recipe lines count.
    ! grep -v '^[[:space:]]*@\?#' "$REPO_ROOT/Makefile" | grep -q "dsperate-headless"
}

@test "PGO is off" {
    # pgo/aarch64/MANIFEST fingerprints GCC 13.3.0 and upstream's configure is a
    # FATAL_ERROR on a mismatch, so `use` cannot configure with our toolchains.
    mk DSPERATE_PGO
    [ "$output" = "OFF" ]
}

@test "no two platforms share a CMake build directory" {
    # The build dir is derived from $(1) in the recipe macros, so each platform
    # gets its own. Upstream's presets all point at one build/aarch64.
    grep -q -- '-B /build/build/\$(1)' "$REPO_ROOT/Makefile"
}

@test "libstdc++ and libgcc are linked statically" {
    grep -q -- "-static-libstdc++" "$REPO_ROOT/Makefile"
    grep -q -- "-static-libgcc" "$REPO_ROOT/Makefile"
}

@test "pthread is forced onto the end of the link line" {
    # CMAKE_EXE_LINKER_FLAGS lands ahead of the objects that reference
    # pthread_create, where --as-needed drops it again. Only
    # CMAKE_CXX_STANDARD_LIBRARIES is appended last. Simplifying this into the
    # linker flags regresses silently, and only on glibc < 2.34.
    mk DSP_PTHREAD_LIBS
    [[ "$output" == *"--no-as-needed"* ]]
    [[ "$output" == *"-lpthread"* ]]
    grep -q 'CMAKE_CXX_STANDARD_LIBRARIES="$(DSP_PTHREAD_LIBS)"' "$REPO_ROOT/Makefile"
    ! grep -q 'CMAKE_EXE_LINKER_FLAGS=.*lpthread' "$REPO_ROOT/Makefile"
}

@test "JOBS caps build parallelism and defaults to one per CPU" {
    # Every test executable is an LTO link of the whole core, which can run a
    # desktop Docker VM out of memory at one job per core.
    mk DSP_JOBS
    [ "$output" = '$(nproc)' ]
    run make -C "$REPO_ROOT" --no-print-directory print-DSP_JOBS JOBS=4
    [ "$output" = "DSP_JOBS=4" ]
    # Both container builds take it; nothing hardcodes nproc any more.
    [ "$(grep -c -- '-j$(DSP_JOBS)' "$REPO_ROOT/Makefile")" -eq 2 ]
    ! grep -q -- '-j\$\$(nproc)' "$REPO_ROOT/Makefile"
}

@test "the ctest build links std::filesystem for GCC 8" {
    # The test executables use std::filesystem through the core but do not link
    # stdc++fs, which GCC 8 keeps in a separate library: ctest fails to link
    # without it. The shipping binary gets it from upstream, so only ctest adds it.
    mk DSP_CTEST_LIBS
    [[ "$output" == *"-lstdc++fs"* ]]
    [[ "$output" == *"-lpthread"* ]]
    grep -q 'CMAKE_CXX_STANDARD_LIBRARIES="$(DSP_CTEST_LIBS)"' "$REPO_ROOT/Makefile"
}

@test "the ABI ceiling is asserted for every platform" {
    [ -x "$REPO_ROOT/scripts/check-abi.sh" ]
    grep -q "check-abi.sh \$(2)" "$REPO_ROOT/Makefile"
    for p in $(pak_platforms); do
        u="$(echo "$p" | tr '[:lower:]' '[:upper:]')"
        grep -q "stage-$p:.*${u}_GLIBC_MAX" "$REPO_ROOT/Makefile" || return 1
    done
}

@test "the GCC 8 platforms hold a tighter ceiling than the GCC 10 one" {
    mk TG5040_GLIBC_MAX
    [ "$output" = "2.28" ]
    mk H700_GLIBC_MAX
    [ "$output" = "2.28" ]
    # tg5050 ships a GCC 10.3 toolchain over a glibc 2.33 sysroot.
    mk TG5050_GLIBC_MAX
    [ "$output" = "2.33" ]
}

@test "every patch in patches/ is applied by the clone rule" {
    grep -q 'for p in $(PATCHES)/\*.patch' "$REPO_ROOT/Makefile"
    # A drifted patch must be fatal, not silently skipped.
    grep -q 'git apply "$$p"' "$REPO_ROOT/Makefile"
}

@test "minui-power-control is gone" {
    # Deep sleep is native now, and that binary has no h700 support anyway.
    ! grep -q "minui-power-control" "$REPO_ROOT/Makefile"
    ! grep -q "MINUI_POWER_CONTROL" "$REPO_ROOT/Makefile"
    ! grep -q "minui-power-control" "$REPO_ROOT/launch.sh"
}

@test "the vendored MinUI module is copied in before the patches apply" {
    # Patch 0003 only modifies files upstream already has, so the module has to
    # be in the tree first or the build fails at the CMake target_sources line.
    mk OVERLAY
    [ -n "$output" ]
    mk MINUI_OVERLAY
    overlay="$output"
    for f in minui.h minui.cpp minui_gfx.h minui_gfx.cpp minui_menu.h minui_menu.cpp minui_pad.h minui_shader.h minui_shader.cpp \
        minui_resample.h minui_resample.cpp minui_shader_presets.h minui_shortcuts.h minui_stb.cpp stb_image.h stb_truetype.h; do
        [[ " $overlay " == *" $f "* ]] || return 1
    done
    for f in $overlay; do
        [ -f "$REPO_ROOT/overlay/$f" ] || return 1
    done
    # And nothing in overlay/ is left behind by the list.
    for f in "$REPO_ROOT"/overlay/*.*; do
        [[ " $overlay " == *" $(basename "$f") "* ]] || return 1
    done
    copy_line="$(grep -n 'cp $(addprefix $(OVERLAY)/,$(MINUI_OVERLAY))' "$REPO_ROOT/Makefile" | head -1 | cut -d: -f1)"
    patch_line="$(grep -n 'for p in $(PATCHES)/\*.patch' "$REPO_ROOT/Makefile" | head -1 | cut -d: -f1)"
    [ -n "$copy_line" ] && [ -n "$patch_line" ]
    [ "$copy_line" -lt "$patch_line" ]
}

@test "the pause menu's ctest suite is copied in and registered" {
    mk MINUI_OVERLAY_TESTS
    [ "$output" = "minui_menu_test.cpp minui_shader_test.cpp" ]
    [ -f "$REPO_ROOT/overlay/tests/minui_menu_test.cpp" ]
    [ -f "$REPO_ROOT/overlay/tests/minui_shader_test.cpp" ]
    copy_line="$(grep -n 'cp $(addprefix $(OVERLAY)/tests/,$(MINUI_OVERLAY_TESTS)) $@/tests/' "$REPO_ROOT/Makefile" | head -1 | cut -d: -f1)"
    patch_line="$(grep -n 'for p in $(PATCHES)/\*.patch' "$REPO_ROOT/Makefile" | head -1 | cut -d: -f1)"
    [ -n "$copy_line" ] && [ "$copy_line" -lt "$patch_line" ]
    p="$REPO_ROOT/patches/0003-minui-integration.patch"
    grep -q '^+add_executable(test_minui_menu minui_menu_test.cpp' "$p"
    grep -q '^+add_test(NAME minui_menu COMMAND test_minui_menu)' "$p"
    # Not excluded the way the SDL-linked input test is: it needs no SDL.
    mk CTEST_EXCLUDE
    [[ "$output" != *minui* ]]
}

@test "patch 0003 never touches upstream's menu" {
    # The MinUI menu stands in for ds::sdl::Menu through its public API and
    # the settings/achievement host interfaces alone, so an upstream refactor
    # of menu.cpp cannot break it silently. Patching menu.{h,cpp} would undo
    # that.
    p="$REPO_ROOT/patches/0003-minui-integration.patch"
    ! grep -q '^diff --git a/src/frontend/sdl/menu\.' "$p"
    ! grep -q '^diff --git a/src/frontend/sdl/settings\.' "$p"
    grep -q '^+  ds::sdl::MinUiMenu menu;' "$p"
    grep -q '^-  ds::sdl::Menu menu;' "$p"
}

@test "stb is vendored unmodified, pinned, and built in one place" {
    for f in stb_image.h stb_truetype.h; do
        grep -q "public domain" "$REPO_ROOT/overlay/$f" || return 1
        grep -q "ALTERNATIVE A - MIT License" "$REPO_ROOT/overlay/$f" || return 1
        # Implementations only in minui_stb.cpp.
        [ "$(grep -l "define STB_.*_IMPLEMENTATION" "$REPO_ROOT"/overlay/*.cpp | xargs -n1 basename)" = "minui_stb.cpp" ] || return 1
    done
    grep -q 'nothings/stb at' "$REPO_ROOT/overlay/minui_stb.cpp"
    grep -qE '[0-9a-f]{40}' "$REPO_ROOT/overlay/minui_stb.cpp"
    # Only PNG and BMP: what the launcher's thumbnails are.
    grep -q 'define STBI_ONLY_PNG' "$REPO_ROOT/overlay/minui_stb.cpp"
    grep -q 'define STBI_ONLY_BMP' "$REPO_ROOT/overlay/minui_stb.cpp"
    # Its warnings are switched off, and nobody else's.
    p="$REPO_ROOT/patches/0003-minui-integration.patch"
    grep -q '^+set_source_files_properties(minui_stb.cpp PROPERTIES COMPILE_OPTIONS "-w")' "$p"
    [ "$(grep -c 'COMPILE_OPTIONS "-w"' "$p")" -eq 2 ]
}

@test "every profile opens the pause menu on a bare MENU tap" {
    for f in "$REPO_ROOT"/config/dsperate/*.ini; do
        sed -n '/^\[padhotkeys\]/,/^\[/p' "$f" | grep -q '^pause.alt = mod$' || return 1
    done
}

@test "every profile binds only the swap, pip and switcher chords" {
    for f in "$REPO_ROOT"/config/dsperate/*.ini; do
        chords="$(sed -n '/^\[padhotkeys\]/,/^\[/p' "$f" | grep '^[a-z_.]* = mod+' | cut -d' ' -f1 | sort | tr '\n' ' ')"
        [ "$chords" = "game_switcher pip_corner_next screen_swap " ] || {
            echo "$f binds $chords"
            return 1
        }
    done
}

# The Shortcuts page's table, as "key chord" lines, read from the header the
# way it promises to be laid out: one entry per line.
shortcut_table() {
    sed -n 's/^  {"\([^"]*\)", "[^"]*", "\([^"]*\)", ".*$/\1 \2/p' "$REPO_ROOT/overlay/minui_shortcuts.h"
}

@test "the shortcut table reads as one entry per line" {
    [ "$(shortcut_table | wc -l)" -eq 24 ]
    shortcut_table | grep -q '^pause.alt mod$'
    shortcut_table | grep -q '^pip_corner_next mod+start$'
}

@test "every profile sets every shortcut, to its chord or to none" {
    for f in "$REPO_ROOT"/config/dsperate/*.ini; do
        section="$(sed -n '/^\[padhotkeys\]/,/^\[/p' "$f")"
        while read -r key chord; do
            value="$(echo "$section" | sed -n "s/^$key = //p")"
            [ -n "$value" ] || {
                echo "$f has no $key"
                return 1
            }
            [ "$value" = "$chord" ] || [ "$value" = "none" ] || {
                echo "$f has $key = $value, not $chord"
                return 1
            }
        done < <(shortcut_table)
    done
}

@test "the state, slot, screenshot, mic and lid chords ship off, and MENU+START moves the pip screen" {
    for f in "$REPO_ROOT"/config/dsperate/*.ini; do
        section="$(sed -n '/^\[padhotkeys\]/,/^\[/p' "$f")"
        for key in save_state load_state slot_next slot_prev screenshot mic lid; do
            echo "$section" | grep -q "^$key = none$" || return 1
        done
        echo "$section" | grep -q '^pip_corner_next = mod+start$' || return 1
    done
}

@test "no two shortcuts share a chord" {
    [ -z "$(shortcut_table | awk '$2 != "none" { print $2 }' | sort | uniq -d)" ]
}

@test "the shader shortcuts ship unbound and come after the game switcher" {
    for key in colour_next colour_prev pattern_next pattern_prev preset_next preset_prev; do
        shortcut_table | grep -q "^$key none$" || return 1
    done
    # 0006 adds the actions to upstream's list after the one 0003 adds.
    p="$REPO_ROOT/patches/0006-shaders.patch"
    grep -q '^+  GameSwitcher, ColourNext, ColourPrev, PatternNext, PatternPrev, PresetNext, PresetPrev, Count$' "$p"
    grep -q '"game_switcher", "colour_next", "colour_prev", "pattern_next", "pattern_prev", "preset_next", "preset_prev"};$' "$p"
}

@test "the shader patch is present, adds no files and leaves upstream's menu alone" {
    p="$REPO_ROOT/patches/0006-shaders.patch"
    [ -f "$p" ]
    ! grep -q "new file mode" "$p"
    ! grep -q '^diff --git a/src/frontend/sdl/menu\.' "$p"
    # The module and its suite are built where 0003's are.
    grep -q '^+target_sources(dsperate PRIVATE minui_shader.cpp minui_resample.cpp)$' "$p"
    grep -q '^+add_executable(test_minui_shader minui_shader_test.cpp ../src/frontend/sdl/minui_shader.cpp ../src/frontend/sdl/minui_resample.cpp$' "$p"
    grep -q '^+add_test(NAME minui_shader COMMAND test_minui_shader)$' "$p"
    mk CTEST_EXCLUDE
    [[ "$output" != *shader* ]]
    # Every key documented in the reference config the pak ships.
    for key in "preset = off" "colour = off" "scaling = off" "filter = off" "pattern = off" "glow = auto" "curve = off"; do
        grep -q "^+# $key" "$p" || return 1
    done
    # GLOW holds lines back through a filter in front of the rest of the scaler.
    grep -q '^+void Gpu::emit_line(int screen, u32 line, const u32\* src) {$' "$p"
}

@test "the shaders keep the pow the ABI gate allows" {
    # std::pow would bind pow@GLIBC_2.29 on the newer toolchain; the core's
    # pow_compat.c pins the old symbol.
    ! grep -q 'std::pow\|[^_]pow(' "$REPO_ROOT/overlay/minui_shader.cpp" "$REPO_ROOT/overlay/minui_resample.cpp"
    grep -q 'ds_pow_compat' "$REPO_ROOT/overlay/minui_shader.cpp"
}

@test "the shaders' authors and licences ship with the pak" {
    [ -f "$REPO_ROOT/THIRD_PARTY.md" ]
    for who in kleinrein/rgds-core jdgleaver Gigaherz "Greg Hogan" hunterk Pokefan531 Themaister ShadX Hyllian Sp00kyFox; do
        grep -q "$who" "$REPO_ROOT/THIRD_PARTY.md" || return 1
        grep -q "$who" "$REPO_ROOT/overlay/minui_shader.h" || return 1
    done
    # NextUI's shaders, credited in the resampler's header as well.
    for who in Fes zadpos decavoid "Team XBMC" "Stefanos A." "guest(r)" "Andrea Mazzoleni" DariusG RiskyJumps davej; do
        grep -qF "$who" "$REPO_ROOT/THIRD_PARTY.md" || return 1
    done
    for who in Fes zadpos decavoid "Team XBMC" "guest(r)" "Andrea Mazzoleni" davej; do
        grep -qF "$who" "$REPO_ROOT/overlay/minui_resample.h" || return 1
    done
    grep -q 'GPL-2.0-or-later' "$REPO_ROOT/THIRD_PARTY.md"
    grep -q 'PolyForm Noncommercial' "$REPO_ROOT/THIRD_PARTY.md"
    grep -q 'cp THIRD_PARTY.md $(DIST)/THIRD_PARTY.md' "$REPO_ROOT/Makefile"
}

@test "the GCC 8 workarounds are still present" {
    # Both are compiler bugs, not upstream ones: drop them when the toolchain
    # moves past GCC 8, not before.
    [ -f "$REPO_ROOT/patches/0002-gcc8-neon-scale-row-grid-miscompile.patch" ]
    [ -f "$REPO_ROOT/patches/0005-gcc8-lto-layout-fp-contract.patch" ]
}

@test "the layout LTO workaround is scoped to GCC 8" {
    # GCC 9 and later keep -ffp-contract=off across LTO, so only GCC 8 should
    # lose the inlining.
    p="$REPO_ROOT/patches/0005-gcc8-lto-layout-fp-contract.patch"
    grep -q '^+if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)' "$p"
    grep -q '^+  set_property(SOURCE layout.cpp APPEND PROPERTY COMPILE_OPTIONS "-fno-lto")' "$p"
    ! grep -q "new file mode" "$p"
}

@test "patches upstream has absorbed are gone" {
    # 0001: the constexpr-in-nested-lambda rewrite landed upstream in v2.0.0.
    # 0004: upstream's quit_early now closes the display on every early return.
    # Either would fail to apply, which the clone rule treats as fatal.
    [ ! -f "$REPO_ROOT/patches/0001-gcc8-constexpr-in-nested-lambda.patch" ]
    [ ! -f "$REPO_ROOT/patches/0004-restore-display-on-early-exit.patch" ]
}

@test "the MinUI integration patch is present and adds no files" {
    p="$REPO_ROOT/patches/0003-minui-integration.patch"
    [ -f "$p" ]
    # A patch that creates files conflicts far more readily on a tag bump, and
    # the Makefile copies the vendored sources in instead.
    ! grep -q "new file mode" "$p"
    # Thumbnails reuse the emulator's own PNG writer. Both launchers read them
    # with IMG_Load, which sniffs magic rather than the extension, so there is
    # no reason to carry an image encoder of our own.
    ! grep -q "write_bmp" "$p"
    [ ! -f "$REPO_ROOT/overlay/minui_bmp.cpp" ]
}

@test "bump-version refuses to run without a version" {
    # `make release` calls it, and an unset RELEASE_VERSION would quietly rewrite
    # pak.json's version to the empty string.
    run make -C "$REPO_ROOT" --no-print-directory bump-version
    [ "$status" -ne 0 ]
    [[ "$output" == *"RELEASE_VERSION is not set"* ]]
    [ "$(jq -r .version "$REPO_ROOT/pak.json")" != "" ]
}

@test "release only bumps the version when one was given" {
    # ci.yaml runs `make release` to produce a reviewable artifact and sets no
    # RELEASE_VERSION. An unconditional bump-version there fails the build.
    grep -q 'if \[ -n "$(RELEASE_VERSION)" \]; then' "$REPO_ROOT/Makefile"
    ! grep -qE '^\t\$\(MAKE\) bump-version$' "$REPO_ROOT/Makefile"
}

@test "screenshots render the shipping menu, with fonts from pinned commits" {
    grep -q '^screenshots: clone verify-pin$' "$REPO_ROOT/Makefile"
    [ -f "$REPO_ROOT/scripts/render-menu.cpp" ]
    # The real menu is linked, not a copy of it.
    grep -q 'src/frontend/sdl/minui_menu.cpp' "$REPO_ROOT/Makefile"
    # And the shaders, through the core's pow.
    grep -q 'src/frontend/sdl/minui_shader.cpp' "$REPO_ROOT/Makefile"
    grep -q 'src/frontend/sdl/minui_resample.cpp' "$REPO_ROOT/Makefile"
    grep -q 'src/core/gpu/pow_compat.c' "$REPO_ROOT/Makefile"
    grep -q '#include "frontend/sdl/minui_menu.h"' "$REPO_ROOT/scripts/render-menu.cpp"
    # The fonts are fetched at an exact commit and never vendored.
    for v in MINUI_FONT_URL NEXTUI_FONT_URL; do
        mk "$v"
        [[ "$output" =~ ^https://raw.githubusercontent.com/[^/]+/[^/]+/[0-9a-f]{40}/ ]] || return 1
    done
    [ -z "$(find "$REPO_ROOT/overlay" "$REPO_ROOT/config" -name '*.ttf' -o -name '*.otf')" ]
    # Output stays under build/, which make clean removes.
    mk SCREENSHOTS
    [ "$output" = "$REPO_ROOT/build/screenshots" ]
}
