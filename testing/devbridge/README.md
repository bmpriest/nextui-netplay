# Two-device gameplay tests

Scripted DevBridge runs that take two handhelds from launch to real
multiplayer play, one per netplay mode, and fail if any screen along the way or
the netplay logs afterwards say the devices did not play together.

| Test | Mode | What it plays |
| --- | --- | --- |
| `sor2.test` | shared screen (PicoDrive) | Streets of Rage 2: 2 PLAYERS, each device picks its own fighter, 90 s of brawling |
| `tetris.test` | instanced GB link (paired Gambatte) | Tetris: 2PLAYER, Mario vs Luigi at different heights, 60 s of play |
| `mkart.test` | instanced GBA link (paired mGBA) | Mario Kart: Super Circuit: Multi-Pak VS, 100cc, Mario vs Yoshi, 2 min on Peach Circuit |
| `aw2.test` | networked GBA link cable (gpSP) | Advance Wars 2: Multi-Pak battle, a full turn each way (the guest builds a tank) |
| `dsp-mkds.test` | DS local wireless (DSP.pak on both) | Mario Kart DS: group, lobby, VS race on Figure-8 Circuit |
| `nds-mkds.test` | DS local wireless, DraStic hosting, DSperate joining | the same race between the two emulators |
| `nds-nds-mkds.test` | DS local wireless, DraStic on both | the same race, Brick and RG35XXH both on NDS.pak |

Gameplay is random input from both devices (`device/gameplay.sh`). After play,
`device/check.sh` reads each device's own netplay log and fails on divergence,
a missing link agreement, dropped link packets or a session average under the
test's minimum frame rate.

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

Options: `--port N` (both devices), `--identity KEY`, `--keep-going`,
`--record`. Sessions, screenshots and closest-miss captures go to
`testing/results/devbridge/`.

On an ad hoc network the guest is not on your LAN. Tunnel through the host,
which keeps its LAN connection while serving the network:

    ssh -f -N -L 127.0.0.2:2222:10.0.0.20:22 -L 127.0.0.3:2222:127.0.0.1:22 root@HOST_LAN_IP
    testing/devbridge/run.sh --port 2222 127.0.0.3 127.0.0.2 testing/devbridge/*.test

(`10.0.0.20` is the guest's lease; the host's `/tmp/netplay_udhcpd.leases`
has it.)

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
(`tg5040`) hosting and an RG35XXH (`h700`) joining. Other pairings or the
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
