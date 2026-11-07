# Netplay for NextUI and Spruce

Two-player LAN multiplayer between handhelds: shared-screen games, GB/GBC and
GBA linking, and experimental Nintendo DS local wireless. Connect through the
same Wi-Fi network or a handheld-hosted `netplay-XXXX` network where supported.
Network quality matters; an ad hoc network is not necessarily faster than a
house access point. Internet matchmaking is not implemented.

## Install the Complete release

Back up your SD card before installing or updating.

- **NextUI:** extract `NextUI-Netplay-Complete.zip` into
  `Tools/<PLATFORM>/` on the SD card. The result is
  `Tools/<PLATFORM>/Netplay.pak/launch.sh`.
- **Spruce:** extract `Spruce-Netplay-Complete.zip` at the SD-card root.
  The result is `App/Netplay/launch.sh`. Use a Spruce version providing PyUI's
  device classes and runtime for your handheld.

The Complete archives include the compatibility cores for offline use. The
smaller Core edition and in-app GitHub downloads are planned, not implemented.
No ROMs are included.

NextUI builds cover `tg5040`, `tg5050`, `my282`, `my355`, and `h700`.
The Spruce RC targets TrimUI Brick/Brick Pro/Smart Pro/Smart Pro S, Miyoo Flip,
Anbernic XX devices on the supported H700 Spruce base, Powkiddy RGB30/RGB20SX,
and Miniloong Pocket 1. Spruce uses four AArch64 build directories: `tg5040`,
`tg5050`, `my355` (also the RK3566 Powkiddy and Miniloong targets), and `h700`.
These are release targets, not a claim that every model has been tested on
hardware. The Spruce hardware rig is a Brick and RG35XX H. Other nightly
Spruce devices are outside this RC's target list.

End any session and turn Netplay off before replacing the application folder.
Preserve external settings, saves, logs and backups. NextUI state lives in
`.userdata/shared/Netplay/`; Spruce state lives in `Saves/netplay/`.

## Play

1. Open Netplay, complete onboarding, and choose **Host** on one device.
   Choose the current Wi-Fi network or create an ad hoc network if offered.
2. Choose **Join** on the other device and select the host.
3. Exit the app and launch your games through the firmware's normal menu.
4. Use **X** in Netplay to end an active session. Wi-Fi is restored as needed.

A session continues while the app is closed. A host can accept a returning
peer without creating a new session. Sessions from a previous boot are cleaned
up when Netplay next checks them.

Starting a game offers an invitation to the peer when its firmware menu is
available. NextUI displays **A ACCEPT / B DECLINE** through Varnish, independently
of the optional START quick menu. Spruce offers **Play / Not now / Browse**.
Invitations identify a local ROM by its relative path; no ROM is transferred.
Invitations received during gameplay are ignored.

## Systems and modes

| Core | Systems | Multiplayer |
| --- | --- | --- |
| FCEUmm | FC/NES, FDS | Shared screen |
| Snes9x / Supafaust | SFC/SNES | Shared screen; compatibility fallback bridges differing builds |
| PicoDrive | MD/Genesis and supported Sega relatives | Shared screen |
| PCSX ReARMed | PlayStation | Shared screen; cross-architecture determinism remains limited |
| Gambatte | GB/GBC | Ordinary Game Link or optional Gambatte Dual |
| gpSP | GBA | Wireless Adapter, Advance Wars 1/2 and generation III Pokémon |
| mGBA Dual | GBA | Emulates both linked consoles locally; experimental |
| DraStic / DSperate | NDS | Local wireless; experimental, no Download Play coverage |

**Gambatte Dual and mGBA Dual default to off.** Both peers must enable the
mode and have the required core. Each device must have both linked ROMs for a
two-cartridge session. Without an agreed paired setup, the normal link path is
used. Ordinary gpSP does not emulate every GBA cable game.

On NextUI, choose Gambatte Dual under **Configuration > Cores > Base > GB/GBC**.
Instanced mGBA is under **Extras > MGBA** and installs the bundled MGBA.pak.
Use `Roms/Game Boy Advance (MGBA)` and `Saves/MGBA` for that pak.
DSP.pak installs from the Extras list, with games in `Roms/Nintendo DS (DSP)`.
The current Complete release also bundles the supported NDS.pak base and
DraStic radio overlay; games use `Roms/Nintendo DS (NDS)`. Managed replacements
have backup/removal controls. Unsupported DraStic binaries cannot use the hook.

Spruce has one combined **Cores** list and uses its installed emulator choices.
GB/GBC shows Gambatte and, when enabled, Gambatte Dual. GBA shows gpSP's
supported modes and, when enabled, mGBA Dual; choose mGBA for the game in
Spruce's own emulator settings. NDS combines DraStic and DSperate. One check
means ready; two indicate dual mode. Spruce does not install NextUI emulator
paks. Its DS hook covers AArch64 `drastic64` and `dsperate`, not DraStic-Steward
or other 32-bit DS executables. The RetroArch hook covers `ra64.*`; 32-bit
RetroArch launches remain outside this RC's Netplay routing.

For DS, start the same game and use its local wireless menu to create and join
a group. DraStic and DSperate use the same radio protocol. Performance and
compatibility depend on the game, emulator pairing, device and network; older
measurements are documented in [the DS notes](docs/nintendo-ds/dsperate.md).

## Configuration

- **Nickname:** the name shown to peers.
- **Cores:** readiness, actual core names and available dual-mode controls.
  NextUI retains Base/Extras; Spruce combines the list. Readonly Spruce rows
  have no A action.
- **Settings:** host protocol (LinkPlay or RetroArch), **Hybrid Network Traffic**
  (default **On**), compatibility cores, and Disable Wi-Fi Power Saving.
  Protocol and hybrid changes apply to the next session.
- **Debug:** force compatibility cores, verbose debugging logs (default **Off**),
  session save backups (default **On**, the last three snapshots), checks and
  Wi-Fi restoration. Existing saved preferences override defaults.
- **Turn Off Netplay:** end the session and remove emulator launch hooks.
  X ends the session while leaving hooks installed as ordinary passthroughs,
  ready for the next host/join. Neither action uninstalls cores.

Hybrid transport applies to LinkPlay: UDP carries inputs and hashes, with
redundancy and TCP retransmission; state transfers, serial link packets and
control remain reliable TCP. Turning it off keeps those inputs/hashes on TCP.
RetroArch protocol uses its own transport and does not support Netplay's
NETPACKET link modes.

NextUI additionally offers the Varnish quick menu and status icon, the
GameSwitcher shortcut, and Ad Hoc Network Switch where the host supports it.
Spruce hides these options and leaves Wi-Fi, sleep and emulator selection to
its own firmware. The Spruce footer uses contextual button glyphs, B first,
with **B EXIT** at the main menu and **X END SESSION** while active.

Disable Wi-Fi Power Saving defaults off. H700 changes can reload the driver
and interrupt Wi-Fi briefly; the setting asks first and resets after reboot.

## Saves

In shared screen the host's save is authoritative and the guest cannot persist
its copy. Linked consoles keep their own saves. Frontend save states,
auto-resume, rewind and speed-changing features are blocked during sessions.
A player opening the emulator menu pauses the peer with a waiting message.
Peer loss offers Wait / Continue solo / Exit; a waiting or dimmed screen is
not proof of successful multiplayer.

## Build and test

See [release instructions](docs/release.md), [features](docs/features.md),
[Spruce coverage](docs/analysis/spruce-parity.md), and
[the test rig](testing/spruce/README.md). Historical reports describe the code
and harness at their recording time; changed fixtures do not rewrite old runs.

## Credits

- [NextUI](https://github.com/LoveRetro/NextUI) and Spruce/PyUI
- [mohammadsyuhada](https://github.com/mohammadsyuhada/nextui-netplay), original implementation
- [DSperate](https://github.com/beebono/DSperate), and josegonzalez's DS emulator paks
- Cheevos button-glyph implementation by Andrii Balakhtar (MIT; bundled license)
- My son, who wanted to trade Pokémon with me
