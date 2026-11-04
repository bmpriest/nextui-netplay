# spruce vs NextUI: feature parity

Where the spruce build (App/Netplay, through spruce's RetroArch) stands against
the NextUI pak, feature by feature from [features.md](../features.md). Measured
on a spruce Brick and RG35XXH, 2026-11-03.

## Same as NextUI

| Feature | spruce | Notes |
| --- | --- | --- |
| Shared screen: NES, SNES, Genesis/MD family, PlayStation | Works | sor2, rcr, contra3 pass; ctr not run (no CTR on the cards) |
| GB/GBC link (Gambatte) | Works | Netplay's gambatte replaces spruce's for armed sessions |
| GBA link cable (gpSP) | Works, intermittent at one point | aw2 passes; its battle start can stall on a jittery link |
| Instanced Gambatte | Works | tetris passes; settable from the app (GB row) |
| Instanced mGBA | Works | mkart passes at 52.9 fps; see the gaps for how it is enabled |
| Compatibility cores | Works | pinned to spruce's own core commits; byte-identical where it matters |
| House WiFi hosting and joining, discovery | Works | |
| Ad hoc hosting | Works, both devices | the Brick with its stock hostapd/udhcpd/iw on wlan1; the RG35XXH through an AP-mode supplicant on wlan1 with the udhcpd Netplay ships. Both keep their house connection |
| Ad hoc joining | Works, both directions | reworked for spruce, see below. sor2 over ad hoc: 60.1 fps, 0.1-0.2% stalls |
| Getting the guest's WiFi back | Works | 3-8 s after the session ends; spruce's saved networks untouched; the watchdog clears its record as on NextUI |
| WiFi power save | Works | the H700 reload now goes through spruce's wifi.sh |
| Menu pause ("Waiting for other player (menu open)") | Works | detected by the shim's network thread, not the frontend |
| Peer loss: Wait / Continue solo / Exit | Works | shim-side |
| Frontend state loads blocked | Works | spruce auto-loads states; the shim refuses the load (seen in 4 of 21 session logs) |
| Run checks, verbose logs | Works | spruce branch of the checks |
| Session outlives the app, broker | Works | |

## Different on spruce, by design

- **Ad hoc joining and restore** go through spruce's own supplicant: the ad hoc
  network is added at runtime and selected, then removed. Replacing the
  supplicant, as on NextUI, got the ad hoc network copied into the user's saved
  networks by spruce's WiFi bring-up (it adopts any foreign supplicant's
  networks), with priority 99.
- **Ad Hoc Network Switch** is off and hidden: spruce restarts its supplicant
  on every game exit, which would put a switched host back on the house
  network mid-session.
- **NextUI integration** (quick menu, Netplay icon, Game Switcher shortcut) is
  NextUI's own UI (Varnish, Game Switcher); spruce has neither.
- **PWR autosleep** is skipped: spruce owns sleep and the power button.

## spruce against NextUI

A NextUI Brick hosting a spruce RG35XXH over house WiFi (2026-11-04): the
handshake picks compatibility cores for 8 systems (both ends load the build
pinned to spruce's revision), and paired cores are agreed for Gambatte and
mGBA. sor2 passes (57-60 fps, 25-27 checkpoints agreed), and mkart passes on
paired mGBA (53.4 fps, 36 paired checkpoints), the NextUI side on MGBA.pak and
the spruce side on GBA with the game's core set to mgba.

## RetroArch during a session

launcher/spruce/retroarch-session.cfg is appended to spruce's RetroArch config
for armed launches only: save states (hotkeys, quick menu entries, auto save
and auto load), rewind, run-ahead, preemptive frames, fast and slow motion off,
and `config_save_on_exit = "false"` so none of it is written back to spruce's
config. When spruce passes its own `--appendconfig` (32-bit RetroArch on
BaseOS), ours joins its list.

## Gaps

1. **DS local wireless.** Not offered: DSP.pak and NDS.pak are NextUI paks.
   spruce ships DraStic (drastic32/64) and DSperate in Emu/NDS and launches them
   itself, not through RetroArch, so neither the RetroArch hook nor the MinArch
   shadow reaches them. Needs a hook for spruce's NDS launch, then the DraStic
   radio and/or Netplay's DSperate against spruce's binaries.
2. **Power save default.** spruce leaves WiFi power save on, and Bricks then
   lose their AP for minutes at a time (seen on a NextUI Brick too). Netplay's
   "Disable Wi-Fi Power Saving" stays off by default; turn it on for sessions on
   a Brick.
3. **Other spruce devices.** Only the Brick and RG35XXH are built and tested.
   np_spruce_platform maps BrickPro/SmartPro (tg5040), SmartProS (tg5050) and
   Flip (my355), but spruce-tg5050/my355 builds are not packaged; 32-bit
   RetroArch sessions are not routed yet.
4. **Paired mGBA toggle** (Cores > Base > GBA on spruce) is built but has only
   been exercised through `--ctl set instanced`, not the menu.
5. **Test harness:** the final `WAIT_FOR devbridge 30000` in the tests is tight
   for the RG35XXH's PyUI over an ad hoc tunnel.

Resolved 2026-11-04: the ad hoc network is `netplay-XXXX` on both firmwares
(was `nextui-XXXX`); RetroArch's auto save during sessions (above); paired mGBA
has an app toggle on spruce.
