# Netplay test matrix

`testing/devbridge/matrix.py` runs the two-device gameplay tests with nobody at
the devices: every cell sets itself up (power save, host and join through
`netplay.elf --ctl`, tunnels to a guest on an ad hoc network), plays, judges
both devices' logs, ends the session and requires the guest to get back to its
house network on its own. See `testing/devbridge/README.md` for running it.

    cd testing/devbridge
    cp rig.example.json rig.json      # once: devices, how to reach them
    ./matrix.py                       # 46 cells
    ./matrix.py --resume ../results/matrix/<run>    # carry on / rerun failures

## The plan (62 cells, now 46)

Since `a6aa9f4` (2026-10-27) every cell runs with WiFi power save off, the
setting that makes these links usable, so the 24 power-save-on DS cells below
are gone and the matrix is 46 cells. The run recorded here predates that.

- **Nintendo DS** (`mkds.test`, Mario Kart DS: group, lobby, VS setup, then
  about a minute of random driving on Figure-8 Circuit - it starts a race but
  does not finish one; the random input cannot complete three laps): Brick to
  Brick and Brick to RG35XXH with either device hosting; DSperate (DSP.pak) or
  DraStic (NDS.pak) on each side, all four pairings; over the house WiFi and
  over Netplay's ad hoc network; WiFi power save off and on. 48 cells.
- **Other systems**, Brick hosting the RG35XXH, over WiFi and ad hoc: NES
  (River City Ransom), SNES (Contra III), GB (Tetris, paired Gambatte), GBA
  link (Advance Wars 2, gpSP), GBA instanced (Mario Kart: Super Circuit,
  mGBA), PlayStation (Crash Team Racing); PlayStation also Brick to Brick.
  14 cells.

## Clean run, 2026-10-27 (full2)

One pass on one build (74d53d0 for the last 24 cells, which changed only the
H700 host's band steering), retrying a failed cell once: **54 of 62 cells
passed, 3 of them on the retry.** Floors: DS 45 fps on ad hoc, 30 on WiFi;
the other tests' own floors.

| | Ad hoc | House WiFi |
| --- | --- | --- |
| Other systems (7 + 7) | 7/7, 59-60 fps | 7/7, 58-60 fps |
| DS, power save off (12 + 12) | 11/12, 54-58 fps | 10/12, 26-44 fps |
| DS, power save on (12 + 12) | 11/12, 51-58 fps | 8/12, 28-51 fps |

Every failure, by cause:

- **DS over the house WiFi sits near 30 fps** on Brick to Brick pairs and
  DraStic guests (25.9-29.1 fps guests in the three speed failures). The same
  pairs race at 55-58 fps on ad hoc. With the H700 hosting, WiFi cells reach
  38-51 fps: its station is on the AP's 5 GHz band.
- **Power save on a Brick guest** misses the host's MP commands while dozing
  (116 timeouts in 10 s on .181, against none on the host), and Mario Kart
  leaves the lobby ("An error has occurred") or drops a menu press.
- **DSperate on an H700 hosting DraStic** ended once in a Communication error at
  CUT OFF: the DraStic guest made ~3 MP exchanges per frame, the frame-phase
  problem patch 0107 fixed in DSperate (VCOUNT writes) but which DraStic, a
  closed emulator behind our radio hook, still shows.
- **The house AP stops answering a Brick's authentication** for minutes at a
  time (both Bricks; .181 all afternoon, .143 after ~100 sessions overnight).
  The H700 never showed it. One cell could not join for it, and it stranded
  .143 at the end of the run.

## What the matrix found and fixed along the way

Netplay:
- An H700 hosting ad hoc from a dual-band house network put its AP on 5 GHz
  with its station, invisible to Bricks; it now holds the station to 2.4 GHz
  for the session.
- With Ad Hoc Network Switch off - the host keeps its house WiFi, so its
  single radio has to share that channel - an AP started while the station was
  between associations was surveyed onto another channel and pinned the radio
  there; it now follows the house network's channel (waited for, scanned, or
  remembered). With the switch on the host still surveys and takes the
  quietest channel; the matrix runs with it off and does not cover that mode.
- Session end: a guest agent race on its port; notices lost on the way to a
  guest in power save. The host now repeats until acknowledged and answers
  guests' polls.
- DraStic's radio waited 25 ms per MP exchange on ad hoc (DSP.pak: 50):
  DraStic pairs went from 44 to 56 fps.
- The core check's first connection retries; the PlayStation state sync gets
  30 s; an H700 uses whichever DHCP client its firmware has.

Test rig (DevBridge and the tests): VERIFY_WAIT polls; VERIFY_UNTIL presses
until a screen shows; stacked virtual-pad binds (DraStic menus scrolling by
themselves) are cleared; quit.sh replaces menu-only quits.

## Recommendations

1. Turn radio power save off for DS sessions by default (or for all guests of
   an H700 host): it costs DS more than any other system. The matrix itself
   now runs with it off.
2. Steer DS players to ad hoc; treat the house WiFi as marginal for it.
3. Look at DraStic's MP frame phase behind the radio hook (the VCOUNT
   equivalent) before calling DSperate/DraStic pairings reliable.
4. Reboot the house router if the Bricks start failing to authenticate.
