# Advanced DraStic Brick multiplayer and DevBridge progress

Last updated: 2026-08-31

## Current status

This is a chronological engineering log. Older diagnoses and next-step lists
are retained as history but are superseded by this section and the latest dated
entry below.

Two TrimUI Bricks can now discover, authenticate, associate, and remain in an
Asphalt Urban GT 2 multiplayer lobby. Player 2 remained present for more than
two minutes in unattended observation. The remaining failure is the transition
from that connected lobby into gameplay: the host's on-screen instruction is
`Press A button to start`; after the host presses `A`, the host retains player 2
but the client reports a communication error.

The stable build includes split peer waits, immediate semantic-reply accounting
while modeled RX is busy, Reply1/Reply2 latch behavior, and melonDS-compatible
reply-duration rejection. Asphalt advertises a 358 us client window, so its
staged 140-byte reply cannot fit and the model correctly sends an empty reply.
Packet-sync diagnostics identify the repeating 34-byte MP command at
`W_TXSlotCmd` (`0x0090`). The current investigation boundary is command
completion and IRQ-visible state across the host-`A` start transition.

DevBridge automation, screenshot baselines, safe shutdown, and the exact
reproduction workflow are documented in the latest sections of this file and
in `drastic-wifi/README.md`. Device testing was paused with both consoles safely
returned to DevBridge.

## 2026-08-30 independent verification and staged-RX breakthrough

The earlier conclusion that all host-visible association inputs were correct
was too strong.  The captured 802.11 bytes were correct, but the receive device
behavior was not.  Comparison with the official melonDS Wi-Fi device model
identified two relevant differences: melonDS raises RX-start, transfers the
payload progressively over emulated airtime through `W_RXTXAddr`, and only then
raises RX-complete; it also treats `W_RXTXAddr` as a halfword address.  Our model
previously wrote the entire packet atomically and did not advance that register.

The model now owns one in-flight receive frame and:

- raises IRQ6 and enters RF receive state at the start;
- initializes `W_RXTXAddr` to `(write_cursor + 12) >> 1`;
- transfers one halfword every 8 us at 2 Mbit/s or 16 us at 1 Mbit/s;
- advances `W_RXTXAddr` after each halfword;
- installs the RX header, publishes the circular write cursor, raises IRQ0, and
  returns to idle only after the cropped frame airtime elapses.

A native regression test verifies that RX-start is visible while packet header
and write cursor are still unpublished, that payload and `W_RXTXAddr` advance,
and that RX-complete appears later.  All hook, model, and adapter tests pass.

This changed the two-Brick Mario Kart result decisively.  The host association
response changed from status 1/AID 0 to status 0/AID 1, the client stopped
deauthenticating immediately, and the host displayed both racers (2/8).  This
rules out the earlier theory that the retail host was rejecting otherwise
correct state for an unknowable game-level reason.

The next failure was packet loss caused by transport draining: an MP ACK and
CMD arrive back-to-back, and the second packet was popped while the modeled
radio was still busy.  Transport now leaves subsequent NetLink packets queued
until RX completes.  That allowed the client to receive commands and synthesize
MP replies.

The synthesized empty reply itself then proved malformed.  Its destination was
zero and its third address field held the host BSSID, so the host classified it
as ordinary data rather than an MP reply.  It now uses the hardware layout:

- destination: host BSSID;
- source: station MAC;
- third address: `03:09:bf:00:00:10` (MP reply flow);
- duration `0x00f0` and a generated sequence number.

The host now receives and recognizes such replies: its MP ACK has been observed
changing `clientfail` from `0x0002` to `0x0000`.  However, the link is not yet
stable.  Host TX is still effectively instantaneous, while RX is now correctly
paced.  ACK/CMD pairs accumulate in the client's NetLink queue (high-water mark
448/512), replies arrive many command generations late, and Mario Kart removes
the second racer after several seconds.  Reducing transport service polling
from 256 us to 64 us did not remove the backlog.

The next implementation task is therefore TX airtime, not another association
heuristic: stage local/CMD/ACK transmission and completion IRQs over the same
radio timing model instead of sending and completing synchronously in
`fire_tx`.  The current broad `tx_slot_armed` selection remains suspicious
because official melonDS strictly maps request bits 0/1/2/3 to
LOC1/CMD/LOC2/LOC3; change it only as a separate, traceable experiment after TX
pacing, since the current DraStic write order otherwise fails to transmit the
association response.

The active 2026-08-30 build containing staged progressive RX, busy-RX queue
flow control, corrected empty MP replies, and the 64 us service poll is deployed
to `libdrastic_wifi_diag.so` on both Bricks.  The saved cold macro remains the
reproduction command.  Session screenshots demonstrating successful 2/8 entry
are under `devbridge-sessions/drastic-cold-join-20260830-030820/` and
`devbridge-sessions/drastic-cold-join-20260830-031144/`.

### 2026-08-30 MP stability follow-up

Further DevBridge runs established that Mario Kart's `0x0228` MP command frames
also leave through DraStic's LOC2 register, not the nominal command slot.  MP
state selection is therefore now based on the transmitted frame type and
`03:09:bf:00:00:00` destination rather than the slot register containing it.
Command reply windows begin after command airtime, and IRQ12/header completion
are delayed until the generated ACK airtime ends.  A unit test exercises this
semantic command path through LOC2.

Preserving every frame while RX was busy was also shown to be wrong: it built a
448/512-frame reliable FIFO that a radio would not provide.  Transport now has
a bounded over-the-air inbox which retains the newest command, ACK, reply,
beacon, and ordinary frame separately, delivering a command before its closing
ACK.  This completely removed NetLink queue high-water/backpressure events and
the client began receiving current command sequence numbers and replying
continuously.

The lobby nevertheless still falls from 2/8 to 1/8 and the client reports an
error after several seconds.  Extending the wall-clock peer wait from 3 ms to
20 ms prevented the initial join and was reverted.  Carrying a valid one-client
reply across adjacent command generations, first for 100 ms and then latched
for the session, also did not prevent the client-side error.  That rules out the
host's `clientfail` mask as the sole remaining cause.

The next comparison should focus on the generated MP ACK contents and timing,
especially `CmdCounter`/runahead, `W_CMD_COUNT`, ACK header words 0--3, and the
client's processing of IRQ/type `0xD`.  Current ACK creation only approximates
these fields, whereas melonDS derives runahead from command time and the next
beacon and schedules the ACK through a multi-phase TX state machine.  The
latest deployed build is the bounded-inbox, semantic-command, 3 ms-wait build;
the 30-second failure screenshot is in
`devbridge-sessions/devbridge-20260830-072612/`.

This document records the current state of Nintendo DS local multiplayer work
for Advanced DraStic on the TrimUI Brick. It is intended as a restartable
engineering handoff: the device setup, reproducible DevBridge workflow,
confirmed findings, current status, and next experiments are all captured here.

### 2026-08-30 Asphalt cross-check and MP inbox ordering

The saved `drastic-wifi/devbridge/asphalt-cold-join.test` macro now reproduces
the same failure in Asphalt: Urban GT 2 as Mario Kart.  Asphalt discovers the
host, associates, briefly displays player 2 in the host lobby, then removes the
player; the client eventually reports `COMMUNICATION ERROR`.  This confirms the
failure is in shared DS local-multiplayer behavior rather than game-specific
Mario Kart handling.

The first Asphalt trace exposed starvation in the bounded radio inbox.  With
commands preferred over ACKs, the client received dozens of commands but only
one ACK.  Reversing that priority produced 55 ACKs but only three commands.
The inbox now alternates the two roles after delivering a command, while still
giving client replies priority at the host.  A cold device run then produced a
balanced 20 commands, 20 automatic replies, and 20 ACKs with no NetLink FIFO
backlog.  Asphalt still removed player 2, so missing ACK delivery is fixed but
is not the final protocol defect.

An experiment that continuously decremented the mirrored `W_CMD_COUNT` in 10 us
units and emitted that raw value in the ACK prevented Asphalt from completing
the initial join.  It was reverted.  This demonstrates that DraStic's value at
the hook boundary cannot simply be treated as melonDS's private `CmdCounter`;
the ownership/write/start semantics must be established from DraStic traces
before changing it again.  The best deployed build therefore retains balanced
CMD/ACK alternation and the previous ACK counter approximation.  The remaining
focus is the client's type-`0xD` ACK processing and the correspondence between
each ACK's sequence/counter fields and the command/reply generation it closes.

Generation pairing was subsequently made explicit. Separate CMD and ACK
coalescing had produced apparently balanced traffic while delivering mismatched
pairs (for example CMD 360 followed by ACK 376). Transport now buffers a
complete generation, validates the host-observed invariant `ack_seq =
cmd_seq + 1`, and publishes the command state before guest delivery so the
automatic reply's synchronous peer-wait cannot re-enter and prematurely
promote the next pair. The resulting Asphalt trace contained 20 exact pairs:
112/113, 129/130, through 984/985, with one automatic reply per command and no
queue backlog. Asphalt still removed player 2. This rules out both role
starvation and cross-generation ACK splicing as the final defect.

The first two exact ACKs carried `clientfail=0x0002`, with all later ACKs
carrying zero. An experiment reporting immediate optimistic success for every
one-client command did not extend the session and was reverted. The next
investigation should therefore trace what Nintendo software reads after a
type-`0xD` receive—RX header words/status and interrupt order—and the host CMD
header/status writes at ACK completion.

### 2026-08-30 MP completion and ACK-rate follow-up

Two more melonDS-conformant completion details were implemented and exercised
through the saved cold Asphalt macro on both Bricks.  The command header is now
written before the TX-complete interrupt; after a generated ACK, the command
slot's success count is incremented on success and the ACK consumes the next
`W_TXSeqNo` value.  Both changes have model regression coverage.  The
post-join result was unchanged: player 2 appeared briefly, then disappeared
from the host while the client reported `COMMUNICATION ERROR`.

The next comparison found a PHY-level mismatch: the generated ACK was hard
coded as rate `0x14` (2 Mbps) and its completion used the corresponding
320-us duration.  Real hardware and melonDS inherit the command frame's rate.
The model now stores that rate, stamps it into the ACK radio header, and keeps
the ACK in flight for `192 + 32 * (4 or 8)` us.  The test's 1-Mbps ACK
completion expectation is updated to 448 us.  This also did not retain player
2 in Asphalt, but it removes a real protocol discrepancy rather than adding a
heuristic.

The active artifact on both Bricks includes a focused `mp-rx` trace.  For each
MP command or ACK it records the flow kind, radio rate, sequence, count,
failure mask, and the status word visible to the receiving game.  Ordinary RX
trace output is intentionally capped during setup, so this separate trace is
needed to prove whether the client accepts the generated ACK as type `0xD`.
The final diagnostic run still needs DevBridge to be foreground on both
devices: the previous cleanup returned both units to NextUI, and a raw ADB
invocation of a pak does not recreate NextUI's foreground UI ownership.

### 2026-08-30 Live ACK classification and station-reply lifecycle

After reboot, `/tmp/drastic-wifi-session` must be restored on both units before
launching.  A missing file cleanly disables the hook transport; it is not a
protocol failure.  With host `peer=192.168.0.144` and client
`peer=192.168.0.180` restored, the fresh DevBridge Asphalt run confirmed the
full MP exchange in the emulator logs:

- the client receives each host command as status `0x801c` (`fc=0x0228`);
- it receives each generated ACK as status `0x801d` (`fc=0x0218`);
- command/ACK sequence values progress as adjacent generations; and
- the host ACK failure mask becomes zero after the two initial reply windows.

Thus the remaining Asphalt disconnect is not caused by discovery, NetLink
delivery, MP frame classification, ACK PHY rate, or host reply accounting.
The next concrete model mismatch was implemented: automatic station replies
now assert `W_TXBusy` bit 7 during their 2-Mbps on-air duration and defer
`W_TXStat=0x0401`/IRQ1 until the duration ends.  melonDS does both; the prior
model reported the reply complete immediately and never exposed busy bit 7.
The updated model regression test verifies this 304-us default-reply lifetime.
All native hook/model/adapter tests pass and the Brick artifact is installed.
It still awaits a hardware run because force-killing DraStic again left the
DevBridge foreground state `unknown`; reopen DevBridge on both devices before
the next `OPEN_GAME` macro.

The station-reply busy lifecycle was subsequently exercised on both Bricks.
It did not change Asphalt's final state.  Do not use `kill -9` for normal test
turnaround: DevBridge's generic `QUIT_GAME` is MinArch-specific, while Advanced
DraStic has its own menu.  The verified safe DraStic sequence is `MENU`, wait,
`UP`, `UP`, `A`, followed by `WAIT_FOR devbridge 45000`.  It is saved as
`drastic-wifi/devbridge/asphalt-safe-quit.test`; both wrappers returned cleanly
and reported `foreground=devbridge` after this sequence.

Known visual outcomes are recorded in
`drastic-wifi/devbridge/asphalt-screen-baselines.json`.  The cropped-image
classifier `classify-screen.py` deliberately excludes most of Asphalt's
animated background and reports a known state only when both its absolute
distance and its margin over the next state are acceptable.  Run
`classify-asphalt-session.sh SESSION_DIRECTORY` after a macro.  The current
baselines distinguish host `player-2-present`/`player-2-dropped` and client
`room-visible`/`communication-error`; an `ambiguous` result warrants another
delayed screenshot or manual inspection.  The 21:34 run classified cleanly as
player 2 present immediately after joining, then player 2 dropped plus client
communication error after twenty seconds.

The Asphalt macro now retries A once after selecting the highlighted room;
Asphalt sometimes ignores the first synthetic press. Forced emulator exits
also revealed accumulated recursive `DevBridge.pak/bin/game-launch` wrappers
(82 on the host and 154 on the client), which made `foreground=game` stale and
invalidated launches. These were removed by resolving their exact PIDs, and
subsequent resets kill only the current resolved DraStic PID and allow its one
wrapper to return normally to DevBridge.

## Current hardware and software setup

The two test devices are TrimUI Bricks running DevBridge:

| Role | Wi-Fi address | ADB serial |
| --- | --- | --- |
| Host | `192.168.0.180` | `1c000c66050607724dd` |
| Client | `192.168.0.144` | `2c000c81b5c4078245d` |

Both use Mario Kart DS from:

```text
/mnt/SDCARD/Roms/Nintendo DS (NDS)/mariokart.nds
```

The Brick runs Advanced DraStic `2.5.2.0-59a7`. This is a different emulator
package and binary from the A30's DraStic `2.5.0.4`; their menus, wrappers, and
available options therefore differ. Brick work should not be inferred from A30
behavior without independently verifying it.

The active Brick launcher preloads this file:

```text
/mnt/SDCARD/Emus/tg5040/NDS.pak/drastic/libs/libdrastic_wifi_diag.so
```

This is important: deploying only `libdrastic_wifi.so` does not update the
currently tested build. The Brick artifact is built locally at:

```text
drastic-wifi/build-brick/libdrastic_wifi.so
```

and must be copied to the remote `libdrastic_wifi_diag.so` path on both units.

The launcher deletes Mario Kart's automatic slot-zero state before each run.
Advanced DraStic otherwise resumes that state automatically, and interrupted
Wi-Fi tests repeatedly produced corrupt or frozen resumes. Battery-backed
`.sav` data and manually selected state slots remain intact.

## DevBridge testing work

The DevBridge protocol documentation is at:

```text
/home/benja/dev/sbcs/my-paks/DevBridge.pak/PROTOCOL.md
```

The runner used for all automated device interaction is:

```text
python3 /home/benja/dev/sbcs/my-paks/DevBridge.pak/tools/devbridge.py
```

DevBridge was extended so `OPEN_GAME` works from a standalone DevBridge session.
Its game launcher was also corrected for DraStic input: it now bind-mounts both
the virtual event device and its matching joystick device over the physical
`event3` and `js0`, restoring both when the game exits. DraStic/SDL may open
`js0` rather than only `event3`; replacing the event device alone made remote
buttons unreliable. The changed host-side file is:

```text
/home/benja/dev/sbcs/my-paks/DevBridge.pak/bin/game-launch
```

The updated launcher was installed on both Bricks. The DevBridge test suite
passes (10 Python tests plus its shell suite), and remote input now reliably
controls both DraStic processes.

The following reusable macros were created:

- `drastic-wifi/devbridge/launch-mariokart.test`
- `drastic-wifi/devbridge/mariokart-host-create.test`
- `drastic-wifi/devbridge/mariokart-client-join.test`
- `drastic-wifi/devbridge/mariokart-cold-join.test`

The cold macro is the primary regression test. It connects both ADB devices,
uses `OPEN_GAME`, waits for Mario Kart's deterministic idle demo, presses `A`
to return to the primary menu, enters Multiplayer on both consoles, creates a
Normal group on the host, captures both screens, attempts the client join, then
captures status and both post-join screens.

Run it with:

```sh
python3 /home/benja/dev/sbcs/my-paks/DevBridge.pak/tools/devbridge.py \
  --launch-timeout 20 run \
  drastic-wifi/devbridge/mariokart-cold-join.test
```

Mario Kart's primary menu is `Single Player`, `Multiplayer`, and `Nintendo
WFC`. If left idle it starts a demo loop. Pressing a button during the demo
returns to the menu; repeatedly pressing an inert button such as `L` can also
keep it there. The cold macro deliberately uses the deterministic demo return
instead of relying on startup timing.

To stop both DraStic instances cleanly within the DevBridge workflow before a
new deployment:

```sh
python3 /home/benja/dev/sbcs/my-paks/DevBridge.pak/tools/devbridge.py \
  --adb 1c000c66050607724dd=host \
  --adb 2c000c81b5c4078245d=client \
  exec all:TRM killall -9 drastic
```

Allow a few seconds after the kill before the next macro run; DevBridge briefly
reports `foreground=unknown` and refuses `OPEN_GAME` during the handoff.

### Distinct console MAC addresses

Both units shipped the same `nds_firmware.bin`, so both DraStic instances read
MAC `00:16:56:83:31:f9`. The transport used to hide that by rewriting the last
octet of the source, BSSID and destination on the wire and translating it back
on receipt.

The client's firmware is now patched to `00:16:56:83:31:fa`, so the two consoles
have genuinely distinct addresses and no rewriting happens. The MAC is at offset
`0x36` of:

```text
/mnt/SDCARD/Emus/tg5040/NDS.pak/drastic/system/nds_firmware.bin
```

It is covered by the CRC16 at `0x2a`, computed over `size` bytes starting at
`0x2c` (`size` is the halfword at `0x2c`, `0x138` here) with the standard DS
routine: initial value `0x0000`, polynomial `0xa001`, LSB-first. Patch the MAC,
recompute that CRC, and write it back. The unmodified image is kept on the
client as `nds_firmware.bin.orig`.

DraStic takes the MAC from this image; there is no MAC setting in `drastic.cfg`.
The in-game player names come from Mario Kart's own save, not the firmware.

The address translation remains in the transport but is now dormant: it turns
itself on only if a received frame's source address is this unit's own MAC,
which is what happens when both units still run the shipped image.

The transport pairing lives in a file on each device:

```text
/tmp/drastic-wifi-session
```

Host is `role=host`, `port=55437`, `peer=192.168.0.144`; client is
`role=client`, `port=55437`, `peer=192.168.0.180`. This is in `/tmp`, so a
reboot loses it. When it is missing the hook logs `transport=netlink
status=disabled`, both games run normally, and the client's room list stays
empty with no frames in either log. Restore it with:

```sh
adb -s 1c000c66050607724dd shell \
  "printf 'role=host\nport=55437\npeer=192.168.0.144\n' > /tmp/drastic-wifi-session"
```

Each run writes screenshots, command results, and `events.jsonl` beneath
`devbridge-sessions/`. The first run with working discovery is
`devbridge-sessions/drastic-cold-join-20260829-180211`; the first run that also
reached the host's waiting room without either process crashing is
`devbridge-sessions/drastic-cold-join-20260829-181352`.

Per-run emulator logs live on each device and are the primary evidence:

```text
/mnt/SDCARD/.userdata/tg5040/logs/NDS-wifi-<timestamp>.txt   # hook trace
/mnt/SDCARD/.userdata/tg5040/logs/NDS.txt                    # stdout/stderr
```

## Confirmed emulator and Wi-Fi findings

### Packet RAM is not reachable through the 16-bit dispatcher

This was the root cause of every discovery failure, and it invalidates the
previous "encoded address" conclusion recorded here.

The hooked functions are DraStic's ARM7 16-bit load and store dispatchers. The
guest passes them the DS address with different truncation: loads receive it
masked to 16 bits and stores to 24 bits. The same Wi-Fi register therefore
appears in the trace as read `0x00008038` and write `0x00808038`, both meaning
DS `0x04808038`.

DS Wi-Fi RAM lives *below* the registers, at `0x04804000`, not at registers plus
`0x4000`. The corresponding dispatcher addresses are `0x4000 + offset` for loads
and `0x00804000 + offset` for stores. The earlier `0x0080c000` base was wrong:
a bounded read-back probe showed it stores and reloads consistently but is a
different region the guest never sees. The host's own beacon at packet offset
`0x0238` reads correctly at `0x4238` and as zero at `0xc238`.

Correcting the base to `0x00804000` immediately restored discovery: the client
listed the host group `NORMAL / test1`, and the client's authentication frame
reached the host for the first time.

That base then crashed the *host*, and the reason is the more important finding:
**the guest never uses these dispatchers for packet RAM at all.** Across full
runs, every traced access is a register in the `0x8000` / `0x808000` window;
there is not one packet-RAM access. DraStic's fast ARM7 path reads and writes
Wi-Fi RAM directly, and the dispatcher fallback only services the low half.
Both loads and stores fault at packet offsets at or above `0x1000`. Mario Kart
places the host's receive ring at offset `0x10c4` (`W_RXBUF_BEGIN=0x50c4`) and
the client's at `0x0bfc`, which is exactly why the crash was host-only and
always reported offset `0x10c4`.

The hook now locates DraStic's packet RAM directly instead. It reads the low
`0x1000` bytes through the load dispatcher, then searches the mapping that
contains the ARM7 memory context for that signature and keeps a pointer to the
match. Received frames are mirrored into that pointer with `memcpy`, and the
beacon is read back through it, so the whole 8 KiB is reachable and the
dispatcher is bypassed. Both units report the same stable location:

```text
drastic-wifi packet-ram=direct memory_offset=0000000000017070
```

`0x17070` is a candidate constant for this DraStic build, alongside the existing
`BRICK_ARM7_IO_OFFSET` of `0x23070`. It is currently discovered at runtime,
which is safer across builds; the runtime search is the fallback-free path and
the dispatcher path remains only as a low-half fallback if the search ever
fails.

Two mechanical notes, both learned by crashing:

- The search runs from `hooked_execute_events`, the outermost emulator frame.
  Running it from the receive callback segfaulted; that path has very little
  spare stack.
- `/proc/self/maps` is parsed by hand. `sscanf` is not safe to call on these
  stacks either.

### The transmit request bit does not select the transmit slot

The model paired W_TXREQ bits with slot registers on the documented map (bit 0
LOC1, bit 1 CMD, bit 2 LOC2, bit 3 LOC3). Mario Kart's host does not follow it:
it answers an authentication frame by setting W_TXREQ bit 3 and then programming
the slot register at `0x00a4`. Under the fixed map those are different slots, so
`fire_tx` skipped the frame and the reply was silently never sent. The host
looked like it was ignoring the client when it had in fact built a reply.

Slots are now *armed* when the guest enables one (bit 15 in the slot register)
and sent on the next transmit request, whichever bit it carries; the armed flag
is cleared on send, so one request cannot transmit the same slot twice. This
needs no bit assignment. `test_tx_request_bit_does_not_select_slot` covers the
Mario Kart ordering.

With this fix the host replies for the first time, and the exchange runs four
frames deep instead of one.

### Transmitted frames carried no sequence number

`fire_beacon` filled in 802.11 sequence control from W_TXSeqNo, as the MAC does
at transmit time, but `fire_tx` sent frames straight out of packet RAM with
whatever the guest had left there. Every frame a console sent therefore carried
sequence 0.

A receiver's duplicate filter drops repeated frames from the same source with
the same sequence number. The first frame of a conversation got through and
everything after it was discarded before the game ever saw it. That is why the
host answered the authentication request but appeared to ignore the association
request that followed: its driver consumed the frame from the ring and dropped
it as a duplicate.

Sequence generation is now shared by both paths (`apply_tx_sequence`), which
required a static transmit scratch buffer so the header can be written without
touching packet RAM. Sequence numbers now increment on the wire.

This is the change that first produced an association response.

### The microsecond counter was not model-owned

`dwifi_model_owns_read` covered W_IF, W_IE and `0x0030`-`0x00b8`, but not the
microsecond counter block at `0x00e8`-`0x00fe`. The model implements that
counter; DraStic does not. Every read therefore returned DraStic's zero, and the
host polled W_US_COUNT 1435 times in a single run and always saw `0000`.

Ownership now extends to `0x00e8`-`0x00fe`. The counter advances on device
(1556 distinct values in the following run) and tracks wall time 1:1, which also
confirms the ARM9 cycle-to-microsecond conversion is right.

### RX-start interrupt

RX-start (`W_IF` bit 6) had been removed on the theory that raising it together
with RX-complete inside one DraStic event callback would coalesce the two
sources. It was restored, and the trace shows the guest handling both cleanly:
it reads `W_IF=0041`, clears bit 6 and then bit 0, and advances the receive
cursor. Removing it was never the discovery regression; the packet mirror base
was.

### Thread stack crashes

DraStic's event callback runs with very little spare stack. Large temporary
arrays in that path caused immediate or delayed crashes when opening games and
processing network traffic. The following scratch buffers were moved to static
storage:

- the transport's 8 KiB outgoing packet buffer;
- the transport's 8 KiB incoming packet buffer;
- the model's 8 KiB beacon buffer.

Native tests and both ARMv7/AArch64 cross-builds pass after this change.

### Receive length and cursor behavior

The hardware reference states that the RX header is 12 bytes and advertised RX
frame lengths are multiples of four. The model rounds the cropped received
length to four bytes and explicitly zero-pads the stored body.

This is confirmed end to end on device. Mario Kart programs `W_RXLENCROP=0x0602`,
so a 108-byte beacon is stored as 104 bytes; the guest advances `W_RXREADCSR`
by exactly `12 + 104` bytes, matching the modeled cursor.

### Beacon content

The host's transmitted beacon is a well-formed Nintendo local-multiplayer
beacon and needs no further work. Decoded from the trace: broadcast DA, source
and BSSID `00:16:56:83:31:f8`, beacon interval 202, capability `0x0021`, and
IEs for supported rates, DS parameter set (channel 1), TIM, and a 52-byte
vendor IE with OUI `00:09:bf` carrying the UTF-16 host name `test1`.

### Beacon transmission is driven by the beacon interrupt

The model used to beacon from a free-running timer of its own and synthesise
IRQ14 on transmission. That is inverted. On hardware the beacon interrupt is the
driver and the beacon is a consequence of it:

- `W_BeaconCount1` (`0x011c`) counts down once per millisecond and reloads from
  `W_BeaconInterval` (`0x008c`); reaching zero raises the interrupt.
- The interrupt has three distinct sources — microsecond compare, beacon count,
  and a forced write — and they are not interchangeable. A forced interrupt does
  not rearm the beacon counter, and the `W_US_COMPARE` bit 0 gate suppresses
  only the beacon-count source.
- Every source is additionally gated on `W_USCompareCnt` bit 0. A guest that has
  not enabled it does not beacon at all, however long the timer runs.
- Taking the interrupt arms `W_BeaconCount2` (`0x0134`) at `0xffff`, clears the
  transmit request bits except bit 1, transmits the beacon if the beacon slot is
  enabled, and advances the listen counter.
- `W_BeaconCount2` counting down to zero raises IRQ13.

The compare is checked as the microsecond counter moves rather than on the
millisecond tick, because a per-millisecond check drifts off the counter's
1024 us boundaries and misses the match.

This is implemented and verified on device: beacons still flow, discovery still
works, and the join still reaches an association response. It did not change the
association outcome, but the previous arrangement was wrong and is the
foundation the multiplayer CMD machinery sits on.

Note that `0x0134` is `W_BeaconCount2`, not `W_BeaconCount1`. akkit labels
`0x0134` "W_BEACONCOUNT", which is what led an earlier attempt astray.

### Transmit start interrupt and header write-back

Two things the transmit path was not doing, both of which Nintendo's wireless
library is documented to look at:

- IRQ7 is raised when a frame starts going out, before the completion
  interrupt. The model only ever raised the completion interrupt.
- Hardware stamps the outcome back into the transmit header once the frame has
  gone: `0x0001` at `TXheader[00]` and byte 5 cleared. The model never wrote
  anything back, so a guest checking whether its frame was actually sent had
  nothing to read. The write-back is mirrored into the guest's packet RAM, not
  just the model's shadow.

Both are implemented and verified not to regress anything. Neither changed the
association outcome.

### Multiplayer command transfers

The CMD/reply/ack cycle described in `ds-wifi-mp-protocol.md` is implemented in
the model:

- Firing the command slot sends the frame, reports `W_TXStat = 0x0800` and the
  completion interrupt when `W_TXStatCnt` bit 14 is set, reads the polled-client
  mask from two bytes into the frame body, and opens a reply window of
  `16 + ((10 + W_CmdReplyTime) * clients)`.
- When the window closes the MAC acknowledges by itself: transmit-start
  interrupt, `W_RFStatus` 8, `W_RXTXAddr` `0x0fc0`, and a generated frame to the
  `03:09:bf:00:00:03` flow carrying the remaining command time and the mask of
  clients that did not answer. `W_TXStat = 0x0b01` and the completion interrupt
  follow when `W_TXStatCnt` bit 13 is set.
- The command frame's own header is then stamped with `0x0001` or `0x0005` and
  the outstanding client mask at `TXheader[02]`, which is where Nintendo's
  library looks.
- IRQ12 marks the end of the whole transfer, including when no client is polled
  at all, so a transfer that has nothing to do still terminates.
- On the client side, receiving a command frame makes the station answer with no
  involvement from the guest: the armed reply slot if there is one, marked at
  `TXheader[04]` and latched from `W_TXSlotReply1` into `W_TXSlotReply2`, and
  otherwise an empty reply frame. `W_TXStat = 0x0401` and the completion
  interrupt follow when `W_TXStatCnt` bit 12 is set.

Two limits are deliberate and documented rather than hidden. Reply accounting
clears the whole outstanding mask when any reply arrives, because a reply frame
does not identify its sender and this link carries one client; telling several
clients apart needs each station's AID. And none of this is exercised on device
yet, because Mario Kart never reaches multiplayer mode — it is verified by unit
tests only, and deploying it does not disturb the association exchange.

### Emulated windows are shorter than the link's latency

The protocol waits on the peer at two points, and both are far too short for
this transport to answer in.

A command transfer's reply window is tens to a few hundred microseconds. A
host's post-beacon listening window is nine milliseconds. Emulated time on the
Brick tracks wall time one-to-one on average — measured at 1.00x over 23
seconds — but it advances in bursts: the emulator runs a chunk of guest time and
then throttles. A nine-millisecond window was observed opening and closing in
1.35 ms of real time, inside a single burst. A TCP round trip is on the order of
a millisecond, so without help a reply always misses its window and every
command transfer would report every client as having failed.

melonDS avoids this by building its multiplayer layer on timestamped primitives
that keep instances in step: sending a command and receiving the replies are
matched by timestamp, and the receive blocks until that instant's replies are
available.

The model takes the same approach in the form that suits this transport. It
calls out to the transport at both rendezvous points — when a command transfer
opens its reply window, and at the last millisecond of a post-beacon window —
and the transport stalls there, draining its receive queue until a frame arrives
or a short deadline passes. The receive thread keeps filling that queue while
the guest is held, and the peer is running the same code, so the two converge on
the same point. The hardware behaviour being preserved is that a station does
not proceed until its window is over.

The wait is bounded — about three milliseconds, in 200 us steps — because a peer
that is not answering must cost a visible stutter rather than bring the emulator
to a halt. Timeouts are logged, bounded to the first eight.

Measured on device, this stretched a nine-millisecond post-beacon window from
1.35 ms of real time to 24.8 ms. That path runs in every session. The reply
window path does not run yet, because Mario Kart never reaches multiplayer mode
and the command slot is never fired.

This is the mechanism Netplay.pak wants generally: stall where the guest
protocol already synchronises, rather than at a video-frame boundary. The
netlink shim's existing frontend-stall path is frame-based, which is why it had
to stay disabled for DraStic — the emulator's event hook is far finer-grained
than a frame and produced pause/resume storms.

### W_RFStatus must not be served to the guest yet

The transceiver reports what it is doing through `W_RFStatus` (`0x0214`),
mirrored on `W_RFPins` (`0x019c`): 1 idle, 3 transmitting, 5 an MP host awaiting
replies, 6 receiving, 8 a reply or ack transfer, 9 powered down. The model now
tracks that state across transmit and receive.

Serving it to the guest made things worse, not better. With the model answering
reads of `W_RFStatus`, Mario Kart's host read it 8106 times in a single run,
always getting the modelled idle value, and never reached the point of
transmitting a beacon at all — where DraStic's own value lets it proceed. The
state is therefore tracked but the register is not owned. Ownership has to wait
until the whole radio state is modelled, power management included, because a
partial answer is worse than none: the guest is evidently waiting for a
transition the model never makes.

### A station adopts the host's clock from its beacon

When a station receives a beacon whose BSSID matches its own network, it copies
the beacon's timestamp into its microsecond counter, corrected for the airtime
between the timestamp being stamped and the frame finishing: the stored length
times four at the 2 Mbit rate, or eight at 1 Mbit, less a fixed 76.

The model did not do this at all, so a client's clock was unrelated to the
host's and everything it scheduled against the host's beacon landed at an
arbitrary time. Implementing it moved the association request from 63 ms after
the host's beacon to 20 ms, measured on device. It did not by itself get the
association accepted, but it is correct hardware behaviour and the timing it
fixes is a precondition for anything that follows.

### The post-beacon window is not why the association is refused

A host stays awake for a window after its beacon to hear from stations trying to
connect, and over an emulated link authentication and association frames are not
synced to that window. The receive path holds the window open by adding 10 to
`W_BeaconCount2` when such a frame addressed to this station arrives, which is
what melonDS does for the same reason.

Three experiments establish that this is not the cause of the refusal:

- stretching the window in real time, by stalling at its tail, took it from
  1.35 ms to 24.8 ms and changed nothing;
- reopening the window unconditionally for frames that arrive after it has
  closed — a deliberate deviation from hardware, tried to test the hypothesis
  and reverted once it bought nothing — also changed nothing;
- the window closing is enforced by the transceiver powering down on IRQ13, and
  the model implements no power management, so this host never stops listening
  in the first place.

The host is not failing to hear the association request. It receives it,
processes it, and chooses to refuse.

## Current state

Reproduced across consecutive runs with the current build:

- The client reliably discovers and lists the host's group as `NORMAL / test1`.
- Authentication completes in both directions.
- The host answers the association request with a real association response.
- Neither process crashes; both stay in `foreground=game`.

The exchange, read from the bounded per-frame logs on both units:

```text
client -> host  auth request        sub 11  seq 0   body 0000 0100 0000
host   -> client auth response      sub 11  seq 26  body 0000 0200 0000  status 0
client -> host  association request sub  0  seq 1   cap 0021, listen 1,
                                                    SSID 32B, rates 8284
host   -> client association resp   sub  1  seq 29  cap 0021, status 0001,
                                                    AID 0000, rates 8284
client -> host  deauthentication    sub 12  seq 2   reason 3 (leaving)
```

The remaining failure is one field: the host returns **status code 1** and
**AID 0**, refusing the association. The client then leaves and shows
`An error has occurred.` The host stays at `CURRENT RACERS 1/8`.

The rejection is a decision, not a timeout: the host queues its response 2.6 ms
after consuming the request, and consults no register in between. Earlier it
sat idle for 1.9 seconds and deauthenticated with reason 1; that was the
duplicate-sequence-number fault, and it is gone.

### What has been ruled out

Each of these was a live hypothesis and each is now disproved by evidence, so
they should not be revisited without new information:

- **Frame delivery.** The frame is read back out of DraStic's packet RAM after
  storage and matches the transmitted bytes exactly, including the RX header
  (`status=8010 rxlen=0044`), the cropped FCS and the four-byte padding.
- **The RX header or classification.** The association request is delivered with
  the same status word as the authentication frame the host does act on.
- **The request's contents.** The 32-byte SSID element carries the game id
  `25000000` followed by the 16-bit session token from the host's own current
  beacon (`e4f1` against a beacon advertising `e4f11c01`), then the client's
  UTF-16 name. The client is echoing the host's live beacon correctly, which
  also proves beacons are not being delivered stale.
- **W_TXSTAT.** The model reports `0x1001` after a transmit from the slot at
  `0x00a4`. melonDS produces exactly `0x0001 | ((slot-1) << 12)` for the LOC
  slots, so this value is right.
- **Shared MAC addresses.** The two consoles now have distinct firmware MACs
  and no address translation happens; the rejection is unchanged.
- **The post-beacon listening window.** Stretching it in real time, and
  reopening it for late frames, both left the refusal unchanged.
- **Clock synchronisation alone.** Adopting the host's clock from its beacon
  brought the request 43 ms closer to the window without changing the answer.
- **A stale session token.** The host advertises a token that is constant for
  the session, not per beacon, and the association request echoes it exactly:
  `12e6` in the beacons and `12e6` in the request, in the same run.
- **Missing acknowledgement frames.** melonDS generates no 802.11
  acknowledgements at all — its only ack is the multiplayer command ack — and it
  runs retail local multiplayer, so they are not required for association.
- **The unraised interrupts.** The host enables W_IE bits 2 to 5 and the model
  raises none of them, but melonDS does not raise them either, so they cannot be
  what the library is waiting for.
- **The RX status word.** melonDS builds `0x0010`, plus `0x8000` for a matching
  BSSID, plus `0x0001` only for beacons. An authentication or association frame
  from the host's own network is therefore `0x8010`, which is exactly what the
  model produces.

The build deployed at this historical point included:

- static receive, transmit, beacon and acknowledgement scratch buffers;
- four-byte RX length alignment and padding;
- runtime-located direct packet RAM, with a low-half dispatcher fallback;
- armed transmit slots, independent of the W_TXREQ bit assignment;
- MAC-supplied sequence numbers on every transmitted frame;
- the beacon driven by `W_BeaconCount1` through the beacon interrupt, with its
  three sources, the `W_USCompareCnt` gate, and the `W_BeaconCount2` countdown
  raising IRQ13;
- RX-start, RX-complete, transmit-start and multiplayer-end signalling;
- the transmit header write-back hardware performs after a frame goes out;
- transceiver status tracked across transmit and receive, but not served to the
  guest;
- a station adopting the host's clock from a beacon on its own network;
- model-owned microsecond counter and beacon counters;
- the multiplayer command, reply and acknowledgement cycle;
- a bounded stall at the protocol's rendezvous points, driven from the model and
  implemented by the transport;
- distinct console MAC addresses, with the wire-address translation dormant and
  self-enabling only on a detected collision;
- bounded per-frame logging with management-frame bodies, and a bounded
  read-back of stored frames from guest packet RAM;
- an MMIO trace that re-arms its budget at the first non-beacon frame.

## The failure is not Mario Kart specific

Asphalt GT2 was run as a second title to separate a game-logic problem from a
model problem. It reproduces the failure exactly.

Discovery works: the client's `JOIN GAME` list shows the host's session as
`1 : DR DRASTIC ( SINGLE RACE )`, named from the firmware username. Selecting
it produces the same five-frame exchange as Mario Kart, with the same frame
sizes (46, 46, 82, 50, 42) and the same outcome. The association response body
is byte-identical to Mario Kart's:

```text
2100 0100 0000 01 02 8284     capability 0x0021, status 1, AID 0
```

The client then deauthenticates with reason 3 and shows `COMMUNICATION ERROR`.

Two unrelated titles — a Nintendo first-party racer and a Gameloft port — refuse
the association at the same step with the same status code. The cause is
therefore something Nintendo's shared wireless library requires and the model
does not provide, not either game's own logic. Any fix should unblock both, and
either title can be used to debug it.

### Advance Wars Dual Strike is not usable for this

It was tried first and abandoned. A fresh save forces the prologue and drops
straight into the Jake's Trial tutorial mission with no path to the main menu,
and its in-game menu is touch-driven: START only raises a minimap overlay.
DevBridge's input vocabulary is buttons and chords (`BTN`) with no touch or
stylus command, so the game cannot be driven to a versus lobby at all. Any DS
title whose multiplayer setup lives behind touch controls is out of reach until
DevBridge grows a touch command; Mario Kart and Asphalt being fully
button-navigable is a lucky property, not the norm.

### Driving Asphalt GT2

No tutorial gate. From a cold launch: about 26 s to the language screen, `A`,
`A`, `START` reaches the main menu with `ARCADE` selected. The sidebar order
upward from `ARCADE` is `EVOLUTION`, `MULTIPLAYER`, `TIPS`, `OPTIONS`, so `UP`
twice selects `MULTIPLAYER`. Inside it, `HOST GAME` is the default and `UP`
selects `JOIN GAME`. The host then picks mode, category, car and track with
four `A` presses and begins beaconing.

Menu transitions are animated and drop presses that arrive faster than about
1.5 s apart, so any macro for this game needs generous waits and verification
screenshots between steps.

## Tried and reverted: gating IRQ14 on W_US_COMPARE bit 0

`W_US_COMPARE` bit 0 is a write-only control rather than part of the compare
value: on hardware it blocks the `W_BEACONCOUNT` interrupt (IRQ14) until
`W_US_COUNT` reaches the compare. Mario Kart's host uses it. Its startup
sequence is:

```text
W_USCOMPARE   = 0x0000_0000_002F_D001   (bit 0 set)
W_USCOUNTERCNT = 1                       (start the microsecond counter)
W_USCOMPARECNT = 1                       (enable the compare)
W_TXBUF_BEACON = 0x811c                  (arm the beacon)
```

The model was masking `W_USCOMPARE0` writes with `0xfc00`, which is right for
the compare value but silently discarded the gate bit.

Implementing the gate — suppressing the model's beacon interrupt until the
compare matches — reproducibly broke discovery across two runs: the client
received beacons and stored them in packet RAM, the host's beacon content was
correct, but no room appeared in the list.

The reason is that the model has no `W_BEACONCOUNT`. It synthesises IRQ14 on
beacon *transmission*, which is not the source the gate is specified against.
Gating a synthesised source suppressed interrupts the guest still needed during
the first ~3.1 seconds, which is exactly the scan window.

The gate bit is still captured from the write and cleared on a compare match, so
it is not discarded, but nothing consumes it. It belongs with a real
`W_BEACONCOUNT` implementation. `W_BEACONCOUNT` is at `0x0134`; the host reads
it and writes back the previous value plus 11 on every beacon period (~192 ms),
which suggests software maintains it and the hardware counts it down.

## Open question: the periodic W_TXREQ bit 1

Every ~74 ms the host writes `W_TXREQ_RESET=0xffff` then `W_TXREQ_SET=0x0002`.
melonDS maps bit 1 to the multiplayer CMD slot, and W_TXREQ_READ reads back
`0x000a` (bits 1 and 3). No CMD buffer is ever programmed: a scan of every
register write with bit 15 set across a whole run finds no slot pointer outside
`0x0080` (beacon) and `0x00a4`, and the writes at `0x024c`/`0x024e` are
initialisation resets, not slot registers.

So these requests appear inert, which fits a host that has not entered
multiplayer mode yet. Consistently, taking the beacon interrupt clears the
transmit request bits with `& 0xfff2`, which preserves bit 1 while clearing 0, 2
and 3 — the command request survives each beacon deliberately. The model does
this. Sending a beacon on that request was tried and reverted:
it changed no outcome and one run behaved erratically.

Related: the documented W_TXREQ bit map does not match this game. akkit and
melonDS both pair bit 3 with LOC3 at `0x00a8`, but Mario Kart sets bit 3 and
then programs `0x00a4`. The armed-slot scheme sidesteps this and works, so the
mapping does not need resolving unless something later depends on it.

## Historical next steps (superseded 2026-08-31)

1. Find why the host's association handler returns status 1. Every input the
   handler can observe has been checked against melonDS and matches: the frame
   bytes in packet RAM, the RX header and its status word, the session token,
   the sequence numbers, the interrupts the host has enabled, and the clock.
   Both Mario Kart and Asphalt GT2 refuse identically, so it is a requirement of
   Nintendo's shared wireless library rather than either game.

   Since the refusal is not explained by what the library is told about the
   request, the two places left are the state it accumulated earlier in the
   session, and the possibility that the fault is not in the Wi-Fi model at all
   but in how interrupts are injected into DraStic's ARM7 while that library is
   running. The model writes DraStic's IF register and `active_interrupts`
   directly from the event thread; association is the first thing the library
   does that is stateful across several interrupts, and a race there would look
   exactly like this — every input correct, the decision wrong.

   A cheap first experiment: narrow injection to RX-complete alone and see
   whether the *failure mode* changes. A different failure is information; this
   one has stopped yielding any.

2. Model power management. It is the largest block of hardware behaviour still
   entirely absent: `W_PowerState`, `W_PowerForce`, `W_TRXPower`,
   `W_PowerDownCtrl` and the mode bits in `W_ModeWEP`, the transceiver being
   turned off by IRQ13 and woken by IRQ15, and the RF wakeup interrupt. Serving
   `W_RFStatus` to the guest depends on it: the state machine is implemented and
   tracked, but answering reads with a partial radio state made the host spin.

3. Exercise the multiplayer command cycle once the association completes. It is
   implemented and unit-tested but has never run on device. Reply accounting
   currently clears the whole outstanding mask on any reply, which is correct
   for one client; more than one needs each station's AID.

4. Remove the temporary breadcrumbs once the join is stable: the per-stage
   receive trace, the `packet-store` and `mirror` probes, the stored-frame
   read-back, the frame and frame-body logging, and the default verbose MMIO
   trace. Keep the static scratch buffers, RX alignment, the direct packet RAM
   pointer, armed transmit slots, transmit sequence numbers, the beacon counters
   and their interrupt, the transmit header write-back, the clock adopted from
   beacons, the peer wait, and microsecond-counter ownership. Consider promoting
   the packet RAM offset `0x17070` to a named constant beside
   `BRICK_ARM7_IO_OFFSET` once a second build confirms it.

After each source change, verify with:

```sh
make -C drastic-wifi test
make nds-wifi-hooks
git diff --check
```

Then stop both DraStic processes through DevBridge, deploy the Brick artifact to
the active `libdrastic_wifi_diag.so` path on both units, and run the cold macro.
Discovery is restored only when `client-room-visible-client.png` shows the host
name `test1` populated; a highlighted but blank first row is not a discovered
room. The exchange is intact when the host's `NDS.txt` holds five `frame dir=`
lines ending in an association response; anything short of that is a run where
the macro's navigation slipped, which happens, and is worth repeating before
reading anything into it. If neither log contains any `frame dir=` line and the
room list is empty, check `/tmp/drastic-wifi-session` on both units before
suspecting the model.

## References used

- <https://www.akkit.org/info/dswifi.htm>
- <https://github.com/devkitPro/dswifi>
- <https://ndsdoc.gbadev.net/ds_wifi_io_map.html>
- <https://ndsdoc.gbadev.net/ds_wifi_general_regs.html>
- <https://blocksds.skylyrac.net/dswifi/index.html>

The old dswifi receive parser was also inspected locally. It reads the frame
length from RX-header halfword 4 (`+8` bytes), treats the frame body as starting
after six halfwords, and advances the circular buffer by the padded body length
plus the 12-byte RX header. That behavior agrees with the cursor movement seen
in the Brick MMIO trace.

## 2026-08-30: stable Asphalt lobby and split peer-wait semantics

The first 50 ms MP-reply barrier made Asphalt's joined player survive much
longer, but the client still failed after roughly a minute. The apparent
"client command waits" in its log were not command transfers: the same peer
wait callback was also called when every post-beacon listening window closed.
The transport had accidentally applied its new reply-only 50 ms policy there,
stalling both consoles while waiting for a reply that cannot exist in that
context.

`dwifi_transport_await_for_peer()` now selects its behavior from
`model->mp_cmd_active`:

- active MP command: up to 50 ms, and only a station reply closes the barrier;
- post-beacon window: up to 3 ms, and any delivered frame closes the wait.

This build passed all hook/model/adapter tests and was deployed to both Bricks.
In session `asphalt-cold-join-20260830-220232`, player 2 remained visible on
the host and the client remained on `WAITING FOR CONNECTION...` at 20 seconds.
The nondestructive follow-up session `asphalt-observe-20260830-220500` showed
the same healthy states at both approximately 60 seconds and two minutes. The
new logs contained no `peer wait timed out` messages.

The screenshot tooling now supports this workflow directly:

- `drastic-wifi/devbridge/asphalt-observe.test` records later checkpoints
  without sending input;
- `classify-asphalt-session.sh` classifies every `*-host.png` and
  `*-client.png` in a session, rather than only the original four filenames.

Control testing clarified the next boundary. The lobby explicitly instructs
the host to press `A` to start; when that action removes player 2 or errors the
client, it is a failed start transition rather than a kick-player action. Host
`START` did nothing and left the stable lobby intact. Client `START` eventually produced the exact
recorded `communication-error` screen while the host retained player 2. The
next development target is therefore the lobby-ready/game-start transition.
Automated tests should press host `A` only after confirming the stable connected
lobby, because `A` is the required start action and the boundary under study.

### Populated MP replies share the ACK destination

Sparse MP telemetry exposed a single 140-byte client transmission after the
initial empty replies. It has destination `03:09:bf:00:00:03`, like a host ACK,
but address 3 is `03:09:bf:00:00:10`, which identifies it as a populated MP
reply. The transport previously checked the destination first, entered its ACK
pairing path, and discarded the frame on the host because there was no incoming
candidate command. melonDS confirms that reply classification has priority and
is based on address 3.

The transport now checks address 3 before command/ACK destination roles in both
its busy-frame queue and diagnostics. The broader experimental rule treating
all host-side incoming ACK-flow frames as replies was removed. A model test now
covers a populated reply carrying both address patterns.

On-device session `asphalt-cold-join-20260830-224642` retained player 2. After
host `A`, both consoles remained connected for another 30 seconds; unlike the
broader experimental build, the client did not enter communication error.
Following with client `A` also left both connected. Neither action advanced the
lobby, so the remaining question is what game-visible ready/start condition is
missing, not basic MP reply transport.

### Reply-slot latch and initially misclassified transition frame

The client MMIO trace shows reply packets being prepared at slot value `8248`
with lengths 34 and 70. The model was missing the hardware latch controlled by
`W_RXCnt` bit 7 and transmitted directly from Reply1. It now mirrors melonDS:
bit 7 copies Reply1 to Reply2 and clears Reply1, while command reception latches
the newly staged Reply1 value and transmits from Reply2. Tests remain green.

This made the client's 140-byte transmission reproducible. At this point it was
misidentified as ordinary ACK-flow traffic because the frame bytes did not
retain the semantic sideband used by melonDS. The later investigation below
established that it is staged in the reply slot. The frame-only transport now
canonicalizes address 3 on its transmitted copy to preserve reply semantics,
and the duration gate rejects it because it cannot fit Asphalt's advertised
client window. The stable result is therefore an empty reply, not asynchronous
delivery of this oversized frame.

Relevant sessions are `asphalt-cold-join-20260830-234119` (latch behavior) and
`asphalt-cold-join-20260830-234549` plus
`asphalt-start-20260830-234820` (ACK-flow delivery and transition error).

### 2026-08-31: reply semantics, duration gate, and start-slot trace

After Wi-Fi was restored on `.144`, the NetLink session was recreated with
`.180` as host and `.144` as client on port 55437. Empty room searches during
the outage were incorrectly classified as `room-visible`; the visual baseline
set now distinguishes `searching-no-room`, `room-visible`,
`waiting-connected`, and `communication-error`.

The apparent 140-byte client "command" is produced from the hardware reply
slot. melonDS retains its reply semantic out of band through `MP_SendReply`,
whereas this frame-only transport initially lost that distinction. Address 3
was canonicalized on the transmitted copy to preserve reply semantics.

More importantly, melonDS rejects a staged reply when its airtime exceeds the
command's client-time field. Asphalt advertises `0x0166` (358 us), while its
staged 140-byte transport frame needs roughly 700 us. The model now applies the
same duration gate and sends the valid empty reply instead. Together with
immediate accounting for a reply already queued while emulated RX is busy,
this restored the stable lobby in session
`asphalt-cold-join-20260831-082812`.

The documented host `A` start action still makes the client report a
communication error while the host retains player 2. New packet-sync logging
includes the register offset. It shows the repeating 34-byte command correctly
uses `W_TXSlotCmd` (`0x0090`). A packet at `0x00a4`, slot `0x84ac`, is setup
traffic and did not newly appear after `A` in the instrumented run. The next
target is therefore command completion and IRQ-visible state across the start
transition, not a missed transmit-slot mapping.
