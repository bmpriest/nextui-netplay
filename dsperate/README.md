# minui-nintendo-dsperate-pak

A MinUI Emu Pak for Nintendo DS, wrapping the standalone [DSperate](https://github.com/beebono/DSperate) emulator (version 3.0.5).

DSperate reimplements DraStic's JIT and NEON rendering techniques with an ARM to AArch64 recompiler, a threaded software rasteriser, save states, Action Replay cheats and RetroAchievements. Unlike DraStic it needs no BIOS dump to play games.

> [!NOTE]
> This pak installs alongside [minui-nintendo-ds-pak](https://github.com/josegonzalez/minui-nintendo-ds-pak), which wraps DraStic. They use separate rom folders, so both can be installed at once.

## Requirements

This pak is designed and tested on the following MinUI Platforms and devices:

- `tg5040`: Trimui Brick (formerly `tg3040`), Trimui Brick Pro and Trimui Smart Pro
- `tg5050`: Trimui Smart Pro S
- `h700`: Anbernic RG28XX, RG34XX, RG34XX SP, RG35XX Plus, RG35XX 2024, RG35XX H, RG35XX Pro, RG35XX SP, RG40XX H, RG40XX V, RG Cube XX and RG SP

Use the correct platform for your device. The `h700` and `tg5050` platforms are provided by NextUI, so those devices need NextUI rather than stock MinUI.

On `h700`, NextUI rc11 changed the button numbers SDL apps see. The pak detects which numbering the running NextUI uses, so both rc11 and later and earlier releases get the right buttons.

## Installation

1. Mount your MinUI SD card.
2. Download the latest release from Github. It will be named `DSP.pak.zip`.
3. Copy the zip file to `/Emus/$PLATFORM/DSP.pak.zip`.
4. Extract the zip in place, then delete the zip file.
5. Confirm that there is a `/Emus/$PLATFORM/DSP.pak/launch.sh` file on your SD card.
6. Create a folder at `/Roms/Nintendo DS (DSP)` and place your roms in this directory.
7. Unmount your SD Card and insert it into your MinUI device.

Both `.nds` and `.zip` roms work. DSperate reads zips itself, detected by content rather than extension, so there is no need to extract them. `.7z` is not supported.

## BIOS

No BIOS is required. Without dumps, DSperate boots games with a built-in replacement BIOS (FreeBIOS) and a generated firmware.

Supplying your own dumps unlocks the DS firmware menu and cycle-exact SWI timing. If you have them, place them at:

- `/Bios/DSP/bios9.bin`
- `/Bios/DSP/bios7.bin`
- `/Bios/DSP/firmware.bin`

None are included here, and none can be.

## Device Configuration

DSperate is configured through an INI file. On the first launch the pak picks the profile matching your device, writes it to `/.userdata/$PLATFORM/DSP-dsperate/dsperate/dsperate.ini`, and records the choice in `device.txt` beside it. After that the file is yours: anything you change there, or in DSperate's own pause menu, is kept on later launches.

The profiles differ only in how the stylus is driven, which depends on how many analog sticks the device has:

| Profile | Stylus | Devices |
| --- | --- | --- |
| `two-sticks` | Right stick moves the pen | Smart Pro, Smart Pro S, Brick Pro, RG35XX H, RG35XX Pro, RG40XX H, RG Cube XX, RG34XX SP |
| `one-stick` | Left stick moves the pen | RG40XX V |
| `no-sticks` | Hold R2 and use the d-pad | Brick, RG28XX, RG34XX, RG35XX Plus, RG35XX 2024, RG35XX SP, RG SP |

Delete `device.txt` to have the profile applied again. Your previous config is kept as `dsperate.ini.bak`.

`configs/reference.ini` in the pak is upstream's fully commented defaults, listing every key and what it does. It is documentation only and is never applied.

## Key Controls

MENU acts as the modifier for every chord. Pressed and released on its own, it opens the pause menu.

- MENU: Pause menu, and MENU or B again to close it
- MENU + L2: Swap which screen is the large one
- MENU + START: Move the small screen of the pip layout to the next corner
- MENU + SELECT: Save and open NextUI's game switcher, which shows the game's picture with A to resume it and B to go back to the game list. Stock MinUI has no game switcher, so there it saves and quits.
- R2 (two-stick and one-stick devices): Hold for fast forward
- L2: Tap the stylus at the pen's position
- R2 (no-stick devices): Hold to move the pen with the d-pad

Swapping the screens or moving the small screen shows the new setting's name at the top of the screen for a few seconds. The choice is remembered for that game.

The pause menu's Options > Shortcuts page changes any of these the way NextUI's does: press A on a row, then press a button. Holding MENU while pressing it makes a MENU + button chord. X clears the row, and Y twice puts back the defaults for your device. The page can also move every chord from MENU to another button. These are there too, and are off by default:

- Pause menu as a chord
- Quit
- Next and previous screen layout, which also show the layout's name at the top of the screen
- FPS counter
- Fast forward toggle
- Screenshot
- Save state, load state and the state slot
- Hold to blow into the mic
- Close or open the lid
- Next and previous colour, next and previous pattern, and next and previous shader preset, which show the new one at the top of the screen and are remembered for that game (see [Visual FX and shaders](#visual-fx-and-shaders))

The Brick's F1 and F2 can also be bound on the Shortcuts page, or picked as the modifier instead of MENU.

Every chord is also in the `[padhotkeys]` section of `dsperate.ini`.

Configs from earlier versions of this pak gain MENU + START on the next launch, and MENU + R1 / L1, MENU + Left / Right and L3's mic blow are turned off unless your config already set them. Every other chord they had stays as it was.

## Pause menu

The pause menu looks and works like the one every other system on the card uses:

- Continue: Back to the game.
- Save: Saves a state to the slot shown. Left and right pick one of eight slots, and the window beside the list shows what each one holds.
- Load: Loads the state in the slot shown.
- Options: Every setting DSperate has, as lists: Emulation, Visual FX, Layout, Controls, Shortcuts and DS Options, then Cheats and Achievements when the game has them, Manage Save Slots to delete states, Save Changes to choose whether changes apply to every game or only this one, and Reset Game.
- Quit: Fades the screen to black and returns to the launcher. The quit shortcut fades out the same way.

On NextUI the menu follows the theme colours and font chosen in NextUI's settings. On MinUI it uses MinUI's own look.

The MENU tap is `pause.alt = mod` in the `[padhotkeys]` section of the config. Configs from earlier versions of this pak gain that line on the next launch. Set it to `none` to keep MENU for chords only.

## Visual FX and shaders

Options > Visual FX has a SHADER PRESET row, and a SHADERS (ADVANCED) page under it, for the screen shaders of [NextUI](https://github.com/LoveRetro/NextUI), of [rgds-core](https://github.com/kleinrein/rgds-core) and the libretro ones it ships, and a CRT look.

SHADER PRESET picks one of NextUI's presets, one of rgds-core's, or `crt`, and sets the rows on the SHADERS (ADVANCED) page to match. Changing a row there makes the preset `custom`.

The SHADERS (ADVANCED) page has:

- COLOUR: another screen's colours, or a mood. `nds`, `natural_vision`, `nds_natural_vision`, `ds_classic`, `ds_lite`, `natural`, `vivid`, `bold`, `gba`, `warm_paper`, `cool_crisp`, `pocket_green` and `amber`. It works on every device.
- SCALING: how the DS's pixels are resampled to the panel, in place of the plain nearest one. `sharp_bilinear`, `pixellate`, `sharp_shimmerless`, `shimmerless_grid`, `shimmerless_vrgb`, `edge1pixel`, `smooth_3x`, `bicubic`, `aa_4`, `waterpaint` and `scale3x`.
- FILTER: a pass over each DS pixel and its neighbours before the picture is scaled. `fast_sharpen` and `pixel_art_aa`. It works on every device.
- PATTERN: what each DS pixel looks like on the panel. `lcd`, `zfast_lcd`, `scanlines`, `soft_grid`, `ds_classic`, `ds_lite`, `gba`, `rgb_stripes`, `beads`, `tiles`, `crt`, `lcd3x`, `scanline`, `res_scanlines`, `res_scanlines_auto` and `retro_v2`. It needs a screen shown larger than the DS's own. rgds-core's fade in between one and two times the size, as they do there.
- GLOW: light spread from the picture onto what is around it. `glow` spreads all of it, `bloom` only the parts brighter than BLOOM THRESHOLD, and `halation` a warm, wider light, as a CRT's glass scatters it. `auto`, the default, is the CRT pattern's own halation when that pattern is drawn, and nothing otherwise. GLOW STRENGTH and GLOW RADIUS (in DS pixels, up to 8) set how much and how far; left on AUTO they are the glow's own. It works on every device.
- CURVE: `barrel` bends each screen like a CRT's glass. The PATTERN is drawn after it, so scanlines stay straight, as in NextUI.

SCALING and CURVE need the pak to scale the picture itself, so they are off where a device's display engine scales it in hardware. There, PATTERN offers only `lcd`, `zfast_lcd`, `scanlines` and `soft_grid`, the ones that only darken, drawn over the picture by the display engine. A SCALING takes the place of BILINEAR, CHUNKY, LCD GRID and SEAM. Without one, BILINEAR and CHUNKY take precedence over PATTERN, and a PATTERN takes the place of LCD GRID and SEAM. The page says why when a row is off.

COLOUR, FILTER and GLOW change the picture behind the menu as soon as they are picked, and SCALING, PATTERN and CURVE when the menu closes. Save Changes decides whether they apply to every game or only this one. Screenshots, save states and the game switcher's pictures keep the game's own colours.

`smooth_3x`, `bicubic`, `aa_4`, `waterpaint` and `scale3x`, and both FILTER values, read many pixels for each one they draw and cost the most time each frame. Not every device may keep full speed with them; `shader-timing.txt` (see [Debug Logging](#debug-logging)) shows what they cost.

They are `preset`, `colour`, `scaling`, `filter`, `pattern`, `glow`, `glow_strength`, `glow_radius`, `bloom_threshold` and `curve` in the `[video]` section of `dsperate.ini`. An unknown value is read as `off`, an unknown GLOW as `auto`, and an unknown preset as `custom`. The old `pattern = sharp_bilinear` is read as `scaling = sharp_bilinear`.

NextUI's presets are:

| Preset | SCALING | PATTERN | CURVE |
| --- | --- | --- | --- |
| Old TV | `off` | `res_scanlines` | `barrel` |
| Real GBA, Real Game Boy | `pixellate` | `lcd3x` | `off` |
| Scanlines | `off` | `res_scanlines_auto` | `off` |
| Pixelperfect sharp | `pixellate` | `off` | `off` |
| Pixelperfect smooth | `smooth_3x` | `off` | `off` |

rgds-core and libretro name their presets differently. In these rows they are:

| Preset | COLOUR | SCALING | PATTERN |
| --- | --- | --- | --- |
| DS Classic | `ds_classic` | `off` | `ds_classic` |
| DS Lite | `ds_lite` | `off` | `ds_lite` |
| GBA Color | `gba` | `off` | `off` |
| GBA screen | `gba` | `off` | `gba` |
| Pixel beads | `bold` | `off` | `beads` |
| Glossy tiles | `bold` | `off` | `tiles` |
| Vivid LCD | `vivid` | `off` | `rgb_stripes` |
| Soft scanlines | `off` | `off` | `scanlines` |
| Soft LCD grid | `off` | `off` | `soft_grid` |
| Warm paper, Cool crisp, Pocket green, Amber | `warm_paper`, `cool_crisp`, `pocket_green`, `amber` | `off` | `off` |
| Natural glow | `natural`, with GLOW `glow` | `off` | `off` |
| Natural smooth | `natural` | `off` | `off` |
| Vivid smooth | `vivid` | `off` | `off` |
| NDS Color | `nds` | `off` | `off` |
| Natural Vision | `natural_vision` | `off` | `off` |
| LCD1x, zFast LCD | `off` | `off` | `lcd`, `zfast_lcd` |
| Sharp bilinear | `off` | `sharp_bilinear` | `off` |
| LCD1x or zFast LCD + NDS Color, Natural Vision or both | `nds`, `natural_vision` or `nds_natural_vision` | `off` | `lcd` or `zfast_lcd` |
| Sharp bilinear + NDS Color, Natural Vision or both | `nds`, `natural_vision` or `nds_natural_vision` | `sharp_bilinear` | `off` |

Three of them are not exactly rgds-core's:

- Natural glow, Natural smooth and Vivid smooth keep their colours only. Their pixel-shape glow and smoothed diagonals read the neighbouring pixels in ways GLOW does not; the Natural glow preset uses GLOW `glow`, which comes closest.
- zFast LCD's gamma darkens a pixel before its border does rather than after, which only changes the border.

`crt` is this pak's own rather than rgds-core's: scanlines with a dark gap between DS rows, under a red, green and blue stripe for each DS pixel, brightened to make up for the light they take away. A brighter beam is wider, so bright rows all but fill the gap and dark ones keep it. With GLOW on `auto` it brings its own faint halation. CURVE `barrel` adds the curvature.

## Saves & States

- Game saves are stored in `/Saves/DSP/`.
- Save states are stored in `/.userdata/shared/DSP-dsperate/`, named after the rom.
- Screenshots are written to `/Screenshots/`, named the way NextUI names them, including the game's name from a `map.txt` beside the rom. Each one shows "Screenshot saved" unless NextUI's screenshot notification is turned off.
- Cheats are read from `/Cheats/DSP/usrcheat.dat` if present.

Saving or loading a state also leaves the launcher a thumbnail, so the game shows up in the game switcher and offers RESUME when you next open it.

Quitting through the launcher writes a state, and the next launch picks it back up.

> [!IMPORTANT]
> DSperate 3.0 changed the save state format, and it cannot load states written by earlier releases of this pak. Battery saves in `/Saves/DSP/` are unaffected. Make an in-game save before upgrading. Picking an old state with RESUME starts the game from its autosave instead.

## Deep Sleep & Shutdown

Deep sleep is handled by the emulator itself, on all three platforms.

Tap the power button to sleep: the screen and sound go off and the game is held exactly where it was. Tap again to wake. After two minutes asleep the device suspends to RAM to save the battery, and the power button still wakes it.

Hold the power button for a second to shut down. Unlike a plain kill, this writes the battery save and a resume state first, and the launcher picks that game back up the next time you turn the device on.

> [!NOTE]
> Earlier versions of this pak bundled [MinUI Power Control](https://github.com/ben16w/minui-power-control) for this. It is no longer used or shipped: it has no `h700` support, and it cannot save the game on shutdown.

## Debug Logging

Debug logs are written to the `/.userdata/$PLATFORM/logs/` folder, as `DSP.txt`.

These lines in that log are worth knowing:

- `pause menu: <look> look, font <path>` names the launcher look and the font the pause menu was given. `built-in` means no launcher font was found, and the menu draws with DSperate's own small font.
- `pad mapping: <guid> (<name>)` names the pad the mapping was added for, and `controller: <name>` confirms DSperate opened it. DSperate has no joystick fallback, so if the `controller:` line is missing there will be no input at all.
- `NextUI fixed h700 pad layout, using the TrimUI button numbers` appears on `h700` with NextUI rc11 or later. Without it, the pre-rc11 per-model button numbers are in use.
- `video: ...` names the display path that opened. Expect `fbdev scanout` on `h700`. Anything mentioning the SDL renderer means it fell back and will be slow.
- The recompiler banner. An interpreter note instead means the JIT is off, and the emulator will be far too slow.
- `resume state <path> not found, falling back to autoload` means RESUME in the game switcher asked for a save state slot that no longer exists, so the game started from its last autosave instead.
- `resume state <path> predates this DSperate (format N, needs M), falling back to autoload` means the slot RESUME asked for was saved by an older DSperate that wrote a format this one cannot read, so the game started from its last autosave instead.
- `state: no state in slot` or `state: cannot load ...` means DSperate refused the state RESUME asked for. The pak exits back to the launcher rather than starting the game from a state it cannot read.
- `shader timing: colour <name> <us>/frame, scaling <name> <us>/frame, filter <name> <us>/frame, pattern <name> <us>/frame, glow <name> <us>/frame, curve <name> <us>/frame` lines, every five seconds, say what each shader row costs each frame, averaged. They are only written while an empty `shader-timing.txt` sits in `/.userdata/$PLATFORM/DSP-dsperate/`. Unlike `probe.txt` it stays there until you remove it.
- `timing: <seconds> <step>` lines mark the launch, the config being ready, the launcher's look being read, the pad mapping, the emulator starting, the game running, quitting, the state being saved, the emulator exiting and the card being synced. The seconds count from when the device booted, so the gap between two lines is how long that step took.

To find the numbers of buttons the pak does not use yet, create an empty `probe.txt` in `/.userdata/$PLATFORM/DSP-dsperate/` and launch a game. For the first 15 seconds, before the game appears, press each button once. Every press is written to the log as a `probe:` line, and `probe.txt` is removed so the next launch is a normal one.

## Development

- `make build` cross-compiles DSperate for all three platforms and runs the upstream unit tests. If Docker runs out of memory, `make build JOBS=4` limits how many compile and link jobs run at once.
- `make test` runs the [bats](https://github.com/bats-core/bats-core) suite in `tests/`.
- `make lint` runs shellcheck and shfmt.
- `make screenshots` draws every page of the pause menu, in the MinUI and NextUI looks and at every panel size the pak supports, and a test screen through every shader preset, into `build/screenshots/`. No device is needed.
- `make release` produces `dist/DSP.pak.zip`.

A scheduled workflow opens a pull request when DSperate publishes a new release. See "Bumping the upstream version" in TECHNICAL.md.

See [TECHNICAL.md](TECHNICAL.md) for the build pipeline, the ABI floors, the toolchain workarounds and how to bump the upstream version.

## Credits

- @beebono for DSperate
- kleinrein for rgds-core's screen shaders, and jdgleaver, Gigaherz, Greg Hogan, hunterk, Pokefan531, Themaister, ShadX, Hyllian and Sp00kyFox for the shaders it ships. The NextUI team for its shader presets, and Fes, zadpos, decavoid, Team XBMC, Stefanos A., guest(r), Andrea Mazzoleni, davej and the other authors of the shaders NextUI ships. See `THIRD_PARTY.md`.
- anyone else I'm missing

## License

This pak is MIT licensed. The bundled DSperate binaries are GPLv3 — see `LICENSE.DSperate` in the release, and `dsperate.build-info` for the exact upstream commit and build flags they were produced from. `THIRD_PARTY.md`, also in the release, lists the shaders the Visual FX rows are ported from and their licences.
