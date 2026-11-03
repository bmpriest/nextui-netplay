# Two-device gameplay tests

Scripted DevBridge runs that take two handhelds from launch to real
multiplayer play, one per netplay mode, and fail if any screen along the way or
the netplay logs afterwards say the devices did not play together.

| Test | Mode | What it plays |
| --- | --- | --- |
| `sor2.test` | shared screen (PicoDrive) | Streets of Rage 2: 2 PLAYERS, each device picks its own fighter, 90 s of brawling |
| `rcr.test` | shared screen (FCEUmm) | River City Ransom: 2P PLAY, 90 s of brawling from both |
| `contra3.test` | shared screen (Snes9x) | Contra III: 2 PLAYERS A, 90 s of running and shooting |
| `ctr.test` | shared screen (PCSX ReARMed) | Crash Team Racing: VS. single race, each device picks its racer, 90 s on Crash Cove; the 4.4MB state makes it the large-sync test |
| `tetris.test` | instanced GB link (paired Gambatte) | Tetris: 2PLAYER, Mario vs Luigi at different heights, 60 s of play |
| `mkart.test` | instanced GBA link (paired mGBA) | Mario Kart: Super Circuit: Multi-Pak VS, 100cc, Mario vs Yoshi, 2 min on Peach Circuit |
| `aw2.test` | networked GBA link cable (gpSP) | Advance Wars 2: Multi-Pak battle, a full turn each way (the guest builds a tank) |
| `dsp-mkds.test` | DS local wireless (DSP.pak on both) | Mario Kart DS: group, lobby, VS race on Figure-8 Circuit |
| `nds-mkds.test` | DS local wireless, DraStic hosting, DSperate joining | the same race between the two emulators |
| `nds-nds-mkds.test` | DS local wireless, DraStic on both | the same race, Brick and RG35XXH both on NDS.pak |
| `mkds.test` | DS local wireless, any pairing | the same race as a template: DSperate or DraStic on either side, filled in by `matrix.py` (the three above are the fixed-pairing versions it grew from) |

Gameplay is random input from both devices (`device/gameplay.sh`). After play,
`device/check.sh` reads each device's own netplay log and fails on divergence,
a missing link agreement, dropped link packets or a session average under the
test's minimum frame rate. It also prints the stall share, the longest stall
and the slowest 10 s window: lockstep never drops input, so a poor link shows
up as frames held waiting for the peer.

## Before running

- Both devices have Netplay armed together: the host hosting (over WiFi or an
  ad hoc network) and the guest joined. The host is Player 1.
- Both are sitting in DevBridge, not the Netplay app. `OPEN_GAME` does nothing
  while the Netplay app is in the foreground.
- The ROMs are at the paths in each test, on both devices.
- Locally: DevBridge.pak checked out next to this repo (or set `DEVBRIDGE`),
  ffmpeg, and Pillow.

## Running

    testing/devbridge/run.sh HOST_ADDR GUEST_ADDR testing/devbridge/*.test

Options: `--port N` (both devices), `--identity KEY`, `--user USER`,
`--keep-going`, `--record`.

On spruce (App/Netplay), log in as spruce with its password:

    DEVBRIDGE_PASSWORD=... testing/devbridge/run.sh --user spruce HOST GUEST testing/devbridge/sor2.test

run.sh maps the tests' NextUI ROM folders to spruce's (`Sega Genesis (MD)/` to
`MD/`) and keeps spruce references in `refs/spruce-<platform>`; `quit.sh`
quits spruce's RetroArch. sor2, rcr and contra3 pass Brick to RG35XXH. The
paired-core tests (tetris, mkart) need instanced cores the spruce build does
not offer yet, and ctr needs CTR on both cards. Sessions, screenshots and closest-miss captures go to
`testing/results/devbridge/`.

On an ad hoc network the guest is not on your LAN. Tunnel through the host,
which keeps its LAN connection while serving the network:

    ssh -f -N -L 127.0.0.2:2222:10.0.0.20:22 -L 127.0.0.3:2222:127.0.0.1:22 root@HOST_LAN_IP
    testing/devbridge/run.sh --port 2222 127.0.0.3 127.0.0.2 testing/devbridge/*.test

(`10.0.0.20` is the guest's lease; the host's `/tmp/netplay_udhcpd.leases`
has it.)

## The unattended matrix

`matrix.py` runs the whole suite with nobody at the devices. Each cell sets
itself up from idle and puts everything back: power save on both devices, the
host arms headless (`device/ctl.sh`, which runs `netplay.elf --ctl`) over WiFi
or its own ad hoc network, the guest finds it by its announcement and joins,
the test runs, `check.sh` judges both logs, the host ends the session, and the
guest has to come back to its house network by itself before the next cell.

    cp rig.example.json rig.json     # devices, how to reach them, the pairs
    ./matrix.py --list               # the cells
    ./matrix.py                      # everything (hours)
    ./matrix.py --only 'ds\..*\.adhoc' --retries 0
    ./matrix.py --resume ../results/matrix/<run>   # skip what already passed

Plans:

- `ds`: Mario Kart DS (`mkds.test`, one template for every emulator pairing)
  for Brick to Brick and Brick to RG35XXH with either hosting, DSP/DSP,
  DraStic/DraStic and both mixed pairings, over WiFi and ad hoc, with WiFi
  power save off (with it on, a Brick's MP replies came too late: the cells
  measured the setting, not Netplay).
- `systems`: NES, SNES, GB, GBA (gpSP and mGBA) and PlayStation with a Brick
  hosting the RG35XXH, and PlayStation Brick to Brick, over WiFi and ad hoc.

Devices are reached over USB ADB or SSH (key, or a password for a device
without it). A guest that left the LAN for the host's ad hoc network is
reached through an SSH tunnel via the host, which keeps its house network
(the runner turns Ad Hoc Network Switch off). A reference a cell needs but
`refs/` lacks is recorded in its place; captures from a passing cell are
copied into `refs/` and listed in the report, to be looked at.

Output, in `testing/results/matrix/<time>/`: `results.jsonl` (one line per
attempt: fps, MP timeouts, stalls, arm time, how long the guest took to get
home, the radio's real power-save state), `report.md`, and per cell the
runner's log, the rendered script and DevBridge's session with every
screenshot. A failed cell is retried once (`--retries`); a device that cannot
be brought home twice running stops the run.

## How the tests are built

**Input runs on the devices.** DevBridge's `BTN` sends press and release as two
remote calls, so a hold is really "hold + one round trip": over a tunnelled ad
hoc link that is a second or more, which auto-repeats menus and misses
edge-triggered presses. The tests call `device/keys.sh` through `TRM` instead,
which holds for exactly what it says; `--bg` lets both devices press together.
`run.sh` copies `device/*.sh` to `/tmp/netplay-devbridge/` first.

**References are per platform and per role.** Resolution and palette differ by
device, and in instanced and link games each device shows its own console.
References live in `refs/<platform>/`; a `-host` or `-guest` suffix marks
screens that depend on the role. The committed set was recorded with a Brick
(`tg5040`) hosting and an RG35XXH (`h700`) joining; `dsp-mkds.test` also has
Brick guest references, so two Bricks can run it. Other pairings or the
reverse roles need their own references:

    testing/devbridge/run.sh --record HOST_ADDR GUEST_ADDR testing/devbridge/aw2.test

Record mode waits out each `VERIFY_WAIT` timeout and captures the screen in its
place. It writes over references, so look at what it recorded before keeping it.

**Checks are measured, not guessed.** DevBridge scores a screen by the mean
pixel difference of a 64x48 greyscale reduction: 0 is identical. Animated
screens never score 0, and some different screens are close: Mario Kart's
CHOOSE A GAME and cc screens are 11 apart, and Advance Wars 2's day 1 and day 2
are 0.4 apart because only the funds change. Each check's `threshold=` and
`region=` comes from measuring correct-screen noise on both devices against
the distance to every other screen in the test, and the comments in the test
say what was measured. Regions written with a decimal point are fractions of
the screen, so one crop works at 640x480 and 1024x768.

Some checks are left out on purpose, with a note where they would be: an idle
menu that rolls into an attract demo faster than both screens can be
captured, or a highlight that differs from the unhighlighted screen by less
than the background animation. A later check catches what they would have.
