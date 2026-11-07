# Nintendo DS local wireless (DraStic)

Netplay.pak adds DS local wireless to the user's own NDS.pak (DraStic). Each
device runs its own DS, and the DS radio travels between them in melonDS's LAN
protocol, so the other console can be DraStic, DSperate (DSP.pak, see
[dsperate.md](dsperate.md)) or melonDS. Two handhelds race Mario Kart DS at
55-58 fps over Netplay's ad hoc network.

DraStic is closed and has no Wi-Fi of its own. `drastic-wifi/` gives it one:
a preloaded library that detours DraStic's Wi-Fi MMIO and event loop into a DS
radio built from DSperate's port of melonDS's Wi-Fi block and LAN code. What
that radio has to reproduce is in [ds-wifi-mp-protocol.md](ds-wifi-mp-protocol.md).

## How it fits together

| Piece | Where | What it does |
|---|---|---|
| `drastic-wifi/src/radio/` | this repo | DSperate's `Wifi` (cut loose from its core), `LanMp`, vendored ENet, and the C glue (`radio.cpp`) |
| `drastic-wifi/src/radio_hook.c` | this repo | Attaches the radio to DraStic 2.5.2.0 (AArch64) |
| `drastic-wifi/pak/launch.sh` | this repo | minui-nintendo-ds-pak's launcher plus the Netplay session |
| `make cores-nds` | Makefile | Builds the overlay for tg5040, tg5050 and h700 into `dist/nds/<p>/`, and fetches NDS.pak 0.14.1 into `dist/nds/base/` |
| `cores/nds/<p>/` | in Netplay.pak | `NDS.pak/{launch.sh, drastic/libs/libdrastic_wifi.so}` and `drastic.sha256` |
| `launcher/nextui/pak-manage.sh nds` | in Netplay.pak | Adds the overlay to `Emus/<p>/NDS.pak`, keeps what it replaces, puts it back |
| Configuration > Cores > Extras > NDS | the app | Adds the overlay (and NDS.pak itself when the card has none), takes it out, or deletes NDS.pak |

drastic-wifi is GPL-3.0-or-later (it carries DSperate and melonDS code); the
rest of Netplay.pak keeps its own license.

### Not a pak, an overlay

Netplay adds two files to the NDS.pak already on the card, and only if that
pak's `drastic` is the build the radio hooks (`drastic.sha256`, the 2.5.2.0
release minui-nintendo-ds-pak ships), so the user's DraStic settings stay
theirs. A card with no NDS.pak gets minui-nintendo-ds-pak's 0.14.1 release,
unmodified, first: `make cores-nds` downloads it (pinned by SHA-256) into
`cores/nds/base/`, and `pak-manage.sh nds install` copies it in before the
overlay. `pak-manage.sh nds remove` deletes NDS.pak from the card.

The launcher the overlay replaces is kept under
`.userdata/shared/Netplay/nds/originals/` and put back when the setting is
turned off (or when Netplay is turned off and the user says so).

If the card's NDS.pak carries another DraStic build, the app says "This
NDS.pak's DraStic build is not supported" and leaves it alone. The check is the
`drastic` binary's hash alone, which is not enough: NDS.pak 0.12.0 carries the
same DraStic binary as 0.14.1, passes, and comes up on a white screen under the
0.14.1 launcher. A stricter release check is in [../upcoming.md](../upcoming.md).

### What the launcher does

It is minui-nintendo-ds-pak 0.14.1's launcher (the release that added h700)
with a Netplay block added. Without `.userdata/shared/Netplay/session` it
behaves exactly like that launcher. With one:

1. Sources Netplay's `pre-launch.sh` (stale-session cleanup, power save, the
   host's ad hoc network).
2. Host: `DRASTIC_WIFI_LAN=host`. Guest: `DRASTIC_WIFI_LAN=join:<peer>`
   (validated as IPv4).
3. MP waits of 50 ms on ad hoc, 100 ms on a session without an ad hoc network,
   or `dsp_lan_timeout_ms` from the session. melonDS's own 25 ms timed out
   about ten times as often on ad hoc: DraStic pairs raced at 44 fps with it and
   56 fps with 50 ms.
4. The performance governor at the top frequency for the session (the waits
   look idle to a utilisation governor; measured with DSperate). cleanup()
   restores the pak's own settings.
5. Deletes the auto-resume slot (slot 0): it is where this console last quit,
   not where the other is.
6. With verbose logs: `DS_LAN_LOG=1`, and the log is archived to
   `logs/netplay-games/<game>/<session>/` for `testing/devbridge/device/check.sh dsp`.

## Inside DraStic

`radio_hook.c` detours three functions, installed three seconds after start
because Advanced DraStic patches itself early in `main`. The offsets and
prologues are checked against `DWIFI_BRICK_*` in
`include/drastic_wifi_hook.h`:

| DraStic | Becomes |
|---|---|
| `load_wifi_16` (+0x13660) | every Wi-Fi read is the radio's `read16` |
| ARM7 `store16` (+0x128d0) | writes to 0x0480xxxx go to the radio, the rest to DraStic |
| `execute_events` (+0x216e0) | master cycles since the last call become radio microseconds; once per emulated frame the LAN is serviced |

The radio uses DraStic's own 8 KB Wi-Fi RAM (`memory + 0x17070`, confirmed at
bind time by a write through DraStic's store path), because the guest reads
and writes it directly. Interrupts go through DraStic's IF/IE/IME and
active-interrupt fields. POWCNT2 is read from DraStic's ARM7 I/O block.

Nothing is installed unless `DRASTIC_WIFI_LAN` is set, so single-player
launches are DraStic as it was.

## Why a port and not a model

The first attempt (August 2026) wrote its own model of the DS Wi-Fi hardware
behind the same hooks, carried over Netplay's TCP link. It got two Bricks
through discovery, association and a stable Asphalt: Urban GT 2 lobby, and
never past the host starting the race: the guest always reported
"Communication error" 1.5 to 2.5 s into the host's emulated time.

The cause was clocks, not the model's register details. melonDS, and so
DSperate, keeps the client on the host's time: the client adopts the host's
microsecond timestamp when it associates, holds each host frame until its own
clock reaches the frame's timestamp, and blocks for the next host frame when it
gets there. The client can never run ahead. The model's two clocks ran free; the
host ran at 0.1x real time (blocked on replies), the client at 0.9x, and on the
client's clock the host went silent for seconds.

Two more lessons from that work carried into the port:

- **The transport must be allowed to lose stale frames.** melonDS sends MP
  frames unsequenced over ENet and drops anything older than a video frame. TCP
  delivered every frame late instead, so the client worked through commands
  whose moment had passed.
- **Porting the whole radio brought exact behaviour** the model had patched in
  one piece at a time: MP command/reply/ack timing, transmit-slot mapping, the
  Reply1/Reply2 latch, packet RAM handling.

The model's code remains in `drastic-wifi/src/{model,adapter,transport,hook}.c`
for the A30 diagnostic library (`DIAG_LIB` in `drastic-wifi/Makefile`), and its
engineering log is in git history (`docs/drastic-brick-devbridge-progress.md`
and `docs/drastic-feasibility.md` at `442efa2`).

## Measured

### Two handhelds

A TrimUI Brick (tg5040) and an RG35XXH (h700), both on NDS.pak 0.14.1 with the
overlay installed through the app, raced Mario Kart DS (Figure-8 Circuit, a
minute of random input). Both screens agreed on positions at 30 and 60 seconds,
and `check.sh dsp mariokart` passed on both logs.

| Session | Link | Race speed |
|---|---|---|
| Brick hosts an ad hoc network, RG35XXH joins | 2 ms RTT | 55-58 fps on both |
| RG35XXH hosts over WiFi, Brick joins | house AP, 20 ms average RTT, 60 ms spikes, 100 ms waits | 31-38 fps |

The test matrix ([../testing/test-matrix.md](../testing/test-matrix.md)) runs
all four DraStic/DSperate pairings, Brick to Brick and Brick to RG35XXH, either
device hosting: 54-58 fps on ad hoc with WiFi power save off, near 30 fps over
the house WiFi on Brick pairs.

### Cost

| | Result |
|---|---|
| DraStic alone, single-player race, session conditions (radio hooked, performance governor) | 60 fps; main thread 63.6% of a core (about 10.6 ms a frame), helpers about 30% more |
| DraStic host + DSperate headless guest on the same Brick, over loopback | race 44-47 fps with the CPU saturated, only about 2 ms a frame waiting |

So DraStic is only modestly cheaper than DSperate on Mario Kart (DSperate's
headless median is 11.6 ms a frame). A race needs one MP exchange a frame; over
ad hoc that adds a few milliseconds to DraStic's 10.6, close to the 16.7 ms
budget. The two consoles run in lockstep, so a mixed pair goes at the slower
one's pace.

## Testing

- `testing/test-nds-manage.sh` (in `make test`): overlay install, status
  (no base pak, unsupported DraStic), restore after a reinstall.
- `testing/devbridge/nds-nds-mkds.test` and the matrix's `mkds.test`: two
  handhelds, lobby to race.
- `drastic-wifi/devbridge/peer/`: a scriptable headless DSperate peer on a PC,
  and the relay for hosting from WSL2.
- `testing/devbridge/device/check.sh dsp <rom> <fps>` judges a DraStic log
  exactly as it judges DSperate's (same "lan pacing:" lines).

DevBridge input into DraStic: DraStic's own menus need 0.2 s holds (0.3 s
auto-repeats; 0.15 s is dropped on h700), in-game 0.3 s. Check the screen
before each A: Mario Kart drops the first D-pad press after its menu settles,
and falls into its attract demo about 30 s after reaching the main menu, so
script menu-to-group as one run. On h700, DraStic only sees DevBridge's virtual
pad with DevBridge's game-launch remap for ANBERNIC-keys. To stop it, use
`quit.sh` (menu, then SIGTERM, then SIGKILL): h700 DraStic can hang on SIGTERM.

## Known gaps

- **Frame phase.** DSperate's patch 0107 lines the two consoles' frames up by
  honouring VCOUNT writes (see [dsperate.md](dsperate.md)). DraStic, behind the
  hook, still keeps whatever phase it started with: a DraStic guest of an
  H700 DSperate host once made about three MP exchanges a frame and ended in a
  Communication error. Finding DraStic's equivalent is open.
- **tg5050**: the overlay is built (same DraStic binary) but never run.
- **The A30** (DraStic 2.5.0.4, ARMv7) has only the old diagnostic library; the
  radio hook is AArch64 only.
- **Save states** know nothing of the radio; loading one mid-session
  desynchronises it, as it would on DSperate.
- **H700 DraStic exit** can segfault after "radio off", likely a shutdown race
  in the radio teardown. It does not affect play.
