# Features

What Netplay.pak does today, as a player would describe it, and how far each
part has been proven. How to use it is in the [README](../README.md); how it
works is in [implementation.md](implementation.md); what is planned is in
[upcoming.md](upcoming.md); which systems and cores take part is in
[game-systems.md](game-systems.md).

Status words:

- **Stable**: shipped and exercised on hardware, including in the unattended
  [test matrix](testing/test-matrix.md).
- **Experimental**: shipped and working on hardware, with narrow game coverage
  or a known weakness.
- **Untested**: should work, never run on hardware.

## Ways to play

### Shared screen (Stable)

Both devices run the same game, and both see one console: inputs are exchanged
every frame and both emulators advance in lockstep. Systems: NES, SNES, Genesis
and its relatives, PlayStation. Two players.

- The two devices need the same ROM content (file names may differ) and a core
  that computes identically on both. When the installed cores differ, both load
  Netplay's matching compatibility core instead.
- The host's save is the game's save. The guest plays on a copy and never
  writes it.
- A desync is detected (state hashes every five seconds) and repaired by the
  host sending its exact state; only the blocks that differ cross the network.
- Either device can quit and relaunch the game; it rejoins the running session.
- Host reset resets both. A guest reset is refused.

Measured: 59-60 fps on NES, SNES and PlayStation, over ad hoc and house WiFi.

### Link cable (Stable)

Each device runs its own game, and the core's emulated link cable or wireless
adapter is carried over the network. Two players.

- **Game Boy / Game Boy Color** (Gambatte): Game Link. Linked games may differ:
  Red with Blue, Seasons with Ages.
- **Game Boy Advance** (gpSP): the Wireless Adapter, plus gpSP's special cases
  for Pokémon generation III and Advance Wars 1 and 2. Other GBA multiplayer
  games are not reached by gpSP; see instanced mGBA below.

Each device keeps its own save. Reset affects only the local console.

### Instanced link (Experimental)

Each device emulates *both* consoles and the cable between them, and only
controller inputs cross the network, so the cable never waits on WiFi. Turned
on per core in Configuration > Cores.

- **Gambatte**: Game Boy and Game Boy Color, any link game. A Pokémon trade
  between Silver and Crystal ran 13,500 frames without a desync.
- **mGBA**: every GBA link mode, including games gpSP cannot link, such as
  Mario Kart: Super Circuit. Installs Netplay's MGBA.pak. Too heavy for the A30.

Both devices need both games, because no ROM crosses the network. If either
device lacks one, the game restarts on the ordinary link path.

### Nintendo DS local wireless (Experimental)

Each device runs its own DS, and the DS radio crosses the network, so games
with DS local wireless see each other as if in the same room. Download Play is
not covered.

- **DSperate** (DSP.pak, which Netplay installs): see
  [nintendo-ds/dsperate.md](nintendo-ds/dsperate.md).
- **DraStic** (the user's NDS.pak, with Netplay's radio added): see
  [nintendo-ds/drastic.md](nintendo-ds/drastic.md).
- Any pairing works, DSperate and DraStic included, as they share melonDS's
  LAN protocol.

Mario Kart DS races at 54-58 fps over ad hoc and around 30 fps through a house
access point. Asphalt: Urban GT 2 also races. DS wants ad hoc.

## Connecting

### Two ways to connect

- **House WiFi**: both devices on the same network.
- **Ad hoc** (Stable on Brick-class hosts): the host creates its own
  `nextui-XXXX` network and the guest joins it. One hop instead of two, and
  4 ms round trips instead of 26-43 ms. Hosts that can run an access point
  alongside their station keep their house connection; **Ad Hoc Network
  Switch** instead leaves it, for a quieter channel. Devices without an
  AP-capable radio cannot host ad hoc (the option is hidden), but can join.

A USB-C cable is planned as a third ([upcoming.md](upcoming.md)).

### Finding a host

Join lists hosts heard on the house network and `nextui-` networks found by a
WiFi scan, merged into one list. A host that has already moved to its own
network is still found.

### Getting the guest's WiFi back (Stable)

A guest that joins an ad hoc network gets its usual WiFi back when the session
ends, when the host disappears (a watchdog notices within about a minute), when
the app next opens, or from Configuration > Debug > Restore Wi-Fi.

### WiFi power save (Stable)

**Disable Wi-Fi Power Saving** keeps the radio awake while a session is armed,
which matters most for DS. On H700 devices the driver must be reloaded to
change it, which drops WiFi briefly and lasts until reboot.

## Sessions

### A session outlives the app

Hosting or joining *arms* a session; games launched from NextUI's own menus
then play over Netplay until the session ends. The app can be closed: a small
background service keeps a host discoverable and reports who is connected. The
app's main screen shows the session while one is armed, and X ends it. A reboot
ends it.

### When the other player goes away

- A player in their emulator's menu pauses the other device, without a time
  limit, with "Waiting for other player (menu open)...". After 30 seconds the
  waiting player may also choose to stop waiting.
- A player whose device crashes, sleeps or loses the network leaves the other
  with **Wait / Continue solo / Exit game**. Continue solo keeps playing alone;
  the session stays armed for later games.

### Saves and save states

Frontend save states, auto-save and auto-resume are off during a session: a
save state from one device means nothing to the other. In-game saves work as
described per mode above.

## Cores

- **Compatibility cores** (Stable): when two devices' installed cores would
  compute differently, both load the same packaged build for that session, and
  the game shows "Starting with compatibility core..." for two seconds. When no
  common build exists the game starts anyway with "Core builds differ. Desyncs
  may occur...".
- **Installs Netplay manages**: MGBA.pak, DSP.pak and the DraStic radio, from
  Configuration > Cores > Extras. Each backs up what it replaces and can put it
  back.
- **Nothing on the card changes.** Netplay wraps NextUI's emulator paks with
  bind mounts that vanish on reboot; Configuration > Turn Off Netplay removes
  them for good.

## NextUI integration

These three need Varnish.pak to draw over NextUI, which Netplay offers to
install (see [analysis/varnish.md](analysis/varnish.md)). Not on the A30.

- **Quick menu** (Experimental): START, or buttons you choose, opens a small
  Netplay menu over NextUI's menus to host or end a session without opening
  the app.
- **Netplay icon** (Experimental): an N at the bottom of NextUI's screens, dim
  with no session, half lit while hosting, fully lit once connected.
- **Game Switcher shortcut** (Experimental): while armed, the Game Switcher
  holds only Netplay, so it is one press away; history comes back when the
  session ends.

## Diagnostics

- **Verbose debugging logs** (on by default): a complete log per game launch,
  kept per game and session under NextUI's `logs/netplay-games/`, so a crash
  survives NextUI truncating its own log on the next launch.
- **Run checks**: bindings, paths, cores and networking, with what failed.

## Platforms

| Platform | Devices | Status |
|---|---|---|
| tg5040 | TrimUI Brick, Brick Hammer, Smart Pro | Stable; the main test device |
| h700 | Anbernic RG XX family on BaseOS (about a dozen devices) | Stable on the RG35XXH, the one tested; other models differ in ports, radios and controls |
| my282 | Miyoo A30 | Shared screen and link; no ad hoc hosting, no instanced mGBA, no Varnish |
| tg5050 | TrimUI Smart Pro S | Untested |
| my355 | Miyoo Flip | Untested |

On spruce (App/Netplay, Brick and RG35XXH) everything above works except DS
local wireless and NextUI's own integration; see
[analysis/spruce-parity.md](analysis/spruce-parity.md).
