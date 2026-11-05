# Core options in a session

Two devices running the same core build can still compute different games:
each firmware starts the core with its own option values, and players change
them. A session therefore settles two kinds of option before the game starts,
the same way for NextUI to NextUI, spruce to spruce and NextUI to spruce.

## What a session settles

**Host-decided options.** Options that change emulated state run the host's
value on both devices. The list is `host_decided_options` in `shim/shim.c`:
every difference measured between NextUI's and spruce's defaults (below), plus
options that plainly change emulation (region, clocks, hardware model,
controller types, memory fill, sound options that fceumm keeps in its state).
Video, palettes, threading and frameskip stay each device's own. The guest
takes only keys on its own build's list, so a host cannot set anything else on
it. The list is a starting point; add to it as testing finds more.

**Built-in firmware.** Options that load a file one device may lack (a console
BIOS, a boot ROM, the Game Genie ROM) are forced to the built-in alternative on
both devices, whatever either frontend says (`builtin_firmware_options`):

| Core | Option | Session value |
| --- | --- | --- |
| gpSP | `gpsp_bios` | `builtin` |
| mGBA | `mgba_use_bios` | `OFF` |
| Gambatte | `gambatte_gb_bootloader` | `disabled` |
| PCSX ReARMed | `pcsx_rearmed_bios` | `HLE` |
| FCEUmm | `fceumm_game_genie` | `disabled` |

The paired mGBA core (`mgba_dual`) has no options; it never gives mGBA a BIOS
path, so it runs the built-in BIOS already.

A RetroArch peer is the exception to both: it cannot be told anything, so a
session with one runs that peer's shipped values instead
(`cores/retroarch-options/spruce`, see [retroarch-protocol.md](retroarch-protocol.md)).

## When: before the guest loads the game

The host publishes its values once its game is loaded, and netlink sends them
first thing on every connection (`CMD_SESSION_OPTIONS`, protocol 16). A guest
waits for them, up to 10 s, before it loads its own game, so its core starts
with them. Both devices then hold the values for the whole session: a change
in either frontend's menu reaches neither core until the session is over.

The timing is the point. A core takes changed options inside `retro_run`, and
a change once the game is running cannot be undone by any state. Measured with
River City Ransom, NextUI Brick hosting a spruce RG35XXH (fceumm sound quality
High on NextUI, Low on spruce), 2026-11-04:

| Guest took the host's options | Result |
| --- | --- |
| On one extra frame run before the host's state loaded | DESYNC at every check, through every resync |
| On its first frame after the host's state | One DESYNC, then 25 checks agreed |
| Same, with the guest refetching the host's state at once | in sync at 600, then DESYNC at every check |
| Never: host forced to spruce's values, nothing to change | PASS, 26/25 checks agreed |
| Before loading the game (shipped) | PASS, 25/24 checks agreed, 60 fps |

The divergence was in fceumm's output-filter accumulators (`FAC1`/`FAC2`)
only. At High quality the filter is fed from `WaveHi`, whose leftover samples
carry from frame to frame and are not in the state; once a change makes the
two sides' leftovers differ, every state the host sends lands on a different
filter history. The CPU and game state matched throughout. The upstream fix is
in [../upstream.md](../upstream.md).

A host whose game is not running yet when the guest loads sends its values on
connection later. The guest applies them then and refetches the host's state
at once (`request_options_resync`). That is the best this case allows, and the
rows above show it may not hold for every core.

## NextUI and spruce defaults

Effective values on a Brick (NextUI's pak `default.cfg` over the core's own
default) against spruce's shipped `.opt` files, for the core revisions both
run (the compatibility pins). Measured from source 2026-11-04.

Change emulation:

| Core | Option | NextUI | spruce |
| --- | --- | --- | --- |
| fceumm | `fceumm_sndquality` | High (FC.pak) | Low |
| fceumm | `fceumm_sndvolume` | 10 (FC.pak) | 7 |
| snes9x | `snes9x_block_invalid_vram_access` | enabled | disabled |
| pcsx | `pcsx_rearmed_psxclock` | auto | 57 |
| pcsx | `pcsx_rearmed_icache_emulation` | enabled | disabled |
| gpsp | `gpsp_bios` | auto | builtin |
| mgba | `mgba_use_bios` | ON | OFF |
| gambatte (spruce's GBC folder) | `gambatte_gb_bootloader` | disabled (GBC.pak) | enabled |

Matter only when the input is used:

| Core | Option | NextUI | spruce |
| --- | --- | --- | --- |
| fceumm | `fceumm_turbo_enable` | Player 1 (FC.pak) | None |
| fceumm | `fceumm_zapper_mode` | clightgun | lightgun in the repo; clightgun on the RG35XXH tested |
| pcsx | `pcsx_rearmed_analog_axis_modifier` | square | circle |

Differ but never change the game: Supafaust thread affinity, Gambatte palettes
and colourisation, mGBA's GB colours, `pcsx_rearmed_spu_interpolation` (audio
output; not confirmed absent from the state). picodrive, Supafaust and gpSP
(BIOS aside) match.

## RetroArch's input latency

Not a core option and not part of any state, but the setting that most
changes how a RetroArch peer behaves: RetroArch runs a frame on a guessed
remote input whenever it gets there before the real one, and rolls back when
the guess was wrong. Measured from captures between two spruce RetroArchs
(2026-11-02): at latency 0 (spruce's RG35XXH) 95-99% of frames were guesses,
about 50 rollbacks a minute in picodrive; at 1-3 (spruce's Brick), 2-6% and a
few a minute.

- **Netplay hosting a RetroArch client:** the client is told 3 frames, exactly
  (`SETTING_INPUT_LATENCY_FRAMES 3..3`, `RA_CLIENT_LATENCY`), and STALLed when
  it still reaches our input's edge.
- **A RetroArch host:** its own `netplay_input_latency_frames_min`/`_range`
  apply; a client cannot change them. Netplay logs what the host announces.
- **Netplay's own side** in either role uses the session's input delay, as
  between two Netplay devices: 3 frames ad hoc, 5 over WiFi, raised if the
  measured round trip needs more.

## Fast forward and rewind

Rewind needs nothing extra: a frontend cannot capture a state while
`retro_serialize_size` reports 0, which it does for every armed session. Fast
forward cannot make a session faster - each frame waits for the other device's
input - but a fast-forwarding frontend calls `retro_run` as fast as it can,
which in shared-screen play turns into a run of stalls and "Waiting for other
player", and in link-cable play runs one console ahead of its partner. So a
session caps the call rate at 110% of the core's frame rate
(`pace_session_frames`), and logs it once. On spruce the session's RetroArch
config also unbinds the fast-forward, slow-motion and rewind hotkeys.
