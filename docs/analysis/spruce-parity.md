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

## Gaps

1. **DS local wireless.** Not offered: DSP.pak and NDS.pak are NextUI paks.
   spruce ships DraStic (drastic32/64) and DSperate in Emu/NDS and launches them
   itself, not through RetroArch, so neither the RetroArch hook nor the MinArch
   shadow reaches them. Needs a hook for spruce's NDS launch, then the DraStic
   radio and/or Netplay's DSperate against spruce's binaries. The largest gap.
2. **Instanced mGBA from the app.** The toggle lives on the MGBA row, which is
   NextUI's MGBA.pak and hidden on spruce. Only `--ctl set instanced` turns it
   on. spruce's GBA also defaults to gpSP, so the player must pick mgba as the
   game's core in spruce too. Wants a GBA-row option on spruce, and a hint that
   the core choice decides link cable (gpSP) vs instanced (mGBA).
3. **Frontend auto-save during a session.** NextUI turns save states, auto-save
   and auto-resume off for a session. spruce's RetroArch config has
   `savestate_auto_save = true`, so quitting a netplay game writes an auto state
   and the next solo launch resumes from the session's end. Loads are already
   blocked; the save is not. Fix: pass RetroArch an `--appendconfig` for armed
   launches with auto save/load, run-ahead, preemptive frames, rewind and
   fast-forward off (the last four are off by default on spruce, but a user who
   turns them on would desync).
4. **Power save default.** spruce leaves WiFi power save on, and the Brick then
   loses its AP for minutes at a time. Netplay's "Disable Wi-Fi Power Saving"
   defaults to off. On spruce it should default to on.
5. **Ad hoc network name.** `nextui-XXXX` on spruce too. Kept for
   interoperability (the join scan, the watchdog and adhoc-join.sh all key on
   the prefix, and a NextUI guest must find a spruce host); a neutral prefix
   would need both sides to accept either.
6. **Other spruce devices.** Only the Brick and RG35XXH are built and tested.
   np_spruce_platform maps BrickPro/SmartPro (tg5040), SmartProS (tg5050) and
   Flip (my355), but spruce-tg5050/my355 builds are not packaged.
7. **Test harness:** the final `WAIT_FOR devbridge 30000` in the tests is tight
   for the RG35XXH's PyUI over an ad hoc tunnel (sor2 played clean and failed
   only there).
