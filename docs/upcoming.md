# Upcoming

What is planned or open, in rough priority order. What exists today is in
[features.md](features.md). Items carried over from the old implementation plan
are marked *(backlog)*; check the code before starting one, as some have been
partly done along the way.

## Next

### Release packaging: Netplay-Core and Netplay-Complete

Decided 2026-10-27. Today `make dist` builds `Netplay.pak.zip`,
`compatibility-cores.zip` and `Netplay-Full.pak.zip`, and every install carries
every platform's binaries (238 MB unzipped on an H700 card, 57 MB of it a
bundled NDS.pak). The new layout keeps one multi-platform pak, so one SD card
still works in every device:

- **Netplay-Core.pak**: shared-screen netplay, plain Gambatte and gpSP, Gambatte
  dual, mGBA dual and MGBA.pak, the DraStic radio overlay, and DSP.pak. It
  downloads the compatibility cores from the Netplay GitHub release when the
  user wants them, and sends the user to the Pak Store for NDS.pak itself.
- **Netplay-Complete.pak**: everything bundled, for offline installs and setting
  up cards on a PC.

Both install from the same code: a pack found in the pak is used, otherwise it
is downloaded. To make that safe:

- **CA bundle**: the Brick has no CA certificates, so the pak carries a small
  bundle (about 220 KB) for `curl`.
- **Pinned manifest**: every download is listed with its URL, size and SHA-256,
  pinned to the pak's own release tag, so an installed pak always fetches the
  builds it was tested with.
- **Per-platform files**: the device downloads the files for every platform on
  the card, so a card shared between a Brick and an H700 gets both.
- **Release tooling**: `make release-assets` builds each pack and the manifest
  for `gh release upload`.

The installed folder stays `Netplay.pak`: the name is hardcoded in the app and
the launchers' fallback paths, and keeping it lets the two editions upgrade
over each other.

The DraStic overlay also needs a stricter check of the NDS.pak it lands on:
its `pak.json` version and the hashes of the files the overlay replaces or
depends on, not only the `drastic` binary. NDS.pak 0.12.0 passes today's check
and then white-screens. Anything else gets a message naming the release needed.

### USB-C cable

A third way to connect, with about 1 ms round trips and no radio involved.
Plan: [analysis/usb-cable-plan.md](analysis/usb-cable-plan.md). Branch
`feat/cable`.

### Onboarding

A first-run flow for new players: what each mode is, how to host and join,
which systems play how, and the optional add-ons to download (compatibility
cores, NDS.pak from the Pak Store, DSP.pak, MGBA.pak). It is the download
flow's front end, so it follows the packaging work, and it describes the
finished product, so it comes last. Device-specific advice (which socket the
cable goes in, whether this device can host ad hoc) is detected at run time,
never assumed from the platform. Branch `feat/onboarding`.

### RetroArch compatibility

Playing against handhelds on other firmwares (spruce, Onion, Allium, muOS,
ROCKNIX, Knulli), which use RetroArch's netplay. spruce on an RG35XXH comes
first; a PC is a bonus. Plan:
[analysis/retroarch-plan.md](analysis/retroarch-plan.md). Branch
`feat/retroarch`.

## Play features

### Open-game invitation: what is left

Invitations work between spruce devices ([features.md](features.md#game-invitations-spruce)).
Still open:

- **Matching by content, not file name.** The offer names the ROM by its path
  under `Roms/`, so a differently named copy of the same game reads as "not on
  this device". A persistent local ROM index keyed by system and content hash,
  consistent with the runtime identity check across raw ROMs, ZIPs, CUE/BIN and
  M3U, would let the receiver find its own copy.
- **NextUI.** NextUI devices neither send nor receive yet. Sending is the same
  call from the MinArch shadow; receiving needs a way to pause and resume
  nextui.elf's menu, or the quick menu (Varnish).
- **Replies.** The sender only learns that the offer arrived, not whether it
  was accepted, declined or the game is missing.
- **Browse speed.** Opening spruce's menu on the game takes about 6 s, nearly
  all of it spruce's loop and PyUI starting again; PyUI has no command to jump
  to a game while it runs.

### Guest mode (simple client)

A joining device that stays in Netplay.pak while the host chooses the game,
then launches its own matching copy on an invitation. The setting exists,
hidden, and nothing reads it. Earlier designs had the host send the ROM and
core; that has copyright, storage, authentication and code-execution problems
and is not planned. If it is ever revisited, the save rule is settled: in
shared screen the host's save is sent; in link play the guest keeps its own,
because a Pokémon trade needs two trainers.

### Controller ports and more players

1. **Swappable controller ports**: a negotiated emulated port per device,
   independent of host/guest, changed at a frame barrier on both devices.
   Small, and the prerequisite for the rest.
2. **Three or more players** in shared screen: a host-side peer table, stable
   client IDs, the host relaying each frame's full input set, per-guest hashes
   and recovery, and identical multitap options everywhere. Until then extra
   guests are refused with `BUSY`. GB link stays two-player; GBA RFU could carry
   more once NetLink routes libretro client IDs.
3. **Drop-in seats**: a player leaves and rejoins (or is replaced) without
   ending the host's game, with an explicit policy for the empty seat.

### In-game status and controls

The shim's overlays only exist while the core runs; MinArch stops calling it
while its own menu is open. A panel beside that menu (session status, end
session, rejoin) needs a narrow MinArch hook. Do not read `/dev/input` behind
the frontend's back from inside a game.

### PlayStation memory card 2

Card 2 is disabled during sessions so the guest cannot persist anything. A
guest-owned local card 2 for in-game exchange is possible, once it is decided
whether it persists, whether it ever reaches the host, and how a resync treats
a game that cached card state.

## Robustness

- *(backlog)* Bind a session to its peer: authenticated pairing, admission tied
  to the gameplay sockets, no queued socket taking over after a disconnect.
- *(backlog)* Discovery records with last-seen expiry, and a bounded wait for
  a fresh supplicant scan instead of whatever was cached.
- *(backlog)* After the app restarts, decide from observable hostapd, radio and
  session state, never from cached UI flags; never regenerate an SSID or drop an
  AP that serves a live peer.
- *(backlog)* Failed arm checks return to the Host or Join flow they came from;
  recovery text says which side is sending and which receiving.
- *(backlog)* Checkpoint writes off the emulation thread, and garbage
  collection of old host checkpoints and guest volatile save directories.
- The state hash needs a volatile-field exclusion list before it can be trusted
  for RTC cartridges (Gambatte writes the host clock into state at power-on).
- `CORE MISMATCH` folds in `library_version`, so builds differing only in flags
  are reported. Reported, never enforced.
- *(backlog)* Fault injection on hardware: process crash on each side, AP loss,
  supplicant or DHCP failure, partial state transfer, dropped ACK/COMMIT, a
  silent or extra client, mismatched mode/delay/ROM/core, app relaunch with an
  AP live. The matrix covers the happy paths.

## Systems and cores

- **PlayStation across architectures**: same-architecture play works (with
  `drc_thread` off, which the shim forces); A30 to AArch64 is not established,
  and the A30's deployed core predates its own pin.
- **A30 SNES margins**: 2.1x realtime headless on an ordinary ROM; SuperFX and
  SA-1 games are untested.
- **Untested Extras**: 32X, Sega CD, SG-1000, SMS, Game Gear, FDS, SUPA.
- **DraStic frame phase**: DraStic keeps a random frame phase against its
  peer, the problem DSperate's VCOUNT patch fixed
  ([nintendo-ds/drastic.md](nintendo-ds/drastic.md)).
- **DS**: Pokémon (Union Room, trading) untested; DraStic on the A30
  (ARMv7) has no radio hook.

## Platforms

- tg5050 and my355 build but have never run on hardware.
- H700 is about a dozen devices; only the RG35XXH has been tested.

## Upstreaming

[upstream.md](upstream.md) has every fix that belongs in another project, with
the evidence and a proposed patch for each: the fceumm and picodrive state
round trips, RetroArch's RESUME bug, spruce's WiFi import and in-game menu.
Each of these would shrink what Netplay has to ship:

1. `HAVE_NETWORK=1` for Gambatte in NextUI's cores makefile: one line, and no
   separate Gambatte link core.
2. Gambatte's serial receive timeout.
3. gpSP's two RFU fixes (the original author's work, still unmerged).
4. `SET_NETPACKET_INTERFACE` in MinArch: about ten lines, and any netpacket core
   could link without the shim.
5. DSperate's emulator fixes (VCOUNT writes, late transport `begin()`, LAN join
   retry) to DSperate, and the launcher and pad changes to josegonzalez's pak.
   DSP.pak could then be an overlay like DraStic's instead of a full fork.
6. Varnish's ICON and MENU commands, input grab and H700 port, from the
   bmpriest fork to Helaas's repository ([analysis/varnish.md](analysis/varnish.md)).
