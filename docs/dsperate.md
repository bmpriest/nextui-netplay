# Nintendo DS local wireless (DSperate)

Netplay.pak can install DSP.pak, a build of the
[DSperate](https://github.com/beebono/DSperate) DS emulator, and give it the
armed Netplay session. Each device then runs its own DS, and DSperate carries
the DS radio between them over the session's network, so games with DS local
wireless (Download Play aside) see each other as if they were in the same room.

This is not shared-screen netplay and not an instanced core: every player has
their own console, their own screens and their own save, exactly as on real
hardware.

## How it fits together

| Piece | Where | What it does |
|---|---|---|
| `dsperate/` | this repo | Builds DSP.pak: upstream DSperate pinned by commit, plus `patches/`, launcher, pad profiles |
| `make cores-dsp` | Makefile | Builds tg5040, tg5050 and h700, stages `dist/dsp/<platform>/DSP.pak` (one platform's binaries each) |
| `cores/dsp/<platform>/DSP.pak` | in Netplay.pak | The payload `pak-manage.sh` installs |
| `launcher/pak-manage.sh dsp` | in Netplay.pak | Install / status / ignore / restore, shared with MGBA.pak (`pak-manage.sh mgba`) |
| Settings > Nintendo DS (DSperate) | the app | Turns the install on (with backups) and off (with restore choices) |
| `Emus/<platform>/DSP.pak/launch.sh` | on the card | With a session armed: re-joins the session network, then starts DSperate as `--lan-host Netplay` or `--lan-join <host>` |

DSperate speaks melonDS's LAN protocol (UDP discovery on 7063, ENet on 7064).
The host is the session's host; the guest joins it by the address in the
session file, so no discovery broadcast has to cross the ad hoc network.

### What the launcher does in a session

Nothing below happens without `.userdata/shared/Netplay/session`.

1. Sources Netplay's `pre-launch.sh`: stale-session cleanup, power save if
   the user turned it off, and re-joining the host's ad hoc network.
2. Host: `--lan-host Netplay`. Guest: `--lan-join <peer>` (validated as an
   IPv4 address before it reaches the command line).
3. `--no-autoload` unless the player picked a slot: the auto state is where
   this console last quit, not where the other one is.
4. Pins the `performance` CPU governor for the session and restores the
   previous one on exit, recorded in `/tmp/dsp-cpu-restore` so a crash is
   undone by the next launch (see Performance).
5. With Netplay's verbose logs on: `DS_VERBOSE=1 DS_LAN_LOG=1`, and the
   log is archived to `logs/netplay-games/<game>/<session>/` like the
   minarch wrapper's.
6. The MP wait bound: 100 ms on a session without an ad hoc network (an
   access point), DSperate's 25 ms on ad hoc, or `dsp_lan_timeout_ms=<n>` from
   the session file if present (see the access-point measurements below).

## Patches on top of upstream

`dsperate/patches/` is a `git am` series against the pinned commit. The
Makefile fails if any patch stops applying.

| Patch | Why |
|---|---|
| 0001 GCC 8 NEON workaround | The NextUI toolchains are GCC 8.3, which miscompiles one NEON scaler (kept from minui-nintendo-dsperate-pak) |
| 0002 MinUI integration | Power button, game-switcher slot thumbnails/markers, `emu.state_key=rom`, minarch-style screenshots; rebased onto upstream main. In a network session the game-switcher hotkey writes no state |
| 0003 non-blocking `--lan-join` | A failed join was final, and Netplay starts the two devices independently. The guest now keeps trying from the frame loop (a "WAITING FOR HOST" toast) and goes live when the host's emulator answers |
| 0004 every controller | Only the first pad was opened, so DevBridge's virtual pad (and Bluetooth pads) were ignored. Face-button correction is now per pad |
| 0005 configurable MP wait | `net.lan_timeout_ms` / `DS_LAN_TIMEOUT_MS` (default 25 ms, as upstream) and wait statistics under `DS_VERBOSE` |
| 0006 late transport says `begin()` | The bug that made Mario Kart fail with "Communication error": see below |

### The communication-error bug (0006)

`MpTransport::begin()` tells the other consoles this console's radio is on.
The host needs it before it will wait for that console's MP replies. It was
only called on the radio's power-on edge. A guest that connected after its game
had powered the radio never announced itself. The host's connected mask then
lacked the guest, so `recv_replies` returned at once with nothing. Every MP
command failed, and the game gave up at race setup. The fix calls `begin()`
when a transport is attached to a radio that is already on.

That ordering is ordinary under Netplay, because the guest's emulator can start
before the host's. Upstream's own late-join path (the guest scan that starts
once the game is on the air) had the same gap.

## Measured

Brick (tg5040) hosting, RG35XXH (h700) joining, Mario Kart DS, using
`testing/devbridge/dsp-mkds.test`.

### Dual instance: rejected

The question was whether one device could emulate both DSes, as instanced
Gambatte and mGBA do, sending only inputs. Headless Mario Kart on the Brick
(attract-mode race, 3600 frames):

| | median ms/frame | frames over 16.7 ms | race windows over budget |
|---|---|---|---|
| one instance | 11.6 | 4.5 % | ~10 % |
| two instances at once | 15.2 | 44 % | ~80 % (≈ 37 fps) |
| two instances, `DS_HOST_CORES=2` each | 15.0 | 45 % | ~80 % |

One instance already uses about two of the Brick's four cores at 60 fps (the
emulation thread ~1 core, three 3D raster bands ~0.4 each). The H700 measured
the same (12.1 ms median). DSperate also has no input-lockstep netplay to build
on. So it is one DS per device over the LAN protocol.

### One instance per device

| Network | Governor | Race speed | MP waits per 600 frames |
|---|---|---|---|
| house access point | schedutil | ~1/3 (≈ 20 fps) | 12–16 s, 200+ timeouts at 25 ms |
| house access point, `dsp_lan_timeout_ms=100` | schedutil | ~1/3 | 12–16 s, ~15 timeouts; no communication error |
| Netplay ad hoc | schedutil | ~60 % | 4–9 s |
| Netplay ad hoc | performance (shipped) | **~50 fps (83 %)**, menus ~60 | ~4 s, single-digit timeouts |

The shipped configuration passes the full test (lobby, race, quit, both logs
checked) at a 57 fps session average on both devices.

- **Use ad hoc.** Through the access point, round trips here have a median of
  10 ms with a long tail (25 % over 25 ms, max ~115 ms). Mario Kart makes
  one to three MP exchanges per frame, each blocking the emulation thread for a
  round trip. Ad hoc averages 3 ms.
- **The governor matters as much as the network.** schedutil reads the MP
  waits as idle time and lowers the clock. That alone cost a third of the speed
  over ad hoc.
- **What is left is CPU.** About 13 ms of emulation per race frame on these
  A53s, plus about 7 ms of round trips that cannot overlap it.
- **Tried and rejected:** fixed frameskip during a session. Mario Kart's logic
  waits on the 3D swap, so it ran at half rate, and the session dropped.
  `idle_skip=all` made no measurable difference.

## Testing

`testing/devbridge/dsp-mkds.test`, run with `testing/devbridge/run.sh` (host
first). It runs from launch to the lobby at 2/8, holds the lobby 15 s, then VS,
characters, karts, settings and Figure-8 Circuit. After a minute of random
driving both consoles quit, and `device/check.sh dsp mariokart 50` checks each
archived log. Both radios must have been in the session (mask `0003`) and the
session average must reach 50 fps.

It needs a Mario Kart save on both devices (the first boot asks for a nickname)
and the ROM at `Roms/Nintendo DS (DSP)/mariokart.nds`. Over ad hoc the guest is
reached through the host; see `testing/devbridge/README.md`. DevBridge must
recognise `dsperate` as a game (DevBridge.pak `bin/protocol`, 2026-10-23).

`testing/test-dsp-manage.sh` covers install, status, backup and restore on a
fake card, and runs in `make test`.

## Not done / known gaps

- **tg5050** builds but has never run on hardware.
- Only Mario Kart DS has been played. Asphalt GT2's ROM is on both test
  devices but untested. Pokémon (Union Room, trading) is untested.
- The race runs at ~83 % on these A53 devices. Shipped DSperate leaves one more
  lever: a GPU 3D path where the device has Vulkan. None of these do.
- Through an access point the race runs at about a third of full speed. The
  launcher's 100 ms MP wait keeps the session alive there, but ad hoc is the
  supported way to play.
- Each device keeps its own firmware nickname ("DSperate" by default, set in
  DSperate's DS settings). Netplay does not set it.
- Two-player only, by Netplay's session model. DSperate itself allows up to 16.
