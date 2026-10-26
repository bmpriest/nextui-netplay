# Technical notes

Developer-facing detail for the build pipeline. For installation and usage see [README.md](README.md).

## Why this pak builds from source

Upstream publishes prebuilt aarch64 tarballs, and neither can load on any device this pak targets.

| Asset | libc floor | Notes |
| --- | --- | --- |
| `dsperate-vX-linux-aarch64-static.tar.gz` | `GLIBC_2.38` | Only libstdc++/libgcc are static; glibc is still dynamic |
| `dsperate-vX-linux-aarch64.tar.gz` | `GLIBC_2.38` | Also needs `GLIBCXX_3.4.30`, i.e. a GCC 12 libstdc++ |

Upstream CI builds on `ubuntu-24.04-arm`, glibc 2.39. The devices are at 2.33 (TrimUI SDK) and 2.35 (H700 stock), and NextUI's own cross toolchain targets 2.28. So the pak cross-compiles a pinned upstream tag in the LoveRetro toolchain images instead.

## Toolchains

| Platform | Image | Compiler | Sysroot glibc | CPU |
| --- | --- | --- | --- | --- |
| `tg5040` | `ghcr.io/loveretro/tg5040-toolchain` | GCC 8.3.0 | 2.28 | cortex-a53 |
| `tg5050` | `ghcr.io/loveretro/tg5050-toolchain` | GCC 10.3.0 | 2.33 | cortex-a55 |
| `h700` | `ghcr.io/loveretro/h700-toolchain` | GCC 8.3.0 | 2.28 | cortex-a53 |

The h700 image is the tg5040 image plus a patched mali-fbdev SDL2 (2.28.5) under `$PREFIX_LOCAL`. That is the SDL2 NextUI installs on those devices, and it is the one the h700 build must link.

Each image ships its own CMake toolchain file at `$CROSS_ROOT/Toolchain.cmake` and exports `CMAKE_TOOLCHAIN_FILE`, so the pak writes none. One gotcha: that file sets `CMAKE_CXX_FLAGS` as a plain variable, which shadows anything passed with `-DCMAKE_CXX_FLAGS`, so `-I$CROSS_ROOT/include/` has to be carried forward by hand in every configure line.

Do not use upstream's `cmake/aarch64-linux-gnu.cmake` or the `aarch64-cross` preset: both hardcode the Debian `aarch64-linux-gnu-` prefix and `/usr/aarch64-linux-gnu`, and the preset's single shared `build/aarch64` directory would have the three platforms clobber each other.

## Measured ABI

`scripts/check-abi.sh` runs after `--strip-unneeded`, on the exact bytes that ship. Measured at v3.0.5:

| Platform | Ceiling | Measured | NEEDED |
| --- | --- | --- | --- |
| `tg5040` | 2.28 | `GLIBC_2.18` | libSDL2, libdl, libpthread, libm, libc |
| `tg5050` | 2.33 | `GLIBC_2.33` | libSDL2, libdl, libpthread, libm, libc |
| `h700` | 2.28 | `GLIBC_2.18` | libSDL2, libdl, libpthread, libm, libc |

The ceiling is per platform because the toolchains genuinely differ, and each ceiling is its toolchain's own floor rather than the device's. That way a build reaching outside its sysroot fails loudly even though the devices would still have tolerated it.

`libpthread` in that list is the point of the `CMAKE_CXX_STANDARD_LIBRARIES` trick below — its absence would be the bug.

## Three silent failures the build asserts on

Each produces a binary that builds, links, boots and plays, just wrongly.

**JIT or NEON silently downgraded.** Upstream disables both with a plain `set(DSPERATE_JIT OFF)`, not `set(... CACHE ... FORCE)`, so `CMakeCache.txt` still reads `ON` afterwards and the NEON path prints no message at all. `scripts/assert-build.sh` therefore checks the generated `build.ninja` for the `-DDSPERATE_JIT=1` / `-DDSPERATE_NEON=1` compile definitions, which `src/core/CMakeLists.txt` emits only when the feature really survived. An interpreter-only build is far too slow to play and nothing in a device log would say why.

**The SDL frontend dropped.** If pkg-config cannot find sdl2, upstream prints `DSperate: SDL2 not found; the SDL frontend is not built` and carries on to a successful build with no `dsperate` target. Building `--target dsperate` rather than `all` turns that into a hard ninja error for free.

**The wrong SDL2 linked on h700.** `src/frontend/sdl/CMakeLists.txt` links `${SDL2_LIBRARIES}` and never references `${SDL2_LIBRARY_DIRS}`, and there is no `link_directories()` anywhere. `pkg_check_modules` puts bare names in `SDL2_LIBRARIES`, so the `-L` is discarded and `-lSDL2` resolves through the default search path into the sysroot. Fixing `PKG_CONFIG_PATH` alone would give patched headers over an unpatched library, and it would link without complaint. `scripts/docker-env.sh` therefore exports both a narrowed `PKG_CONFIG_PATH` and a `DSP_SDL_LDFLAGS` carrying the `-L`, and `assert-build.sh` greps the recorded link command to confirm it took. A wrong-SDL2 link is invisible to `readelf`, since the SONAME is the same either way.

## pthread

DSperate uses `std::thread`, and glibc 2.34 folded libpthread into libc; below that it is a real library. Upstream's SDL frontend links `Threads::Threads` since v2, but the core and the test executables still do not, so the pak keeps forcing it onto every link line.

It is passed as `CMAKE_CXX_STANDARD_LIBRARIES`, **not** `CMAKE_EXE_LINKER_FLAGS`. Linker flags land ahead of the objects that reference `pthread_create`, where `--as-needed` drops the library again; standard libraries are appended at the very end of the link line, the one place it survives. `--push-state`/`--pop-state` keeps `--no-as-needed` from applying to everything else on the line. `tests/makefile.bats` asserts this, because simplifying it into the linker flags regresses silently and only on glibc < 2.34.

## Netplay and Vulkan

Both are upstream options that default on, and both are set explicitly in `DSP_CMAKE_COMMON` rather than left to the default.

`DSPERATE_NET=ON` builds netplay, with upstream's vendored slirp and enet. Spelling it out means an upstream default change shows up as a diff here rather than as a feature quietly appearing or disappearing.

`DSPERATE_VULKAN=OFF`. None of these SoCs has Vulkan, and upstream's `find_path` for `vulkan/vulkan.h` deliberately looks outside the sysroot, so left alone the tier would be compiled in or out depending on whether the toolchain image happens to carry the headers.

## Patches

`patches/` is applied to every build by the `clone` rule. A patch that no longer applies is fatal, so a drifted patch cannot produce a green build with the workaround silently missing.

`0002` and `0005` are **GCC 8 compiler bugs, not upstream bugs**. Drop them when the tg5040 and h700 toolchains move past GCC 8, not before.

Two earlier patches are gone because upstream absorbed them. `0001` worked around GCC 8 rejecting a `constexpr` local read from a nested lambda in `render3d.cpp`; upstream took the same rewrite in v2.0.0 ([beebono/DSperate#1](https://github.com/beebono/DSperate/issues/1)). `0004` gave the display-engine and fbdev tiers destructors, because an early `return 1` from `main()` skipped `Display::close()`, aborted on a joinable presenter thread and left the launcher's UI layer off, which is how a failed RESUME became a hard reset in [#4](https://github.com/josegonzalez/minui-nintendo-dsperate-pak/issues/4). Upstream now routes every early exit after the display opens through a `quit_early` lambda that closes it, so the bug cannot occur ([beebono/DSperate#21](https://github.com/beebono/DSperate/issues/21)). The numbers are not reused.

**`0003-minui-integration.patch`** is not a compiler workaround and does not go away: it is the
launcher integration described below. It is the patch most likely to need a rebase on a tag bump,
which is why the logic it wires in lives in `overlay/` rather than in the patch itself.

**`0002-gcc8-neon-scale-row-grid-miscompile.patch`** — GCC 8 miscompiles the NEON `scale_row_grid` kernel. Bisected with `-mcpu` held constant so the compiler was the only variable:

| Compiler | `-mcpu` | `gpu` / `kernels` |
| --- | --- | --- |
| GCC 8.3.0 | cortex-a53 | FAIL |
| GCC 10.3.0 | cortex-a53 | pass |
| GCC 10.3.0 | cortex-a55 | pass |

Not an optimiser level: it reproduces at `-O3`, `-O2` and `-O1`, with and without LTO, and with `-fno-strict-aliasing`. The patch falls back to the portable reference on GCC 8 only; GCC 9 and later keep the NEON path, so tg5050 is unaffected. It costs performance only in the LCD-grid and chunky filters, which are off in the shipped default configuration.

This is exactly what `ctest` is in the pipeline to catch, and it caught it on the first run.

**`0005-gcc8-lto-layout-fp-contract.patch`**: upstream compiles `src/frontend/video/layout.cpp` with `-ffp-contract=off`, so screen placement, which is float arithmetic truncated to pixels, lands on the same pixel everywhere. `ctest`'s `layout` test pins it with a hash over a sweep of layouts. With LTO on, GCC 8 loses that per-file option once the layout functions are inlined into callers compiled without it, and the `dominant_v` layout puts its second screen a pixel lower in 13 of 3942 sweep cases. That affects the shipping binary on tg5040 and h700, not just the test. Bisected the same way as `0002`:

| Compiler | Build | `layout` |
| --- | --- | --- |
| GCC 8.3.0 | `layout.cpp` off, the rest LTO-linked without it | FAIL |
| GCC 8.3.0 | the same, with `-ffp-contract=off` on the link too | FAIL |
| GCC 8.3.0 | `layout.cpp` built with `-fno-lto` | pass |
| GCC 10.3.0 | `layout.cpp` off, the rest LTO-linked without it | pass |

The patch builds `layout.cpp` outside LTO on GCC 8 only. Placement runs only when the layout or the output size changes, never per frame, so nothing is lost by not inlining it. Reported upstream as [beebono/DSperate#23](https://github.com/beebono/DSperate/issues/23).

**`0006-shaders.patch`** is not a workaround either: it wires in the Visual FX page's SHADER PRESET row and the Shaders page under it, described under "The shaders" below. Like 0003 it adds no files, the module being `overlay/minui_shader.{h,cpp}`, `overlay/minui_resample.{h,cpp}` and `overlay/minui_shader_presets.h`. It applies after 0003 and builds on a few of 0003's lines (the `GameSwitcher` action in `input.{h,cpp}`, `fade_out` and the hotkey notice in `main.cpp`), so rebasing 0003 can mean regenerating 0006.

## Tests

`make test-native` builds the upstream suite in its own `build/ctest` directory with `-DDSPERATE_TESTS=ON`, so the three shipping builds keep `-DDSPERATE_TESTS=OFF` and never carry test binaries.

It runs once, with the tg5040 flag set. All three platforms build identical source with identical options and only `-mcpu`/`-mtune` differ, which changes scheduling and not semantics; using the cortex-a53 build keeps the tested binary a strict ARMv8-A baseline. CI runs on `ubuntu-24.04-arm`, so the container is native arm64 and the cross-built binaries execute directly — no `qemu-aarch64-static`.

The ctest build also links `-lstdc++fs`. The test executables use `std::filesystem` through the core, which GCC 8 keeps in a separate library, and upstream only links it into `dsperate` itself. GCC 10 still ships the library, so it needs no compiler check. Reported upstream as [beebono/DSperate#24](https://github.com/beebono/DSperate/issues/24).

Every test executable is an LTO link of the whole core, and at one job per CPU those links can run a desktop Docker VM out of memory, which shows up as `Error 137`. `JOBS` caps the parallelism of both the shipping and the ctest builds; `make build JOBS=4` matches the 4-core CI runner.

`minui_menu` is the pak's own suite, `overlay/tests/minui_menu_test.cpp`, copied into `tests/` by the `clone` rule and registered by patch 0003. It drives the pause menu with fake settings and achievement hosts the way upstream's `menu` test drives upstream's, draws every page into a guarded canvas at every panel size, and checks that a thumbnail written the way `write_png` writes one comes back through stb_image. It needs no SDL, so it runs in the container.

`minui_shader` is the shaders' suite, `overlay/tests/minui_shader_test.cpp`, registered by patch 0006. It checks every colour table entry against the double-precision maths and some presets against their GLSL written out again; the NEON pattern kernel against its reference at every width up to a panel row, which is how a GCC 8 miscompile like 0002's would show; the pattern rows against the pattern at the phase each panel pixel is drawn in, at every panel size and layout; and the scaler itself, through `scale_image`, with the hooks off (upstream's nearest to the byte), on, and cropped by an integer overscale. It holds the preset table to the rows' values, each resampler to its shader's maths and to nearest where the maths says it is nearest, a resampler run line by line through the scaler to the same resampler over a whole frame, the filters and their chain, the NextUI patterns, and the curve's map to barrel-distortion's maths. It prints what each preset costs per frame there, for comparing presets; the CI runner is not a device.

`input` is excluded. It links SDL2, the only libSDL2 in the container is the sysroot's, and putting that on `LD_LIBRARY_PATH` drags its glibc in front of the container's loader: `undefined symbol: __libc_vfork, version GLIBC_PRIVATE`. Nothing to do with the code. `RUN_CTEST=0` skips the suite entirely, `CTEST_EXCLUDE` adjusts the filter.

`make test` is the host-side bats suite, which needs no toolchain: `abi.bats` drives `check-abi.sh` through a stubbed `readelf`, `makefile.bats` reads the build wiring through `print-%`, and `platform.bats`/`launch.bats` source the launcher with `DSP_PAK_TEST=1`.

## PGO is off and cannot be otherwise

`pgo/aarch64/MANIFEST` records `compiler 13.3.0 (aarch64-linux-gnu)` and a fingerprint over the compiler and flags, and upstream's configure calls `message(FATAL_ERROR)` on a mismatch. Our compilers differ and each platform adds a different `-mcpu`, so `use` could not match for more than one of them even in principle. `DSPERATE_PGO_DIR` also defaults inside the cloned tree that `make clean` deletes, and the hottest code on the device is JIT output that ahead-of-time profiling cannot touch. `DSPERATE_PGO` stays an overridable variable so the experiment is one flag away.

## Bumping the upstream version

Dependabot only covers Actions, so `.github/workflows/update-dsperate.yaml` watches for DSperate releases instead. Once a day it compares upstream's latest release with `DSPERATE_TAG`. On a new one it runs `scripts/bump-dsperate.sh`, which rewrites the pin in the `Makefile` and the version the README names, then `make clone` to see whether every patch still applies. It opens a pull request on `build/update-dsperate-<tag>` saying which patch, if any, failed. It skips a release that already has a branch, so a rebase pushed there is never reset.

A pull request opened with the default `GITHUB_TOKEN` does not trigger `ci.yaml`, so the workflow uses a `GH_ACCESS_TOKEN` repository secret when there is one, a token with contents and pull-requests write access. Without it the pull request still opens, but nothing builds it until someone pushes to the branch.

Pinning is all the workflow does. Reviewing the pull request is the rest of the bump:

1. Read the upstream release notes for CMake option changes. A renamed switch fails open, because CMake ignores unknown `-D` cache args, and `assert-build.sh` is the only thing that would catch the consequence. Look for a new option that defaults on, too, and decide it explicitly in `DSP_CMAKE_COMMON`.
2. Every patch must still apply. If one does not, check whether upstream fixed it before rebasing it on the branch.
3. Check the release notes for removed ini keys, which upstream ignores silently, and for a save state format change. `dsperate.state-version` follows the format on its own, but users lose their old states and the release notes should say so.
4. Re-measure the ABI table above.

`tests/makefile.bats` asserts the pin is a tag rather than a branch and that the README names it, and `make verify-pin` asserts the checkout matches. To bump by hand, run `scripts/bump-dsperate.sh <tag>`.

## The MinUI integration

The files in `overlay/` (named by `MINUI_OVERLAY` in the `Makefile`) are vendored here and copied
into the DSperate tree by the `clone` rule, along with the pause menu's test suite from
`overlay/tests/`; `patches/0003-minui-integration.patch` only ever modifies files upstream already
has. That split is deliberate: a patch that creates files conflicts far more readily on a tag bump,
and 0003 is the patch most likely to need a rebase.

### The power button is read from evdev, not SDL

`minui-n64-pak` reads it as SDL scancode 102, which works because GLideN64 runs on a real video
driver. DSperate does not: on the disp and fbdev tiers it forces a headless SDL video driver
(`go_headless()`), whose window never takes keyboard focus, and `KEY_POWER` sits on its own evdev
node that SDL's keyboard backend does not surface either way. So the module opens
`/dev/input/event*` directly and looks for `EV_KEY`/`KEY_POWER`, modelled on DSperate's own
`lid.cpp`. Nothing contends for it — NextUI's keymon opens those nodes read-only without
`EVIOCGRAB` and handles no power code.

Backlight is one mechanism for all three platforms: `/sys/class/backlight/backlight0/brightness`
if it is there, otherwise `/dev/disp` ioctl `DISP_LCD_SET_BRIGHTNESS` (`0x102`). That ioctl is what
NextUI's own `libmsettings` uses on both tg5040 and h700. There is no read side to it, so where
sysfs cannot be read the module recovers the level from NextUI's `minuisettings.txt` rather than
waking to a hardcoded guess the user never chose.

`/sys/class/speaker/mute` is TrimUI-only and the write is best effort; h700 has no such node.

### The artifact contract

Keys are the ROM basename **with** its extension, which is what minarch keys on. `<EMU>` is `DSP`,
from the last parenthesised group of the Roms folder.

| Artifact | Path | Content |
| --- | --- | --- |
| Battery save | `/Saves/DSP/<rom stem>.sav` | DSperate's own |
| Save state | `$SHARED_USERDATA_PATH/DSP-dsperate/<rom stem>.<slot>.dss` | `emu.state_key = rom` |
| Slot thumbnail | `.minui/DSP/<rom.ext>.<0-7>.bmp` | 32-bit BI_RGB BMP |
| Slot marker | `.minui/DSP/<rom.ext>.txt` | one ASCII int, no newline |
| Resume after sleep | `.minui/auto_resume.txt` | card-relative ROM path, no newline |
| Game switcher | `.minui/game_switcher.txt` | existence only |
| Resume request | `/tmp/resume_slot.txt` | read and consumed by launch.sh |
| Readable state format | `<pak>/dsperate.state-version` | one ASCII int, written at build time |
| Screenshot | `/Screenshots/<display name>.<YYYY-MM-DD-HH-MM-SS>.png` | PNG; the name as NextUI's minarch works it out (below) |

Five things here are silent when wrong, so they have tests:

- **No trailing newline** on `auto_resume.txt` or the slot marker. The frontend concatenates
  without trimming, so one `\n` makes the resume quietly never happen. The module uses `fputs`;
  shell must use `printf '%s'`, never `echo`.
- **The ROM path must be card-relative.** An absolute path becomes `/mnt/SDCARD/mnt/SDCARD/...`
  and is dropped. launch.sh exports nothing rather than something wrong when the ROM is outside
  the card.
- **`autoload` alone is not enough.** minarch writes `8` to `/tmp/resume_slot.txt` for a plain
  launch, meaning "start fresh"; with the shipped `autoload = true` the pak would resume anyway,
  so launch.sh translates the marker into `--load-state` or `--no-autoload`.
- **The state path is one argument.** ROM names nearly always hold spaces, and DSperate takes
  every stray positional word as the ROM, so a split `--load-state` path boots nothing and exits.
  launch.sh builds the argument list with `set --` rather than expanding a word list, and
  `tests/launch.bats` checks the argv DSperate actually receives. A slot whose `.dss` is missing
  is dropped in favour of autoload, because an explicit `--load-state` that cannot be read fails
  the run, after holding the panel on a "STATE NOT LOADED" screen for up to a minute.
- **A state must be in a format this DSperate reads.** A `.dss` is `DSST` and a little-endian u32
  format version, and upstream refuses anything older than its `OLDEST_READABLE_VERSION`, which
  v3.0.0 raised past every state an earlier pak wrote. `dist-common` reads that constant out of
  `src/core/state/state.h` into `dsperate.state-version` at the pak root, and launch.sh compares a
  slot's header against it and falls back to autoload on a mismatch. A missing file or an `od`
  that cannot read the header skips the check rather than dropping a good state.

Slots follow minarch: 0-7 are the player's and are the only ones the switcher lists, 8 is its
hidden default, and 9 is the sleep autosave, which maps onto DSperate's own `.auto` slot. Matching
minarch means the sleep path writes no thumbnail and no marker — the switcher resolves its picture
through the marker, which only ever holds 0-7.

Deleting a slot from DSperate's pause menu also deletes its thumbnail, and the marker when it names
that slot, so the switcher stops offering a resume with nothing behind it.

Thumbnails are written with DSperate's own `write_png()`, at the `.bmp` path the launcher looks
for. That is not a mismatch: NextUI writes PNG bytes into that same name (`ma_menu.c:1604`,
`IMG_SavePNG_RW`) and MinUI writes a real BMP (`minarch.c:4148`), and both read with `IMG_Load`,
which dispatches on the magic bytes rather than the extension. MinUI's SDL_image certainly has PNG
— it loads its own UI atlas that way at `common/api.c:150` — so there is no reason for the pak to
carry an image encoder of its own.

### The pause menu

`overlay/minui_menu.{h,cpp}` is minarch's in-game menu: Continue, Save, Load, Options and Quit over the dimmed game, the game's name in a pill, the battery, the button hints, and a preview window that shows the launcher's own thumbnail of the selected slot. Options holds every page DSperate's own menu has, drawn as minarch draws its option lists. The layout units, colours and offsets are minarch's (`PILL_SIZE`, `BUTTON_SIZE`, `PADDING` and the rest), times 3 on the 1024x768 Brick and 2 everywhere else, as minarch's `FIXED_SCALE` is. Neither MinUI (no licence) nor NextUI (PolyForm Noncommercial) is copied from: they are the reference for numbers and behaviour only, and everything is drawn procedurally rather than from the launcher's `assets@2x.png`.

It replaces upstream's menu by type and nothing else. `MinUiMenu` has `ds::sdl::Menu`'s public API method for method and uses `Menu`'s own public types (`Menu::Result`, `Menu::GameEntry`, `Menu::kAutoSlot`), so main.cpp's switch over results and its seven draw calls compile unchanged: the patch swaps one declaration. Settings, bindings, cheats and achievements come only through the interfaces upstream already keeps `menu.cpp` behind, `SettingsHost`, `CheevosHost`, the `Setting` tables and `cheat::Code`, never through `Menu`'s internals. `menu.h` and `menu.cpp` are not patched at all, and `tests/makefile.bats` asserts that. What an upstream change does to it:

| Upstream change | Effect |
| --- | --- |
| main.cpp calls a new `Menu` method | Compile error; add a forwarder |
| A `SettingsHost` or `CheevosHost` signature changes | Compile error |
| A settings row is added, renamed or removed | Picked up: the settings pages walk upstream's tables |
| A `Menu::Result` value is added | Harmless; the pak never returns it |
| `Input`'s modifier flags are renamed | Compile error in the one-line `input.h` hunk |

None of them fails silently. Beyond `Menu`'s API, `MinUiMenu` has a few methods of the pak's own, the layout notice and the Shortcuts page's inputs, which only the 0003 hunks in main.cpp call. Two things stay upstream's, on a `Menu` the pak's menu embeds: the game picker a cart-less firmware boot raises, which a pak never does, and everything, when no `MINUI_DIR` names a launcher, so a desktop build is exactly upstream's.

Behaviour that differs from upstream's menu, all of it minarch's:

- Save leaves the menu. The menu returns `Save`, the frontend saves, and the next tick returns `Resume` with nothing pressed, which closes through the frontend's ordinary path.
- The root's slot picker has minarch's eight slots, all the launcher's switcher lists. The slot hotkeys still reach 8 and 9, so Options > Manage Save Slots lists all ten and the auto state, and deletes on a second A.
- Lists wrap at the ends, except cheats and achievements, which can be thousands of rows long.
- Reset, Save Changes (upstream's "SAVE TO" switch) and the slot page live under Options rather than on the root.

Every tier hands the menu a different canvas, and the drawing is straight-alpha source-over that is correct on all of them. On the fbdev and DRM scanline tiers the canvas is the scanout frame, already holding the dimmed game, so it is opaque. On the display-engine tier it is an ARGB overlay cleared to 0 that the display engine blends per pixel (`alpha_mode 0`, not premultiplied), and on the GPU tier an overlay blended `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`; there a pill's smoothed edge has to come out partially transparent rather than blended against black. The root draws over the game, which the frontend has already dimmed by half, as minarch's 50% overlay does. The options pages paint the canvas opaque, as minarch clears to black. On the SDL renderer tier the canvas is a 256x192 scratch that is scaled like a frame; the menu draws there at a fraction of a unit, legible but small.

`launch.sh` picks the look, in `dsp_detect_style`, and the menu reads it from the environment:

| Variable | Value |
| --- | --- |
| `MINUI_STYLE` | `nextui` when `.userdata/shared/minuisettings.txt` and `.system/res/font1.ttf` both exist, else `minui` |
| `MINUI_FONT` | NextUI: `font1.ttf`, or `font2.ttf` for `font=0`, or the file `font=` names. MinUI: `BPreplayBold-unhinted.otf`. Empty when the card has none, which draws with DSperate's 5x7 font |
| `MINUI_COLOR1..7` | NextUI's `color1..7` as written (`0xRRGGBBAA`); unset ones keep NextUI's defaults |
| `MINUI_PROFILE_INI` | The profile the config was seeded from, which Shortcuts > Defaults restores |
| `MINUI_PAD_BUTTONS` | The control names in the pad mapping DSperate added, which the Shortcuts page's modifier is picked from. Set by DSperate itself, not `launch.sh`; unset with no mapping |
| `MINUI_BUTTON_LABELS` | `name=LABEL` pairs for buttons the device labels differently from SDL, such as the Brick's `leftstick=F1 rightstick=F2`. Empty elsewhere |
| `MINUI_NOTIFY_SCREENSHOT` | `0` when NextUI's `notifyScreenshot=0`, else `1`; stock MinUI has no such setting |
| `MINUI_SLOW_SHADERS` | `config/platform.sh`'s `PROFILE_SLOW_SHADERS`, the shader values the Shaders page marks as slow (see "The shaders") |
| `MINUI_SHADER_TIMING` | `1` while `shader-timing.txt` sits beside `device.txt`: DSperate logs what the shaders cost a frame |

NextUI's look puts the title in an accent pill (color2) and the selection in color1 with color5 text, with no text shadow. MinUI's is a black title pill, a white selection with black text, and a black shadow under every other row. The `font=` reading follows NextUI's `config.c`, where `0` and `1` are the numbers older releases wrote.

Text is drawn with the launcher's own font through `stb_truetype.h`, and thumbnails are decoded with `stb_image.h`, both nothings/stb at the commit `minui_stb.cpp` names, unmodified, and public domain or MIT at the user's choice, which GPLv3 can absorb. Their implementations are compiled once, in `minui_stb.cpp`, with `-w`. stb_image decodes PNG and BMP by content, so it reads the PNG bytes this pak and NextUI write under the launcher's `.bmp` name, and the real BMP MinUI writes. Neither header adds a library: stb_image has its own inflate, and stb_truetype needs only libm, which the binary already links. BPreplay is a CFF-flavoured OTF, which stb_truetype has read since 1.13. Font sizes match SDL_ttf's (the em height in pixels), so minarch's `FONT_LARGE` and the rest come out the same size.

A bare MENU tap opens the menu because upstream already fires a hotkey bound to `mod` alone when the modifier is released with nothing else pressed (`Bind::ModAlone`), and the profiles bind `pause.alt = mod`, and the `Pause` action already closes an open menu. The profiles leave `pause` itself unbound, along with every chord but `screen_swap`, `pip_corner_next` and `game_switcher`.

The profiles are seeded once, so `dsp_migrate_config` brings an older config up to date in one awk pass, which adds a key under `[padhotkeys]` once and leaves alone any value the player set there, `none` included. It adds `pause.alt = mod`; `pip_corner_next = mod+start`, unless some key already holds `mod+start` or there is no `pause` line, since upstream's default `pause` is `mod+start`; and `none` for `save_state`, `load_state`, `slot_next` and `slot_prev`, which now ship off, and for `mic`. The profiles never set `mic` before, so upstream's default, a held L3, was live, and on the stick profiles L3 is also `stylus_button.alt`: one press tapped the pen and blew into the mic. `quit`, `pause`, `layout_next`, `layout_prev`, `fps`, `fast_forward_toggle` and `screenshot` ship off now too, but an older config keeps the chords its profile gave it.

### The Shortcuts page

Options > Shortcuts binds the pak's modifier chords the way NextUI's Shortcuts page does, and picks the modifier. A on a row listens for the next button: pressed alone it is bound alone, and pressed with the modifier held it becomes a modifier chord (`mod+x`). The modifier pressed and released on its own binds its tap, `mod`. X clears the row to `none`. Both move on to the next row, as NextUI's do. The listening is upstream's own capture, the one the Controls page uses, so triggers and sticks are bound as the axes upstream reads.

The chords live in one table, `overlay/minui_shortcuts.h`. Which ones ship bound is the profiles' business: the table's chord is only what Defaults falls back to when there is no profile to read. Every profile lists every key in the table explicitly, and `tests/makefile.bats` reads the table out of the header to hold each bound one to its chord. A key the config does not hold runs with upstream's default, which the table records too, so the page shows what is actually live.

Every change goes through `SettingsHost::bind`, as the Controls page's do: it lands in the global config or the game's, as Save Changes says, and the input is reconfigured at once. A binding reads the way NextUI's do, `MENU+R1`, `L2`, `NONE`, with the modifier under its own label. Upstream reads `a`, `b`, `x` and `y` as the south, east, west and north buttons, and every supported pad has Nintendo's layout, so they read as B, A, Y and X. The Controls page's pad column gets the same names: upstream spells them with a position pip, a control byte the TTF font has no glyph for, and a compass point (`\x02 EAST`), and calls MENU `FUNC BTN` and the modifier `MOD`, so `MinUiMenu::controls_label` swaps those for the Shortcuts page's names.

The modifier steps through MENU, SELECT, START, L3, R3 and SDL's `misc1` and `paddle1-4` with left, right or A, but only those `MINUI_PAD_BUTTONS` names, so it never offers a button the pad mapping does not have. SELECT and START are DS buttons as well; as the modifier upstream withholds them from the game while held and passes a lone press on as a tap, unless something is bound to the tap, as `pause.alt` is. The page says so.

The page will not clear both `pause` and `pause.alt`, since nothing else opens the menu that binds them again. Y restores the modifier and every key from the profile the config was seeded from (`MINUI_PROFILE_INI`), after a second press. X and Y go by position on a pad, as on the Controls page. The Controls page's own Defaults stays upstream's for the `[pad]` rows, since `bind()` cannot take the profiles' values for those back (it refuses `pad.stylus_axis = right`, wanting a captured axis); the chords it also resets are then put back from the profile, as Y here does.

With a pad, the Controls page's pad column leaves out the modifier and every key the Shortcuts page binds, held fast forward included, along with their second bindings (the `.alt` "(2)" rows upstream adds once one is bound), so each chord is changed in one place. Both columns also leave out `volume_up`, `volume_down`, `mute` and `fullscreen`: the launcher owns the volume, and the pak always runs full screen.

The six shader shortcuts (`colour_next`, `colour_prev`, `pattern_next`, `pattern_prev`, `preset_next`, `preset_prev`) are rows of the same table, unbound in every profile. They are actions 0006 adds to upstream's list after 0003's `game_switcher`.

### The layout notice

When a hotkey changes the layout, swaps the screens or moves the pip screen, the menu draws the setting's name at the top centre of the game, as `LAYOUT: DOMINANT V`, in its title pill. The label and value come from the same upstream settings tables the Layout page shows. It holds for three seconds and fades out over half a second, timed on `SDL_GetTicks()` rather than frames, so fast forward does not cut it short. The fade is a `Painter` alpha, so it is straight alpha like the rest of the menu and right on every tier; the canvas tiers note its rectangle, and the frame after it ends too, as upstream's toasts do. Upstream's slot label and FPS counter keep the top corners, and its toasts the bottom.

A screenshot puts up `Screenshot saved` in the same pill, as NextUI's minarch notifies, and only when NextUI's `notifyScreenshot` is not `0`. Its name follows NextUI's minarch too (`getDisplayName`, then `getAlias`): the alias for the ROM's filename in the `map.txt` beside it, tab separated, when there is one; otherwise the filename less any one-to-four-letter extensions (`.p8.png` both), less everything from the first `(` and then the first `[`, unless that leaves nothing. The pause menu's title pill shows the same name, as NextUI's does. The picture itself stays the DS screens at their own size in the current layout, which `write_png` draws; NextUI captures the whole panel instead.

### Saving on the way out

MENU+SELECT writes a state and the launcher's thumbnail, then quits, which writes the auto state too: two state files back to back, each several megabytes on the SD card, with nothing feeding the audio device. ALSA logged `underrun occurred` and the music stopped. The `GameSwitcher` hunk pauses the audio first, since the pak is leaving anyway.

A plain quit writes the auto state alone, but the same way, and logged the same underrun. The 0003 hunk at the end of the main loop pauses the audio before it too.

Before that write, the screen fades to black in 8 steps of 20 ms, as the pico-8 pak's Quit does, so a still frame is not left up while the state is written and the launcher starts again. The fade redraws the game's last frame darker each step through the same path the pause menu draws its dimmed backdrop with, starting at half brightness when the menu was open, and on the display-engine tier clears the overlay layer the menu was drawn on. It is skipped on a signal, where whoever sent it is taking the panel back.

Every launch also runs `launch.sh` before DSperate, and each process it starts costs the slower devices tens of milliseconds. So the config migration is one `awk` pass that writes nothing when there is nothing to add, and NextUI's `minuisettings.txt` is read with the shell's own `read` rather than a `sed` pipeline per key.

Both ends of a launch are timed in the log. `launch.sh` writes `timing:` lines from `/proc/uptime` at the launch, once the config is ready, once the launcher's look is read, once the pad is mapped, and either side of the emulator, and `cleanup` writes one once the card is synced. `MinUi::mark` writes DSperate's own on the same clock (`CLOCK_BOOTTIME`) as the main loop starts, as it ends and once the auto state is saved, so a slow start or exit can be split into its steps from a single log. `cleanup` used to wait two seconds after asking DSperate to stop, even when it had already exited; it now waits only when `pidof` still finds it, which is when the pak is being stopped from outside.

`make screenshots` renders the menu without a device. `scripts/render-menu.cpp` links the shipping menu, draws every page over a generated game picture with fake settings, cheats and achievements, and writes 256 PNGs to `build/screenshots/`: 16 pages and the layout notice held and fading, both looks, at 640x480, 720x720, 1024x768 and 1280x720, and a stand-in DS screen through every SHADER PRESET, each SCALING, FILTER and GLOW alone, `crt` without its halation, and the curve, at 640x480 and 1024x768 (`shader-<preset>-<size>.png`), drawn with the tables that ship. It is built with the tg5040 toolchain and run in that container, as ctest is, so it needs an arm64 host or qemu. The launcher fonts are fetched from MinUI and NextUI at pinned commits rather than vendored, because neither licence lets them ship here; offline, the built-in font stands in.

That tap has one trap. MENU+VOL is the launcher's brightness chord, which its own daemon reads over evdev, so SDL sees only a MENU press and release with nothing between them, which is a tap. `MinUi` therefore also watches the evdev nodes that report `KEY_VOLUMEUP`/`KEY_VOLUMEDOWN`, and when one goes down the frontend calls `Input::spoil_mod_tap()`, the one-line `input.h` hunk, which marks the modifier as used so its release fires nothing.

### The shaders

rgds-core's screen shaders are GLES2 fragment programs, run by hooking `SDL_RenderCopy`, and only on its SDL GL renderer; on its own direct present they show the original pixels. This pak's devices draw through fbdev (h700) or the Allwinner display engine, where there is no GL stage, and the SDL renderer tier is the slow fallback. NextUI's run as minarch's GLES passes. So the presets are done on the CPU, rgds-core's each split into two parts:

- **COLOUR** depends on the pixel alone. Each preset is baked into one table of 2^18 entries (1 MB), one per colour the DS can show: the output stage widens 6-bit channels to 8 by repeating their top bits, so the top 6 bits of each channel index it exactly. A 2^15 table (5 bits a channel, 128 KB) was measured first and rejected. It is exact for 2D palette colours, but 3D output, master brightness and a bitmap's green carry a sixth bit, and dropping it put dark shades up to 57/255 off on `natural_vision`, 25 on `gba` and 23 on `nds`.
- **PATTERN** depends on where a panel pixel sits inside its DS pixel. Per view, `build_pattern` makes one row of multipliers and offsets (`c * mul / 128 + add`, per byte) per class of panel row. The phase comes from the run the nearest scaler actually draws (`xrun`, and `emit_scaled`'s line spans), so every seam lands on a DS pixel's edge. Rows that come out the same share a class, which leaves 2 to 7 classes at the pak's panel sizes. A view at 1x or smaller gets no table. The RG DS patterns fade in between 1x and 2x as rgds-core's do.

Colour runs first, on each 256-pixel line at the top of `Gpu::emit_scaled`, so chunky, bilinear and the pattern all see the coloured line. The output stage's own line, and so `fb_`, is never written. Screenshots, thumbnails, save states and display capture keep the game's own colours, and the frontend colours its copies of `fb_` itself: the tiers that take frames whole, the pause menu's backdrop and the quit fade. The pattern runs on the plain nearest path. Each distinct class in a DS line's span is patterned once into a scratch row (`modulate_row`, NEON with a reference twin) and copied to the rows of that class, keeping upstream's copy-per-row shape.

`crt` is the pak's own: a beam profile across each DS row (`mix(0.45, 1, exp(-8 (fy - 0.5)^2))`) times rgds-core's box-filtered RGB stripe coverage at +-25%, lifted by 1.2 and faded in like the RG DS patterns. The test holds its average over a pixel within 10% of 1. It tints each channel, so it is not a mask.

Beam spread: a pattern can carry a second, bright, row of multipliers per class (`PatternRows::bright`), and each pixel's multipliers move from the dark row to the bright one by `weight[mean of its channels]`, a 256-byte smoothstep table (`spread_weights`). `crt`'s bright beam is `mix(0.80, 1, exp(-3 (fy - 0.5)^2))` lifted by 1.05, so bright rows all but fill the gap; `rgb_stripes` drops its stripes above 75%, which makes Vivid LCD exact. The NEON kernel looks the weight up with four `tbl`/`tbx` (AArch64 only) and blends in 16-bit lanes with a rounded divide by 255 that the reference shares, so the two agree to the bit. It costs about five times a plain pattern.

GLOW runs at DS resolution after COLOUR, so it works on every tier. A Gaussian (sigma radius / 2, weights in 1/256 summing to 256) blurs the light across each line as it is pushed (`GlowFilter::push`, NEON) and down the lines around it as it is pulled (`pull`), then screen-blends it back: `c + k g (255 - c) / 65280`, k being strength and, for halation, a warm tint (1, 0.6, 0.35). Bloom's light is what is above the threshold, stretched back to the full range. The blur needs the lines below each one, so patch 0006 splits `Gpu::emit_scaled`: after the colour, a line goes to `ScaleTarget::filter_push`, and line L comes back from `filter_pull` and on through the rest (`emit_line`) once line L + radius has gone in, the last lines with line 191. That is the same frame, `radius` lines late, at most 8. `GlowFilter::frame` does the same over a whole frame for the copies, and the test holds the two to the byte. `auto` resolves to the crt pattern's own halation (strength 0.2, radius 4) where crt is drawn. It costs 0.36 to 0.8 ms a frame for both screens on this machine, from radius 2 to 8.

SCALING is NextUI's resampling shaders, and rgds-core's sharp-bilinear, on the CPU (`overlay/minui_resample.{h,cpp}`). A `Resampler` per view takes the scaler's place through `ScaleTarget::resample`, after COLOUR, FILTER and GLOW: it keeps the last few DS lines in a ring and writes each panel row once the last DS line that row needs has arrived (`last_line`), so the frame still goes out line by line, then runs the pattern over the row as the nearest path does. Each follows its GLSL the way NextUI's minarch runs it, at the view's size, sampled with the filter libretro's preset names (linear for the sharp ones, nearest for the rest), clamped at the edges. The two-tap ones (`sharp_bilinear`, `pixellate`, the three shimmerless, `edge1pixel`) are separable: per-axis taps and weights built once, rows that come out the same drawn once, NEON blends in 16-bit lanes. `pixellate` blends in linear light, as its own default says. `smooth_3x` is NextUI's pixelperfect-smooth: nearest at three times, then linear to the view. `bicubic` is four taps an axis; `scale3x` and `aa_4` work from each DS pixel's neighbours and cache their results by the pixel's 3x3 or 2x2 position; `waterpaint` follows its own loops. `shimmerless_grid` brings its own grid as a pattern when PATTERN is off.

FILTER is NextUI's `fast-sharpen` and `pixel_art_AA`, at DS resolution, a line behind, through the same `filter_push`/`filter_pull` split as GLOW (`NeighbourFilter`, chained before GLOW by `FilterChain`). `fast-sharpen` takes GL_LINEAR's taps a third of a pixel out, as its pass at the DS's size does.

The NextUI patterns (`lcd3x`, `scanline`, `res_scanlines`, `res_scanlines_auto`, `retro_v2`) are pattern tables like rgds-core's. `res_scanlines` counts its lines from the bottom of the view, as the GLSL's `gl_FragCoord` does; `_auto` is the same at the view's own number of DS rows. `retro_v2` and `zfast_lcd` change the colours as well as the pattern, so the colour table is built in the pattern's tone (`pattern_tone`).

CURVE is `barrel-distortion` at the panel's resolution. Under a curve each view is scaled into its side buffer, and `Display::finish_views` remaps it into the frame through a per-pixel map (`build_curve_map`, nearest, black outside), then draws the PATTERN over the curved rows, which NextUI's old-tv preset does as its second pass, so the scanlines stay straight. The frontend then draws the menu and notices over it unbent. The map is 4 bytes a panel pixel, 3 MB at 1024x768. Rotation composes with it, being done at present from the logical frame.

SHADER PRESET (`video.preset`) is a row of its own, not a shorthand. Picking one sets the Shaders page's rows to its values (`preset_values`), and the host reads it back as the saved preset only while those rows still match it (`preset_matches`), else `custom`. Changing a row on the Shaders page stores `custom`. `custom` is never offered by the row itself.

The menu lists the Shaders page's rows from `kShaderSettings`, a settings table of 0006's, behind a link row after SHADER PRESET (`MinUiMenu::link_row`), so the Visual FX table stays upstream's apart from the one row.

On the display engine the pattern needs panel pixels the CPU never draws. The patterns that only darken, the same in every channel (`lcd`, `zfast_lcd`, `scanlines`, `soft_grid`), are drawn instead as the overlay layer's image, black at `1 - mul`, where upstream draws its LCD grid. The layer is redrawn only when the layout changes, so it costs nothing a frame. The rest are not offered there (`value_allowed`), and a view the composite shrinks or an overscale crops is left alone. `pattern_is_mask` is held to that rule by the test.

Precedence: a SCALING first, which turns BILINEAR, CHUNKY, the grid and seams off; else BILINEAR, then CHUNKY, then PATTERN, which forces the grid and seams off and disables their rows. SCALING and CURVE are off on the display engine, the GPU present and the SDL renderer, none of which hands the CPU the panel's pixels. zFast LCD's GBA gamma hack, on by default in libretro, is folded into the colour table whenever that pattern is drawn, so it applies before the border rather than after it; only the border pixels differ.

COLOUR, FILTER and GLOW apply live, with no display reopen; the filters are configured between frames. SCALING, PATTERN and CURVE are rebuilt when the menu closes, with no reopen either. The colour table is double-buffered: `stage()` builds the spare buffer on a thread of its own (on this machine a megabyte of entries takes 2 to 52 ms, more on an A53) and `commit()`, at the top of the frame loop, swaps it in once done. A change made before the last finished abandons it. Nothing is in flight between frames, because the core joins its workers at line 0 before `run_frame` returns. The pattern's tables are rebuilt in `build_scale`, on the path a layout shortcut takes. The table builders use `ds_pow_compat` (`pow_compat.c`), never `std::pow`, which would raise the glibc floor the ABI gate holds.

`config/platform.sh`'s `PROFILE_SLOW_SHADERS` is the list of `colour:<name>`, `scaling:<name>`, `pattern:<name>` and so on measured too slow on a platform, exported by `launch.sh` as `MINUI_SLOW_SHADERS`. The pause menu notes "SLOW ON THIS DEVICE" beside a listed value and still offers it. Every list is empty until measured. To measure, put an empty `shader-timing.txt` beside `device.txt`. `launch.sh` exports `MINUI_SHADER_TIMING=1`, the hooks time themselves, and DSperate logs `shader timing: colour <name> <us>/frame, scaling <name> <us>/frame, filter <name> <us>/frame, pattern <name> <us>/frame, glow <name> <us>/frame, curve <name> <us>/frame` every five seconds. The pattern figure covers `modulate_row` only; the row copies it replaces were upstream's already.

What the heavier ones cost on this machine's arm64 container, one screen, at 640x480 and 1024x768: the two-tap scalings 0.4 to 1.2 ms, `smooth_3x` 2 to 4.3 ms, `bicubic` 2.8 to 7.2 ms, `scale3x` 1.4 to 5.5 ms, `aa_4` 11 to 14 ms and `waterpaint` 9.5 to 32 ms; each FILTER about 3 ms for both screens; the curve 0.14 to 0.38 ms. An A53 is several times slower, so at least `aa_4` and `waterpaint`, and likely `bicubic` and `scale3x`, will not hold full speed with both screens shown. They are offered anyway, and the timing lines are what `PROFILE_SLOW_SHADERS` should be filled from.

## h700 pad numbering

NextUI h700-rc11 changed how its SDL presents the built-in Anbernic pad. Before rc11 the button numbers depended on the model, which is why `config/platform.sh` carries three per-model `DSP_MAP_H700_*` bodies. From rc11, every H700 model reports the TrimUI (tg5040) numbers, L2/R2 become axes 2 and 5, and the pad's GUID changes to `19000000010000000100000000016e01`.

DSperate picks the body by that GUID, as it starts (`MinUi::add_pad_mapping`, with the choice itself in `minui_pad.h`). `dsp_export_pad_mapping` only hands it the bodies, as `MINUI_PAD_MAP` and `MINUI_PAD_MAP_FIXED`, and the GUID to look for, as `MINUI_PAD_FIXED_GUID`. Reading the GUID in `launch.sh` meant running `dsp-pad-guid`, which started SDL a second time on every launch; it is now only there for `--probe`. Once SDL is up, the first joystick's GUID goes in front of the right body and the result goes to `SDL_GameControllerAddMapping`, before the frontend looks for a pad. On the fixed GUID, h700 uses `PROFILE_MAP_FIXED`, which is `DSP_MAP_TRIMUI` or `DSP_MAP_TRIMUI_NOSTICK`; on anything else it keeps the per-model body. The log names the GUID as `pad mapping: <guid> (<name>)`. This is the check NextUI's [Porting NextUI Paks to H700](https://github.com/pvaibhav/NextUI/wiki/Porting-NextUI-Paks-to-H700#built-in-controls-in-sdl) page recommends, and it follows the real state rather than a version number: `SDL_JOYSTICK_H700_FIXED_LAYOUT=0` brings back both the old numbers and the old GUID. `PROFILE_MAP_FIXED` is empty on every other platform, so a TrimUI pad can never take this path.

rc11's SDL also ships a built-in mapping for the new GUID, but the pak still adds its own. An added mapping replaces the built-in one for that GUID, and the built-in one does not match the TrimUI bodies the pak uses everywhere else.

The per-model bodies only matter for pre-rc11 builds and can go once those are no longer worth supporting.

## Known rough edges

- **The shaders are unmeasured on hardware.** The colour lookup, the pattern kernels, the glow, the resamplers, the filters and the curve are timed only on a desktop arm64 container, where the heaviest SCALING values already take most of a frame. The `shader timing:` lines on an h700 at 640x480 and a Brick at 1024x768 are what `PROFILE_SLOW_SHADERS` should be filled from. The display engine's mask is drawn through the same mapping as upstream's grid layer, but which tier the TrimUI devices open, and so whether they ever draw it, is unconfirmed (below).

- **rg28xx rotation is unverified.** Its panel is mounted portrait and NextUI exports `SDL_ROTATION=1` so applications see 640x480 landscape, but DSperate's fbdev tier writes `/dev/fb0` directly and never goes through SDL, so that rotation does not reach it. `config/platform.sh` sets `DS_ROTATE=270` for that device on that reasoning; it needs confirming on hardware.
- **The display tier on TrimUI is unverified.** The A133P is Allwinner, so `/dev/disp` may answer and the display-engine tier — written and measured for the Miyoo A30 — could open first. `--no-disp` is the escape hatch and the `video:` log line says which tier actually opened.
- **`:latest` toolchain images.** House convention, but a green build today can be red tomorrow with no repo change, and a toolchain glibc bump would trip the ABI gate. The gate firing is the desired behaviour; the per-platform `*_GLIBC_MAX` variables are the knob.
- **`pad.face_fix` is left at upstream's `auto`, unverified.** Since v3.0.5 DSperate compares the SDL mapping with the kernel's `BTN_SOUTH`/`BTN_EAST` positions and swaps A/B or X/Y when they disagree. The mappings `config/platform.sh` exports are deliberately positional, so if a TrimUI or h700 kernel reports Nintendo-labelled codes this could undo them. Setting `face_fix = off` in the profiles is the fix if a device turns out to swap.
- **The volume-key node is unverified.** The MENU+VOL suppression assumes the volume keys arrive as `KEY_VOLUMEUP`/`KEY_VOLUMEDOWN` on an evdev node, which the `minui: volume keys on ...` log line confirms. If a device reports them some other way, MENU+VOL changes the brightness and also opens the pause menu.
- **The launcher fonts are unverified on hardware.** BPreplay and NextUI's font1 render correctly through stb_truetype on a desktop build. The `pause menu:` log line names the font in use; an empty one falls back to DSperate's 5x7 font.
- **Spare buttons are mapped only where they have been probed.** The Brick's F1 and F2 report as SDL buttons 9 and 10, the numbers L3 and R3 have on the other TrimUI pads, so the Brick's mapping (`DSP_MAP_TRIMUI_BRICK`) names them `leftstick` and `rightstick` and `PROFILE_LABELS` has the pause menu call them F1 and F2, exported as `MINUI_BUTTON_LABELS`. The Smart Pro family's extra buttons are in no pad mapping yet. `dsp-pad-guid --probe <seconds>` logs every SDL joystick button, hat and axis and every evdev key pressed, with the node it came from; launch.sh runs it for 15 seconds when `probe.txt` sits beside `device.txt`, and removes the trigger. A button SDL reports goes into `config/platform.sh` as `misc1` or `paddle1-4`, or as an unused standard button, with a label when the device names it differently. One that only shows up as an evdev key on a node that is not the pad cannot be put in a GameController mapping at all.
- **Root-owned build output.** Everything under `src/`, `build/` and `dist/` is written as root by the containers, so `make clean` from an unprivileged account can fail. `make clean-docker` does the delete from inside a container.
