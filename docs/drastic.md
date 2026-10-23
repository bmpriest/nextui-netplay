# Nintendo DS local wireless (DraStic)

Netplay.pak can add DS local wireless to the user's own NDS.pak (DraStic). Each
device runs its own DS, and the DS radio is carried between them in melonDS's
LAN protocol, so the other console can be DraStic, DSperate (DSP.pak, see
[dsperate.md](dsperate.md)) or melonDS.

DraStic is closed and has no Wi-Fi of its own. `drastic-wifi/` gives it one:
a preloaded library that detours DraStic's Wi-Fi MMIO and event loop into a
DS radio built from DSperate's port of melonDS's Wi-Fi block and LAN code.

## How it fits together

| Piece | Where | What it does |
|---|---|---|
| `drastic-wifi/src/radio/` | this repo | DSperate's `Wifi` (cut loose from its core), `LanMp`, vendored ENet, and the C glue (`radio.cpp`) |
| `drastic-wifi/src/radio_hook.c` | this repo | Attaches the radio to DraStic 2.5.2.0 (AArch64) |
| `drastic-wifi/pak/launch.sh` | this repo | minui-nintendo-ds-pak's launcher plus the Netplay session |
| `make cores-nds` | Makefile | Builds the overlay for tg5040 and tg5050 into `dist/nds/<p>/` |
| `cores/nds/<p>/` | in Netplay.pak | `NDS.pak/{launch.sh, drastic/libs/libdrastic_wifi.so}` and `drastic.sha256` |
| `launcher/pak-manage.sh nds` | in Netplay.pak | Adds the overlay to `Emus/<p>/NDS.pak`, keeps what it replaces, puts it back |
| Settings > Nintendo DS (DraStic) | the app | Turns the overlay on and off |

drastic-wifi is GPL-3.0-or-later (it carries DSperate and melonDS code); the
rest of Netplay.pak keeps its own license.

### Not a pak, an overlay

Unlike MGBA.pak and DSP.pak, Netplay does not ship NDS.pak. It adds two files
to the NDS.pak already on the card, and only if that pak's `drastic` is the
build the radio hooks (`drastic.sha256`, the 2.5.2.0 release minui-nintendo-ds-pak
ships). So DraStic is never redistributed, and the user's DraStic settings stay
theirs. The launcher it replaces is kept under
`.userdata/shared/Netplay/nds/originals/` and put back when the setting is
turned off (or when Netplay is turned off and the user says so).

If NDS.pak is missing the app says "Install NDS.pak (DraStic) first"; if its
DraStic is another build, "This NDS.pak's DraStic build is not supported".

### What the launcher does

Without `.userdata/shared/Netplay/session` it is minui-nintendo-ds-pak's
launcher, unchanged. With one:

1. Sources Netplay's `pre-launch.sh` (stale-session cleanup, power save, the
   host's ad hoc network).
2. Host: `DRASTIC_WIFI_LAN=host`. Guest: `DRASTIC_WIFI_LAN=join:<peer>`
   (validated as IPv4).
3. MP waits of 100 ms on a session without an ad hoc network, melonDS's 25 ms
   on ad hoc, or `dsp_lan_timeout_ms` from the session.
4. The performance governor at the top frequency for the session (the waits
   look idle to a utilisation governor; measured with DSperate). cleanup()
   restores the pak's own settings.
5. Deletes the auto-resume slot (slot 0): it is where this console last quit,
   not where the other is.
6. With verbose logs: `DS_LAN_LOG=1`, and the log is archived to
   `logs/netplay-games/<game>/<session>/` for `testing/devbridge/device/check.sh dsp`.

## Inside DraStic

`radio_hook.c` detours three functions at offsets and prologues the earlier
diagnostic hook validated on hardware (`DWIFI_BRICK_*` in
`include/drastic_wifi_hook.h`), installed three seconds after start because
Advanced DraStic patches itself early in `main`:

| DraStic | Becomes |
|---|---|
| `load_wifi_16` (+0x13660) | every Wi-Fi read is the radio's `read16` |
| ARM7 `store16` (+0x128d0) | writes to 0x0480xxxx go to the radio, the rest to DraStic |
| `execute_events` (+0x216e0) | master cycles since the last call become radio microseconds; once per emulated frame the LAN is serviced |

The radio uses DraStic's own 8 KB Wi-Fi RAM (`memory + 0x17070`, confirmed at
bind time by a write through DraStic's store path), because the guest reads
and writes it directly. Interrupts go through DraStic's IF/IE/IME and
active-interrupt fields, as the earlier adapter did. POWCNT2 is read from
DraStic's ARM7 I/O block.

Nothing is installed unless `DRASTIC_WIFI_LAN` is set, so single-player
launches are DraStic as it was.

## Why this works where the hand-written model did not

`docs/drastic-brick-devbridge-progress.md` records the earlier model in
detail. Its last finding (2026-09-04) was that the lobby always died after
1.5 to 2.5 s of the host's emulated time, whatever the transport did. The
host ran at 0.1x real time (blocked on replies) and the client at 0.9x.

melonDS, and so DSperate, does not let the clocks drift. The client adopts the
host's microsecond timestamp when it associates, holds each host frame until
its own clock reaches the frame's timestamp, and blocks for the next host
frame when it gets there. The client can never run ahead of the host. The old
model had no equivalent, so on the client's clock the host went silent for
several seconds and the game gave up. Porting the whole radio, rather than
patching the model, also brought melonDS's exact MP command/reply/ack timing,
transmit-slot mapping and packet RAM handling. Several of those had been
patched individually in the old model.

## Measured

Only one handheld was reachable, so the other console was DSperate's headless
frontend on a PC, joining over the house Wi-Fi (`drastic-wifi/devbridge/peer/`):

| DraStic (Brick) | Peer | Link | Result |
|---|---|---|---|
| host | DSperate, joins | UDP through the house AP, 22 ms mean RTT, 90 ms spikes | room found, lobby 2/8, CUT OFF, VS, race; 35-49 fps |
| guest | DSperate hosts | UDP over an SSH TCP tunnel | CUT OFF fails at 25 ms waits; with 100 ms: race, 24-37 fps |
| host, real Netplay session (Host over WiFi) | DSperate, joins | house AP | race; `check.sh dsp mariokart`: PASS, 37.9 fps average |

DraStic itself used 15% of the Brick's CPU during these races; the rest was
waiting on the link. At those round trips DSperate itself only managed about
a third of full speed (dsperate.md).

Two more measurements on the Brick alone bound what two handhelds can do:

| | Result |
|---|---|
| DraStic alone, single-player race, session conditions (radio hooked, performance governor) | 60 fps; main thread 63.6% of a core (about 10.6 ms a frame), helpers about 30% more |
| DraStic host + DSperate headless guest on the same Brick, over loopback | lobby 60 fps (one MP wait a frame, 6 ms with both emulators sharing the CPU); race 44-47 fps with the CPU saturated (11% idle) but only about 2 ms a frame waiting |

So DraStic is only modestly cheaper than DSperate on Mario Kart (DSperate's
headless median is 11.6 ms a frame), not the large margin assumed at the
start. A race needs one MP exchange a frame. Over ad hoc (3 ms round trips,
plus the other console catching up to the command's time) that adds a few
milliseconds to DraStic's 10.6, close to the 16.7 ms budget. The two consoles
run in lockstep, so a DraStic/DSperate pair goes at DSperate's pace (about
50 fps in a race, dsperate.md). Two DraStic handhelds should do somewhat
better. Neither pairing has been measured on two handhelds yet.

## Testing

- `testing/test-nds-manage.sh` (in `make test`): overlay install, status
  (no base pak, unsupported DraStic), restore after a reinstall.
- `drastic-wifi/devbridge/peer/`: the scriptable DSperate peer and the relay
  for hosting from WSL2.
- `testing/devbridge/device/check.sh dsp <rom> <fps>` judges a DraStic log
  exactly as it judges DSperate's (same "lan pacing:" lines).

DevBridge input into DraStic: use `BTN` with gaps of a second or more and
check the screen before each A. Mario Kart drops the first D-pad press after
its menu settles, and `keys.sh`'s short holds are missed.

## Not done / known gaps

- Two handhelds over ad hoc: not yet measured.
- tg5050: the overlay is built (same DraStic binary) but never run.
- H700: no NDS.pak for NextUI-h700, so no overlay. An H700 can still be the
  other console with DSP.pak.
- The A30 (DraStic 2.5.0.4, ARMv7) still has only the old diagnostic library;
  the radio hook is AArch64 only.
- DraStic's own save states know nothing of the radio; loading one mid-session
  desynchronises it, as it would on DSperate.
