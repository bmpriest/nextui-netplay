# spruce tests

How the spruce results in the docs were measured: the protocol comparison
(pure RetroArch, RetroArch over Netplay, Linkplay), link games under Linkplay,
game invitations, NextUI against stock RetroArch, an input delay sweep, and
core comparisons. Everything runs from the PC against two spruce devices.
The single-protocol pass/fail tests are in [../devbridge](../devbridge/README.md),
and these scripts drive those same tests.

## The rig

| Device | Firmware | Reached by | Role |
|---|---|---|---|
| TrimUI Brick | spruce | USB ADB | hosts every session (Player 1) |
| Anbernic RG35XXH | spruce | SSH as `spruce`, through a tunnel | joins |
| TrimUI Brick | NextUI | USB ADB, and root SSH by key on its house address | serves the ad hoc network; a player only in the NextUI tests |

Both spruce devices join the NextUI Brick's ad hoc network as plain
stations, so neither protocol gets an advantage from hosting the network. The
PC stays on the house network and reaches the RG35XXH through the NextUI
Brick, which keeps its house connection while it serves the ad hoc network:
`ssh -L 127.0.0.2:2222:<RG35XXH ad hoc address>:22 root@<NextUI Brick>`.
`adhoc.sh` finds that address by the RG35XXH's MAC in the NextUI Brick's ARP
table. The control tunnel now reaches the AP's SSH daemon through USB ADB
(`127.0.0.1:40243` forwards to its port 22), avoiding the AP's unreliable
house-WiFi hop. Gameplay still uses the same ad hoc network.

Locally: DevBridge.pak checked out beside this repository (or `DEVBRIDGE=`
in `rig.conf`), adb, Python 3 and Pillow. On the devices: Netplay installed
(App/Netplay on spruce, Netplay.pak on NextUI) and DevBridge running, as for
the DevBridge tests. On both spruce cards: the ROMs the tests open, under
spruce's folders (`Roms/SFC/Street Fighter II (USA).zip` and so on).

Set it up once:

    cp testing/spruce/rig.example.conf testing/spruce/rig.conf    # serials and addresses
    export DEVBRIDGE_PASSWORD=...                                 # spruce's SSH password

The password is only ever read from the environment. `rig.conf` is ignored by
git.

## Ad hoc and sessions

    testing/spruce/adhoc.sh up          # AP up, both spruce devices on it, tunnel up
    testing/spruce/arm.sh linkplay      # Brick hosts, RG35XXH joins: Linkplay
    testing/spruce/arm.sh retroarch     # the same with RetroArch's protocol
    testing/spruce/arm.sh off           # no session, for pure RetroArch
    testing/spruce/adhoc.sh down        # sessions end, house WiFi back, AP down

`adhoc.sh up` also keeps the spruce Brick's radio awake (xradio's debugfs
`ps_disable`) until `adhoc.sh down` writes back what was there. Under power
save that radio loses the AP for minutes ("BSS lost"), and the runs then
measure the radio, not netplay. The RG35XXH is left alone: the driver must be
reloaded to change its power save.

On spruce, ending a session puts the house WiFi back, so `arm.sh` ends, arms,
then rejoins the ad hoc network on each device. The RG35XXH does that
detached and the tunnel follows it.

Paired (instanced) Game Boy consoles are a setting, read when arming: run
`sh /tmp/netplay-devbridge/ctl.sh set instanced 1` (or `0` for Gambatte's own
link cable) on both devices first. `testing/spruce/lib.sh` has `on b` and
`on h` for running commands like this one.

## The protocol comparison

Every run follows the same four steps, and each step leaves screenshots under
`testing/results/spruce/shots/` to check before the next:

1. **Start the game on both devices with the protocol under test.**
2. **Play**: `play.sh TEST TAG` plays a DevBridge test on games that are
   already running: its menus, 90 s of random input from both, and its
   after-play screenshot, without its launch, log checks or quit
   (`run.sh --play-only`).
3. **Collect**: `collect.sh TAG` stops the captures, pulls both captures and
   both Netplay game logs, quits both games, and prints the run's row.
4. **Table**: `table.py` builds the comparison table from any number of runs:
   `python3 testing/spruce/table.py testing/results/spruce/{ra,np,lp}-*`.

Runs live in `testing/results/spruce/TAG/`. `run.env` records the protocol,
game, addresses, latency and when play ended.

### Pure RetroArch

    testing/spruce/arm.sh off
    testing/spruce/ra-config.sh set                  # latency 1-3 on both, a check every 600 frames
    testing/spruce/pure-ra.sh ra-sf2 SFC "Street Fighter II (USA).zip"
    testing/spruce/play.sh sf2 ra-sf2
    testing/spruce/collect.sh ra-sf2
    ...
    testing/spruce/ra-config.sh restore

`pure-ra.sh` sets the session up through RetroArch's menus, as a player
would. The Brick hosts from Quick Menu > Netplay. The RG35XXH opens the
RetroArch menu from spruce's in-game menu, then connects by typing the
Brick's address on RetroArch's keyboard. Each stage ends on a screenshot, and
a single stage can be run again by name (`pure-ra.sh TAG SYS ROM type`). The
LAN list is the other way to join (`lan` and `join`), but on the RG35XXH it
failed with "No core found" for Supafaust.

Each device's own settings differ: on the rig, the Brick had latency 1-3
with a state check every 3600 frames, and the RG35XXH latency 0 with a check
every 600. So `ra-config.sh set` backs up every
`Saves/ra-configs/retroarch-*.cfg` and sets the same on both: latency 1-3, and
a check every 10 s so that desyncs are measured. `restore` puts the backups
back.

### RetroArch over Netplay, and Linkplay

    testing/spruce/arm.sh retroarch                  # or: arm.sh linkplay
    testing/spruce/launch.sh np-sf2 SFC "Street Fighter II (USA).zip"
    testing/spruce/play.sh sf2 np-sf2
    testing/spruce/collect.sh np-sf2

`launch.sh` captures the pair's traffic on both devices. It opens the game on
the Brick, then on the RG35XXH 3 s later, as the tests do. DevBridge's
`OPEN_GAME` needs DevBridge's app in front, so `launch.sh` and `pure-ra.sh`
open it from spruce's menu first, queued the way spruce opens any app. Under Linkplay a
whole DevBridge test can also run on its own, with its log checks for a
pass/fail result:

    DEVBRIDGE_PASSWORD=... testing/devbridge/run.sh --user spruce --port 2222 \
        adb:BRICK-SERIAL 127.0.0.2 testing/devbridge/tetris.test

### Games

| Test | System | Play | Notes |
|---|---|---|---|
| `rcr` | NES | 2P, 90 s brawling | |
| `sf2` | SNES | VS BATTLE, Honda vs Ken, 90 s fighting | the mode menu can time out to the title: look at `sf2-select` |
| `sor2` | Genesis | 2 PLAYERS, 90 s brawling | the title falls into its demo; an extra START may be needed |
| `ctr` | PlayStation | VS race on Crash Cove, 90 s | the large-state test (4.4 MB) |
| `tetris` | GB | 2PLAYER, 60 s | instanced or Gambatte link |
| `mariodx` | GBC | VS, 90 s | instanced passes; Gambatte's link cable does not reach VS on both |
| `aw2` | GBA (gpSP) | a turn each way | link games: no state checks, so the table's checks column is empty |
| `mkart` | GBA (mGBA) | VS race | paired mGBA |

Under pure RetroArch a game runs for a minute or so while the menus are set
up, so a title may be in its demo when play starts. The screenshots show
where each step landed. Steps that need to be done again can be done by hand
with `keys.sh` (`on b "sh /tmp/netplay-devbridge/keys.sh start:0.3:2"`).

### The table

The same measures for every protocol, over the same window (the 93 s ending
at the after-play screenshot):

- **Avg fps** and **Slowest 10 s**: frames advanced, from the RetroArch
  protocol's INPUT frame numbers or Linkplay's pacing lines.
- **Longest freeze**: the longest time the frame number stood still, or a
  Linkplay resync from DESYNC to commit.
- **Checks failed** of total: RetroArch CRC checks the client answered with a
  savestate request, or Linkplay DESYNCs of all hash checks (or paired
  console checkpoints).
- **State resends**: savestates sent by the host.
- **Frames on a guess** and **Rollbacks per min** (pure RetroArch only): from
  each device's view of the other's inputs, with latency at both ends of its
  range (`testing/retroarch/rollback.py`).

A run collected without an after-play screenshot (aw2) needs `end=EPOCH` set
in its `run.env` by hand.

Capture tools in `testing/retroarch/`: `radecode.py` (every command in a
RetroArch netplay capture), `rasum.py` (frame rate and command counts per
direction), `rollback.py`, `ra_checks.py` (each CRC check and whether it
matched, every savestate sent), and `raproxy.py` (a proxy that makes a
client offer an older protocol version). `pingstat.py` here turns a ping log
into the share of late inputs at each input delay.

## Game invitations

With a session armed (either protocol) and both devices in spruce's game menu:

    testing/spruce/invite.sh b      # Not now: the menu comes back as it was
    testing/spruce/invite.sh x      # Browse: the menu reopens on the game, several seconds later
    testing/spruce/invite.sh a      # Play: the RG35XXH starts the game and joins

The Brick opens Pokémon Blue (or `invite.sh a SYS ROM`) from DevBridge. The
RG35XXH must be in spruce's own menu, so `device/spruce-menu.sh` closes
DevBridge's app there first. The RG35XXH's prompt answers through DevBridge's
BTN. `keys.sh` will not press while a menu
is in front, and the prompt counts as one. The script screenshots the prompt
and the result, prints the RG35XXH's `invite.log` lines, and for `a` the
game log's connection line.

Timings, on the device (`adhoc.sh up` copies these to
`/tmp/netplay-devbridge/`):

    sh /tmp/netplay-devbridge/invite-timing.sh /mnt/SDCARD/Roms/GB/Imgs/<game>.qoi
    sh /tmp/netplay-devbridge/menu-timing.sh GB "/mnt/SDCARD/Roms/GB/<game>.zip"
    sh /tmp/netplay-devbridge/pause-timing.sh

- `invite-timing.sh`: how long the prompt takes to appear over the paused
  menu, with and without art (0.4-0.7 s on the RG35XXH, 1.4 s on the Brick).
- `menu-timing.sh`: what Browse costs. spruce restarts its menu to get there.
- `pause-timing.sh`: the PyUI alternative (`-msgDisplay`), which starts PyUI
  cold each time (about 4.6 s).

## NextUI against stock RetroArch

On the house network, with no ad hoc rig (`adhoc.sh down`): Netplay's
RetroArch protocol on the NextUI Brick against spruce's own RetroArch on the
RG35XXH. `device/ra-wrap.sh` adds `--host` or `--connect` to RetroArch's
command line there.

    testing/spruce/nextui-vs-ra.sh brickhost FC "Nintendo Entertainment System (FC)/River City Ransom (USA).zip" \
        "FC/River City Ransom (USA).zip" 60
    testing/spruce/nextui-vs-ra.sh sprucehost ...

## Input delay

A DevBridge test at several input delays, with the NextUI Brick hosting the
spruce RG35XXH over the house network (armed first). Each delay is pinned in
both session files:

    testing/spruce/delay-sweep.sh rcr 3 4 5 6

## Core comparisons

`testing/corecmp` runs two builds of a core side by side on one device and
compares their RAM, video and save states. `testing/corecmp/batch.sh`
compares NextUI's cores, or Netplay's compatibility cores, against spruce's
shipped ones on a spruce device. See its header for what it expects in
`/tmp/corecmp`.

## Afterwards

    testing/spruce/ra-config.sh restore    # if pure RetroArch ran
    testing/spruce/adhoc.sh down

`adhoc.sh down` ends both spruce sessions, which also puts their house WiFi
back. It also restores the Brick's power-save setting, ends the NextUI
Brick's network, and closes the tunnel.

## Broker suite

The deterministic spruce manifest covers FC, SFC with Snes9x and Supafaust,
MD, GB/GBC paired and cable modes, GBA with mGBA and gpSP, PS, and the four
DSperate/DraStic pairings. Of 26 planned cells, six RetroArch link-core cells
are declared unsupported: the adapter does not carry NETPACKET traffic.

With the existing rig on its ad hoc network and both devices in DevBridge:

```sh
DEVBRIDGE_PASSWORD=... python3 -P testing/devbridge/spruce-broker.py \
  testing/devbridge/spruce-complete-suite.json \
  --out testing/results/broker-autonomous-20261106 --resume
```

The adapter uses the sibling DevBridge PC broker. It preserves failed attempts,
skips passing cells on resume, archives the rendered gameplay script and its
screens/events, checks ROM hashes, restores core choices and settings, and
quarantines devices after a failed teardown. A stopped broker's leases must
only be reclaimed after verifying recovery on both devices. Final restoration
returns this rig to its starting ad hoc LinkPlay host/client session and
DevBridge foreground; it does not put an already-armed rig onto house WiFi.

The connection judge derives the gameplay interval from the recorded input
commands and checks both archived logs. Solo continuation fails, missing
coverage is an infrastructure failure, and normal disconnection after play
is outside the interval. Successful navigation checks retain their captured
frame without taking another screenshot. Reviewed references include their
source and hash in `testing/devbridge/refs/spruce-*-review-20261106.json`.
DS references still awaiting layout-specific review cannot produce a pass.

This run uses existing device logs without packet capture or gameplay polling.
The instrumentation overhead gate remains outstanding. Performance is
unvalidated and network RTT is `NO_DATA`; functional passes are not protocol
performance comparisons.

Rebuild the protocol/game measurement report from archived broker evidence,
without touching the handhelds:

```sh
python3 testing/spruce/broker-report.py testing/results/broker-autonomous-20261106
```

The broker also runs this reporter automatically. `report.md` and
`comparison.csv` contain average FPS, slowest 10 seconds, longest logged
freeze, checks failed, state resends, and the result for every planned cell.
Each attempted row links to `telemetry.json` with the exact gameplay window,
both devices' measurements, log coverage, and sources. FPS is estimated from
logged pacing bins; missing measurements remain `NO_DATA`. Counter deltas
exclude RetroArch's initial state transfer. These observed measurements do
not establish that the instrumentation overhead gate has passed.

For the TCP/hybrid experiment, `transport-rig.sh deploy` preserves the installed
shim and launcher under `/tmp/netplay-transport-original` and deploys the build
from `TRANSPORT_WORKTREE` (default: the sibling `.worktrees/netplay-net-traffic`).
`tcp` sets `NETLINK_TEST_NO_UDP=1`; `hybrid` clears it. The RetroArch hook must
be rebound after changing the launcher inode. `restore` reinstalls the saved
files and refreshes the hook. Run this helper only with both games stopped.

`transport-report.py RUN_ROOT` combines `adhoc-tcp`, `adhoc-hybrid`, `house-tcp`,
and `house-hybrid` broker directories into a side-by-side Markdown report and
CSV. It preserves all planned cells and shows transport negotiation evidence.
RetroArch's independent transport is an unchanged control. The input recipes
are shared, but random gameplay draws differ; this is a physical-network
comparison, not a claim of statistically established loss resilience.
