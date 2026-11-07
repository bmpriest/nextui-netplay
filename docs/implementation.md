# Implementation

How Netplay.pak is built: the parts, how a session flows through them, and the
decisions that shape them. Each section is a summary that links to the document
holding the detail and the evidence. What the parts add up to for a player is in
[features.md](features.md).

## Design in one paragraph

Netplay patches nothing in NextUI. Its setup app arms a *session* (a small file
under `.userdata/shared/Netplay/`). While one is armed, NextUI's own emulator
paks are wrapped by bind mounts so that `minarch.elf` resolves to Netplay's
wrapper, which loads a **shim**: a libretro core that wraps the real core and
adds the network. Everything Netplay needs from the emulator goes through the
libretro ABI. A reboot removes temporary mounts; ending the session makes the wrappers
pass through. Turn Off Netplay removes the hooks explicitly. The DS emulators are the exception: they are not libretro cores, so their
launchers read the session themselves.

## Spruce integration

The Spruce front end is `app/spruce`, using the installed PyUI runtime and
theme. Its native `netplay.elf --ctl` backend is the same session control plane.
`launcher/spruce/firmware.sh` maps Spruce model names onto four AArch64 build
directories; the Flip build also serves the RK3566 RGB30, RGB20SX and Pocket 1.
The RK3566 native control backend uses the H700 SDK's standard SDL/GLES ABI
rather than the Flip SDK's vendor Mali hook; cores still use the Flip build.
At runtime its native loader prefers temporary aliases to Spruce's own SDL
libraries, preserving the KMSDRM backend on dArkMoss.
The app metadata allowlist and launcher runtime selection must both cover a
model. An executable-presence check is not an architecture compatibility test.

Spruce bind-mounts wrappers over `ra64.universal`/`ra64.h700` and the supported
AArch64 DS executables, stashing the originals in `/tmp/netplay`. It preserves
Spruce's emulator configuration and passes through when unarmed. 32-bit
RetroArch/DS paths are not routed. State is in `Saves/netplay/`, with logs in
`Saves/netplay/logs/`. See [the device matrix](analysis/spruce-parity.md).

## The parts

| Part | Where | What it does |
|---|---|---|
| Setup app | `app/main.c`, `app/netsetup.c` → `netplay.elf` | Host, Join, Configuration; arms and ends sessions; installs extras. `--ctl` drives it from scripts |
| Broker | `app/broker.c` → `netplay-broker.elf` | Runs while a host session is armed: LAN announcements, the compatibility-core service, one-guest admission, a status file for the app, icon and quick menu |
| Quick menu | `app/nextui/quickmenu.c` → `netplay-quickmenu.elf` | A Netplay menu over NextUI's menus, drawn by Varnish |
| Shim | `shim/shim.c`, `shim/netlink.c` | The wrapping libretro core and its hybrid TCP/UDP transport |
| Launchers | `launcher/` | `minarch.elf` (the `PATH` shadow), `bind-mount.sh` / `wrap-pak.sh` (the mounts), `pre-launch.sh` (per-game session setup), `adhoc-join.sh`, `wifi-watchdog.sh`, `pak-manage.sh` (MGBA.pak, DSP.pak, the NDS overlay), `varnish-install.sh`, `status-icon.sh`, `quick-menu.sh` |
| Cores | `cores/` | Gambatte and gpSP link builds, the paired (dual) cores, MGBA.pak, compatibility cores, `udhcpd` |
| DS | `cores/dsperate/`, `cores/drastic-wifi/` | DSP.pak, and the DraStic radio overlay |
| Varnish | `dist/varnish/` (built from Helaas's Varnish.pak) | Draws the icon and quick menu over NextUI |
| Hints | `pak_hints.json` | All menu hint text, editable on the card |

## A session, end to end

1. **Arm.** Host or Join in the app (or the quick menu, or `--ctl`). The app
   brings up the network (house WiFi, or the host's ad hoc network and the
   guest's join to it), compares the two devices' core manifests, runs the
   arm-time checks, installs the bind mounts and writes `session`: role, peer,
   ports, input delay, chosen compatibility cores, boot identity. A host starts
   the broker.
2. **Launch.** The player starts a game from NextUI as usual. The wrapped pak's
   `launch.sh` finds `minarch.elf` on `PATH` first: Netplay's wrapper. With a
   session armed it runs `pre-launch.sh` (re-joins the ad hoc network, power
   save, logging) and starts the real MinArch with the shim in place of the
   core, the real core named beside it.
3. **Play.** The shim connects to the peer and picks its mode from the core it
   wraps: link for Gambatte and gpSP, paired for the dual cores, shared screen
   for the rest. DS paks skip steps 2-3 and read the session in their own
   launcher.
4. **End.** X in the app (or the quick menu) ends the session: the host tells
   the guest, which acknowledges; the guest's WiFi is put back. Installed
   wrappers become inactive passthroughs. Turn Off Netplay removes their mounts. A session from an earlier boot is cleaned up when anything next looks
   at it.

## Getting into the emulator

Emulator paks call `minarch.elf` unqualified, so a directory early in `PATH` is
the hook. Netplay stages a copy of each covered pak and bind-mounts it over the
original's own directory, which keeps `$0`, `EMU_TAG` and `CORES_PATH` correct.
Mounts vanish on reboot; a boot hook in `auto.sh` restores them while Netplay is
on. Launch stubs on the SD card remain as the fallback where mounting fails.
Detail, including the filename rule that keeps save paths stock:
[analysis/shim-architecture.md](analysis/shim-architecture.md).

## Inside the shim

The shim forwards all 25 libretro entry points and intercepts only what
multiplayer needs. With no session armed it is a pure passthrough.

**Shared screen** is input delay with lockstep, not rollback. Each side sends
its input for frame `F + delay`; a frame missing the peer's input stalls. The
app writes a transport floor (3 frames ad hoc, 10 through an access point) and
the shim raises it from the measured round trip; both adopt the higher. The host
sends its exact state before the first frame, both load it through the same
`unserialize`, and both hash state every 300 frames. A mismatch triggers an
authoritative resync at a frame barrier, acknowledged and committed. Since
protocol 15 a state crosses as a block delta against the guest's own state
(Crash Team Racing: 16 KB instead of 330 KB). Frontend save states are refused
while armed; guest save memory is volatile. Detail:
[analysis/state-sync.md](analysis/state-sync.md), and the core-by-core findings
in [analysis/determinism.md](analysis/determinism.md).

**Link** answers the core's `SET_NETPACKET_INTERFACE` request, which stock
MinArch refuses, and carries its packets over `netlink.c` on a receive thread
that keeps the link alive while the frontend blocks in a menu.

**Paired (instanced)** loads a dual core (`gambatte_dual_libretro.so`,
`mgba_dual_libretro.so`) holding both consoles and the cable, behind a versioned
dual ABI. Only inputs cross the network; startup exchanges each console's save
memory and establishes one checkpoint. Detail:
[gbs/multi-instance.md](gbs/multi-instance.md) and
[gbs/mgba-groundwork.md](gbs/mgba-groundwork.md).

**Failure handling** is the shim's own overlay: Wait / Continue solo / Exit on a
lost or silent peer, a waiting notice while the peer's menu is open (the peer
reports `CMD_PAUSE`), and a bounded, distinct error for every way a session can
fail to start.

## Networking

| Port | Protocol | Owner | Purpose |
|---|---|---|---|
| 55437 | TCP + UDP | shim (`netlink.c`) | TCP: states, link packets, control, input retransmit (protocol 18). UDP (host port 55437, client ephemeral): inputs and hashes with 3-entry redundancy |
| 55438 | UDP | broker / app | Host announcements |
| 55439 | TCP | broker | Core manifests and compatibility negotiation |
| 55440 | UDP | app / broker | Session end, acknowledged |
| 55441 | UDP | broker / guest agent | Peer game invitations and delivery acknowledgements |
| 7063 / 7064 | UDP / ENet | DSperate, DraStic radio | melonDS LAN protocol |

**Ad hoc**: the host runs `hostapd` and `udhcpd` (the system's, or the one the
pak ships) on a second interface when its radio allows an AP beside the station,
on the station's channel; the host is `10.0.0.1`. The guest's supplicant command
is captured before it moves, so every recovery path can put it back. Detail and
the failure catalogue: [analysis/adhoc.md](analysis/adhoc.md).

**Platform differences** in WiFi stacks (supplicant control sockets, DHCP
clients, the H700's 8821cs driver ignoring nl80211 power-save requests, band
steering) live in `launcher/wifi-platform.sh`, probed at run time rather than
keyed on platform name.

## Cores and compatibility

Netplay prefers the installed core. At arm time the devices compare manifests;
when their builds would compute differently and **Use compatibility cores** is
on, both select the same packaged build (`cores/compatibility/<arch>/`), and
the shim says so on screen for two seconds. If no common build exists the game
starts with a desync warning rather than refusing. The link cores ship because
NextUI builds Gambatte without `HAVE_NETWORK` and its gpSP lacks two RFU fixes.
Revisions per platform and release: [game-systems.md](game-systems.md).

## Where things live

All mutable state is outside the replaceable pak, at
`/mnt/SDCARD/.userdata/shared/Netplay/`: settings, the session, broker status,
WiFi recovery records, mount ownership, Game Switcher backups, extras backups
and the ROM scan cache. Files are written then renamed, so a power cut cannot
leave a half-written file that reads back as defaults.

With **Verbose debugging logs** on (off by default), each game launch mirrors its
whole output to `$USERDATA_PATH/logs/netplay-games/<game>/<session>/<time>-<role>-<pid>.log`,
because NextUI truncates its per-system log on the next launch and would erase
the crash a rejoin follows. A missing exit trailer means the process was killed.

## What would break it

| # | Assumption | If it changes | Fails |
|---|---|---|---|
| 1 | Emulator paks call `minarch.elf` unqualified | the wrapper never runs | silently, safely |
| 2 | `getEmuPath` prefers SD `Emus/<PLATFORM>/<TAG>.pak` (stub fallback) | stubs never found | silently, safely |
| 3 | `core.name` is the core file's basename up to the last `_`, feeding `config_dir`/`states_dir` | a staged shim yields a different name | **silently, relocating save states** |
| 4 | The libretro ABI's 25 entry points | a newly required symbol is missing | loudly, at core load |

Three of four fail safe: Netplay stops engaging and games launch stock. The
arm-time checks (Configuration > Debug > Run checks) test all four and refuse
to arm on a failure.

What it costs to maintain: no rebuild per NextUI release (the original pak
needed six patched binaries each time); rebuilds follow core sources and
toolchains. The [upcoming.md](upcoming.md#upstreaming) list would shrink that
further.

## Testing

- **Host-side**: `make test` runs the shim suites (`shim/test/run.sh` for
  shared screen including forced desyncs and rejoins, `link.sh`, `dual.sh`) and
  the launcher and app script tests under `testing/`.
- **Two devices**: DevBridge scripts under `testing/devbridge/`
  ([testing/test-harness.md](testing/test-harness.md)) and the unattended
  matrix ([testing/test-matrix.md](testing/test-matrix.md)).

## Document map

| Area | Documents |
|---|---|
| Product | [features.md](features.md), [upcoming.md](upcoming.md), [game-systems.md](game-systems.md) |
| Analysis | [shim-architecture](analysis/shim-architecture.md), [state-sync](analysis/state-sync.md), [determinism](analysis/determinism.md), [adhoc](analysis/adhoc.md), [varnish](analysis/varnish.md) |
| Plans | [usb-cable-plan](analysis/usb-cable-plan.md), [retroarch-plan](analysis/retroarch-plan.md), [retroarch-protocol](analysis/retroarch-protocol.md) |
| Game Boy family | [multi-instance](gbs/multi-instance.md), [mgba-groundwork](gbs/mgba-groundwork.md) |
| Nintendo DS | [dsperate](nintendo-ds/dsperate.md), [drastic](nintendo-ds/drastic.md), [ds-wifi-mp-protocol](nintendo-ds/ds-wifi-mp-protocol.md) |
| Testing | [test-harness](testing/test-harness.md), [test-matrix](testing/test-matrix.md) |
