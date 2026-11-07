# Upcoming

What is planned or open, in rough priority order. What exists today is in
[features.md](features.md). Items carried over from the old implementation plan
are marked *(backlog)*; check the code before starting one, as some have been
partly done along the way.

## Next

### Core edition and downloads

The current RC builds `NextUI-Netplay-Complete.zip` and
`Spruce-Netplay-Complete.zip`, with compatibility cores bundled for offline use.
See [release.md](release.md) for the actual layouts and supported targets.

A smaller Core edition remains planned: shared-screen support, ordinary
Gambatte/gpSP, paired cores, MGBA.pak/DSP.pak on NextUI, and the DraStic radio
without the base NDS.pak. It should refer NextUI users to the Pak Store for
NDS.pak and download compatibility cores from a version-pinned release.
Neither the in-app downloader nor `release-assets` manifest tooling exists yet.
Downloads need verified TLS, a pinned URL/size/hash manifest, atomic staging
and recovery from interrupted installs. These are follow-up work, not RC
Complete features.

### USB-C cable

A third way to connect, with about 1 ms round trips and no radio involved.
Plan: [analysis/usb-cable-plan.md](analysis/usb-cable-plan.md). Branch
`feat/cable`.

### Onboarding and RetroArch

First-run onboarding is implemented (`pak_onboarding.json`), including the
optional Wi-Fi power-saving choice and save-backup guidance. RetroArch protocol
selection and Spruce integration are implemented. Broader external-firmware
coverage remains future work; NETPACKET link modes use LinkPlay.

## Play features

### Open-game invitation: what is left

Invitations are implemented on both Spruce and NextUI ([features.md](features.md#game-invitations)).
Still open:

- **Matching by content, not file name.** The offer names the ROM by its path
  under `Roms/`, so a differently named copy of the same game reads as "not on
  this device". A persistent local ROM index keyed by system and content hash,
  consistent with the runtime identity check across raw ROMs, ZIPs, CUE/BIN and
  M3U, would let the receiver find its own copy.
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
- **Limited Extras coverage**: 32X, Sega CD, SG-1000, SMS, Game Gear and FDS.
  Supafaust now has scripted Spruce shared-screen coverage.
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
