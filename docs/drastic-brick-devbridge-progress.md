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
| Host | `192.168.0.179` (was `.180`) | `1c000c66050607724dd` |
| Client | `192.168.0.143` (was `.144`) | `2c000c81b5c4078245d` |

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

Host is `role=host`, `port=55437`, `peer=192.168.0.143`; client is
`role=client`, `port=55437`, `peer=192.168.0.179`. This is in `/tmp`, so a
reboot loses it. When it is missing the hook logs `transport=netlink
status=disabled`, both games run normally, and the client's room list stays
empty with no frames in either log. Restore it with:

```sh
adb -s 1c000c66050607724dd shell \
  "printf 'role=host\nport=55437\npeer=192.168.0.143\n' > /tmp/drastic-wifi-session"
```

Each run writes screenshots, command results, and `events.jsonl` beneath
`devbridge-sessions/`. The first run with working discovery is
`devbridge-sessions/drastic-cold-join-20260829-180211`; the first run that also
reached the host's waiting room without either process crashing is
`devbridge-sessions/drastic-cold-join-20260829-181352`.

Per-run emulator logs live on each device and are the primary evidence:

```text
/mnt/SDCARD/.userdata/tg5040/logs/NDS-wifi/NDS-wifi-<timestamp>.txt  # hook trace
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

### 2026-09-01: restore command -> reply -> ACK delivery order

Sparse host command-boundary telemetry initially made the lobby look healthy,
but client-side automatic-reply telemetry showed that no current MP commands
were reaching the client's Wi-Fi model. The host was reporting success only
because `mp_reply_seen` remained latched from an earlier reply.

The bounded transport inbox was withholding a command until its closing ACK
arrived. That reverses the protocol dependency: the client must receive the
command and reply before the host can generate the ACK. Commands are now
published immediately, with their ACK retained separately as the closing half
of the bounded generation. Session `asphalt-cold-join-20260901-183528` retained
player 2 after 20 seconds. The client stderr trace showed continuous, ordered
command/default-reply/ACK traffic at sparse counts through 1024 cycles.

Live register tracing also confirmed that Asphalt did not write nominal
Reply1/Reply2 registers in this session. The observed slot `0x8248` traffic was
through LOC2, with frame lengths 34, 70, and 30 bytes. Reply-slot packet-RAM
synchronization remains supported defensively, but it was not responsible for
the behavior change above. The next validation target is the host Start action
on top of the corrected transport ordering.

### 2026-09-01 (later): deployment gap, reverted transport reorder, and an
### exonerated start action

Three findings from this session change what the previous entry concluded.

**The last three sessions did not run the library they were credited to.**
`adb push` preserves the source file's mtime, so the deployed
`libdrastic_wifi_diag.so` mtime names the build it came from. The Bricks were
carrying a build stamped 18:16, while the working tree's artifact was stamped
18:35. Sessions `asphalt-cold-join-20260901-182355`, `-182938` and `-183528`
therefore all ran the 18:16 library, and the entry above credits the
`transport.c` reorder (edited 18:34) with a result produced without it. When
reading any past session, check the deployed mtime against
`drastic-wifi/build-brick/libdrastic_wifi.so`; matching md5 is the only
reliable confirmation that a change was actually on the device.

**The immediate-command-publish reorder is a regression and has been
reverted.** Deploying the tree build for the first time produced
`communication-error` on the client at `post-join`, against `player-2-present`
on the host, and a client trace truncated to 419 KB where a healthy run writes
about 1.65 MB. Reverting only `transport.c` and rebuilding, with the telemetry
changes left in place, restored the healthy join in
`asphalt-cold-join-20260901-190017`: host `player-2-present`, client
`waiting-connected`. The change is preserved as
`drastic-wifi/devbridge/rejected-transport-immediate-publish.patch`.

Its likely defect is generation splicing, which the code it replaced existed to
prevent. Publishing a command immediately leaves the next command overwriting
`next_mp_cmd` while `next_mp_ack` still holds the previous generation's ACK, and
the host branch also lost the `!candidate_mp_cmd_length` guard, sending every
host-side ACK-flow frame to `pending_other`. Any second attempt should keep
per-generation pairing while still releasing the command ahead of its ACK.

Note that `peer wait timed out` is not a regression signal. The healthy run
logged eight of them and the failing run five. Use the screen classification and
the client trace size instead.

**The host `A` start action is exonerated.** The documented failure — host `A`
sends the client to `COMMUNICATION ERROR` while the host retains player 2 — is
an artifact of `asphalt-observe-start.test` being run minutes after the join. A
no-input control settles it. Running `asphalt-cold-join.test` and then
`asphalt-observe.test`, which sends no input at all, reproduced the same
outcome: client `communication-error` at the sixty-second checkpoint and host
`player-2-dropped` at two minutes, in session
`asphalt-observe-20260901-190702`. The lobby does not survive about a minute on
its own. Pressing `A` was never the trigger.

That control run also rules out the transport layer. It logged no netlink
disconnect at all, and zero client `peer wait timed out` messages, while both
consoles continued exchanging command (`kind=1`), ACK (`kind=2`) and reply
(`kind=3`) frames through `count=2048` — well past the point where the client's
screen already read `COMMUNICATION ERROR`. The MP cycle keeps running while the
game has already given up on it.

Those 40-byte `kind=3` frames are **not** populated replies. The model's default
reply is 28 bytes plus the 12-byte radio header, which is exactly 40.

**Note on validity.** The runs in this section were disturbed — one Brick was
being used by hand while they executed. Their conclusions are void and were
re-established on the ad hoc link below. The static findings above (the
deployment gap, and the reply-slot source in `send_mp_reply`) do not depend on
any run and stand.

### 2026-09-01 (later still): ad hoc link, and the client never stages a reply

Testing moved to an ad hoc network created by Netplay.pak, with
`2c000c81b5c4078245d` as the access point on `wlan1` = `10.0.0.1` and
`1c000c66050607724dd` joined at `10.0.0.20`. The Bricks measure 0% loss and
2.0 ms minimum / 3.0 ms average / 8.6 ms maximum round trip between them, which
is both faster and far less jittery than routing through the house AP. Restore
the session files for this topology with:

```sh
adb -s 1c000c66050607724dd shell \
  "printf 'role=host\nport=55437\npeer=10.0.0.1\n' > /tmp/drastic-wifi-session"
adb -s 2c000c81b5c4078245d shell \
  "printf 'role=client\nport=55437\npeer=10.0.0.20\n' > /tmp/drastic-wifi-session"
```

Both Bricks carry only busybox `nc`, which cannot listen, so the link is best
verified by launching the game and reading the handshake: the host should log
`hosting on port 55437` then `connected as host`, and the client `connected as
client`. It does.

The link change alters the failure mode. In `asphalt-cold-join-20260901-201029`
the join is clean — host `player-2-present`, client `waiting-connected`, zero
`peer lost` events, zero client peer-wait timeouts. The no-input control
`asphalt-observe-20260901-201258` then shows, with no input sent at all, that
the client no longer reaches `COMMUNICATION ERROR`: it sits on
`WAITING FOR CONNECTION......` for the full two minutes with `W_IE` healthy at
`e0ff` throughout. The host, however, shows `2: (EMPTY)` and
`WAITING FOR PLAYERS...`. The host expels the client while the client is still
patiently waiting.

This re-confirms, on undisturbed runs, that pressing host `A` is not the
trigger. The lobby degrades on its own.

**The client's guest never stages a multiplayer reply.** Across the whole run
it writes `REG_TX_SLOT_REPLY1` (`0x0094`) and `REG_TX_SLOT_REPLY2` (`0x0098`)
exactly zero times, and all 34 sampled replies report `addr=0000 len=0`. Every
reply-flow frame on the wire, client TX and host RX alike, is 40 bytes: the
empty default.

The earlier LOC2 theory is wrong and should not be pursued. `0x00a4` is written
only twice in an entire run, against thousands of MP command cycles, so it is
setup traffic as originally recorded and not a per-cycle reply path. Routing it
into `send_mp_reply` would not help.

What matters instead is that **the model fabricates the client's
participation.** `send_mp_reply()` synthesises a 28-byte default whenever no
slot is staged, so the host sees an answer to every poll and holds `fail=0000`
even though the client's software has produced nothing. Real hardware would not
transmit, and the host's fail mask would set. The hardware layer therefore looks
perfectly healthy while the game layer starves, which is exactly the observed
split: `mask=0002 fail=0000` on the host, and an emptied player slot.

### The model was fabricating the client's participation (confirmed)

`send_mp_reply()` synthesises a 28-byte empty reply whenever the guest has
staged nothing. A diagnostic now suppresses that, gated by
`DRASTIC_WIFI_NO_DEFAULT_REPLY`, which the packaged launcher sets from
`drastic/no_default_reply.enabled` in the same style as `wifi_model.enabled`:

```sh
adb -s <serial> shell "touch /mnt/SDCARD/Emus/tg5040/NDS.pak/drastic/no_default_reply.enabled"   # on
adb -s <serial> shell "rm -f /mnt/SDCARD/Emus/tg5040/NDS.pak/drastic/no_default_reply.enabled"   # off
```

Enabling it on the client alone, in `asphalt-cold-join-20260901-202107`, flips
the host's accounting immediately:

| | default synthesised | suppressed |
| --- | --- | --- |
| client replies | 34 x `mp-reply-default` | 14 x `mp-reply-suppressed` |
| host fail mask | `fail=0000` throughout | `fail=0002` on 9 of 10 commands |

The host's fail accounting is therefore correct, and was only ever clear because
the model answered on the client's behalf. The join still completes with
suppression on, so this is safe to leave enabled while investigating.

### Why the guest can never stage a reply in time

In `dwifi_model_receive_frame()` the client's command path raises
`WIFI_IRQ_RX_COMPLETE` and then calls `send_mp_reply()` **synchronously, in the
same C call**:

```c
dwifi_model_raise_irq(model, WIFI_IRQ_RX_COMPLETE);
...
if (mac_equal(frame + 12 + 4, cmd_flow))
        send_mp_reply(model, client_time);
```

`dwifi_model_raise_irq()` only sets IF bits and pokes DraStic's interrupt state.
The guest ARM7 has not executed a single instruction by the time the reply slot
is read, so `REG_TX_SLOT_REPLY1` is guaranteed empty at that moment. No reactive
staging can ever win this race.

That is a real structural defect, but it is probably not the whole story.
Hardware double-buffers REPLY1/REPLY2 precisely so software can stage
*proactively*, independent of any command, with the `W_RXCnt` bit-7 latch
picking it up at command time. This guest writes `0x0094` and `0x0098` zero
times across an entire run, so it is not staging late — it is not staging at
all, which points at the client's library not believing it is an associated MP
station.

### Fixed: the reply is no longer sent from inside the receiving call

`dwifi_model_receive_frame()` no longer calls `send_mp_reply()` inline. A command
now schedules the reply — `mp_reply_due`, `mp_reply_client_time`,
`mp_reply_due_us` — and `dwifi_model_advance_us()` services it **at the top of a
later call**, deliberately not inside its tick loop. The loop walks the whole
elapsed delta, so a deadline set during it could still fire before the guest ran
and would reproduce the very behaviour being removed. Servicing at the top of a
later call guarantees one event-hook boundary, which is where the guest ARM7
executes. The turnaround is `DWIFI_MP_REPLY_TURNAROUND_US` (16 us), short enough
to stay well inside the advertised client window.

`test_reply_staged_after_command_is_sent()` covers it: a reply staged only after
the command was received still reaches the air, which was impossible before.
The two existing reply tests now assert nothing is sent from the receiving call.

Verified on device in `asphalt-cold-join-20260901-203216` — the join is
unaffected and still healthy.

### Why the guest still does not stage: the ownership boundary eats it

The deferral was necessary but not sufficient. The guest still writes
`0x0094`/`0x0098` zero times. Parsing every register access in that session's
client trace explains why, and rules out the obvious suspects:

| register | observed |
| --- | --- |
| `W_AID_LOW` `0x0028` | written `0001` |
| `W_AID_FULL` `0x002a` | written `0001` |
| `W_TXSLOT_REPLY1` `0x0094` | **read 445 times, always 0, never written** |
| `W_TXSLOT_REPLY2` `0x0098` | **read 668 times, always 0, never written** |
| `W_RXCnt` `0x0030` | only ever `8000`/`8001`; bit 7 never set |
| `W_TXREQ_SET` `0x00ae` | `0002`, `0008`, `000d` |

So the client **is** a properly associated MP station: it parsed the
association response and holds AID 1, which is exactly the station the host's
`mask=0002` polls. And it is actively in a reply loop — it polls both reply-slot
registers over a thousand times. It is not idle or confused about its role.

Every one of those reads returns zero, and they return zero because **the model
answers them itself**. `dwifi_model_owns_read()` claims the whole range
`REG_RX_CNT` (`0x0030`) through `REG_TX_STAT` (`0x00b8`), which contains both
reply slots, and serves them from a shadow register file the guest has never
been able to write.

That is the shadow-model boundary described in the architecture notes failing at
its narrowest point. The hook patches only the 16-bit MMIO handlers; the 8-bit
and 32-bit stubs are deliberately not hooked. If the guest stages its reply
through one of those, the model never sees the write, its shadow stays zero, and
the owned read then hands that zero straight back — so from the guest's side its
staging vanishes the instant it is made, and the reply loop can never advance.

### Ownership of the reply-slot reads: tested and ruled out

The theory was that the guest stages its reply through the unhooked 8-bit or
32-bit store path, leaving the model's shadow at zero, and that the model then
hands that zero back because `dwifi_model_owns_read()` claims the whole
`0x0030`-`0x00b8` range. Narrowing that ownership so `0x0094`/`0x0098` fall
through to DraStic disproves it. In `asphalt-cold-join-20260901-205348`
**DraStic answers `0000` as well**, across 566 and 849 reads. Neither the model's
shadow nor DraStic's own register file holds anything, so the guest is not
staging through any store width. The change has been reverted: it is harmless —
the join stays healthy and DraStic does not abort — but it buys nothing.

### Trace coverage, and a methodology note

Two earlier readings in this section were wrong for the same reason and are
corrected here.

First, `OPEN_GAME` does not relaunch DraStic if it is already running. Two runs
were scored against a library that had never been loaded, because no safe-quit
preceded them. **Always safe-quit before a cold join, and confirm a new
`NDS-wifi-<timestamp>.txt` appeared before reading anything into a run.** The
absence of a new trace file is the tell. This is the same failure as the
deployment gap at the top of this section: a stale artifact scored as if fresh.

Second, the general MMIO trace fills during startup, well before multiplayer
begins, so slot-access counts were identical across runs with different outcomes
— they measured where the buffer ran out, not behaviour. The transmit-slot block
`0x0090`-`0x00b0` now has its own budget beyond `DWIFI_TRACE_LIMIT`
(`DWIFI_SLOT_TRACE_LIMIT`, 24576), which costs about 1000 extra records and
covers the whole run.

With that coverage, the picture is solid. Multiplayer begins at line 4903 of
22176, and **all 824 `0x0094` reads and all 1237 `0x0098` reads fall after it**.
During multiplayer the client polls both reply slots roughly two thousand times,
always reads zero, and never writes either. The only writes anywhere in the slot
block are LOC2 twice, `W_TXREQ_RESET`=`ffff` 97 times, and `W_TXREQ_SET` with
`0002`, `0008` and `000d` — never a reply-slot bit.

### RX header compared against melonDS: it matches

`Wifi.cpp` `FinishRX()` was read directly rather than through a summary — two
web summaries disagreed about the field offsets, and both were partly wrong.
The verbatim source settles it. Note `IncrementRXAddr(headeraddr, 4)`, which
skips the `+4` slot.

| offset | melonDS | this model |
| --- | --- | --- |
| `+0` | `rxflags` | `receive_flags`, identical formula |
| `+2` | `0x0040` | `0x0040` |
| `+4` | skipped | `0` |
| `+6` | TX rate | TX rate |
| `+8` | frame length | length rounded up to 4 |
| `+10` | `0x4080` | `0x4080` |

The flag encoding is identical too: base `0x0010`, `0x8000` on BSSID match,
low nibble `0xC` command, `0xD` ack, `0xE`/`0xF` reply. `classify_rx_frame()` is
a faithful port, and `SendMPReply()` matches ours structurally — same
REPLY1-to-REPLY2 latch, same duration gate. **The RX header is not the fault**,
which retires that hypothesis.

The comparison did expose three divergences, all of them in what is delivered
rather than how it is described. Each makes this model deliver *more* than
hardware would:

1. **Duplicate commands.** melonDS keeps `MPLastSeqno`; a CMD frame repeating it
   sets `cmd_dupe`, which skips the RX header write and `SetIRQ(0)` while still
   answering the reply. Implemented here as `finish_duplicate_command()`, with
   `test_duplicate_command_is_not_delivered()` covering delivery suppression,
   cursor stability, IRQ suppression and recovery on a fresh sequence number.
2. **RX filtering — still missing.** melonDS applies `W_RXFilter` and
   `W_RXFilter2` throughout `FinishRX()` and returns outright on many
   type/subtype/ToDS-FromDS combinations. This model implements none of it and
   delivers everything.
3. **The reply AID gate.** melonDS sends a real reply only when
   `W_AIDLow && (clientmask & (1 << W_AIDLow))`, reading clientmask from body
   offset 26, and otherwise calls `Platform::MP_SendReply(nullptr, 0, ...)` — a
   zero-length sideband reply, never a fabricated frame on the air. This model
   replies unconditionally with a synthesised 28-byte frame. That is the
   fabrication identified earlier, now confirmed against the reference.

`cmd_dupe` alone did not change the guest: in
`asphalt-cold-join-20260901-211632` the join stays healthy and the client still
reads `0x0094`/`0x0098` (356 and 534 times) without ever writing either, and
still never sets `W_RXCnt` bit 7. The read counts fell from 824/1237, which is
consistent with the filter actually suppressing deliveries.

### Console nicknames, and a sharper success criterion

Both consoles shipped with DraStic's default firmware nickname, so every lobby
showed two players called `DR DRASTIC`. It is a plain config setting, not a
firmware patch:

```text
/mnt/SDCARD/Emus/tg5040/NDS.pak/drastic/config/drastic.cfg
firmware.username = ...
```

Set with DraStic stopped (it rewrites the file on exit); the originals are kept
alongside as `drastic.cfg.pre-nickname`:

```sh
adb -s <serial> shell \
  "sed -i 's/^firmware.username = .*/firmware.username = Test1/' \
   /mnt/SDCARD/Emus/tg5040/NDS.pak/drastic/config/drastic.cfg"
```

The host `1c000c66050607724dd` is now `Test1` and the client
`2c000c81b5c4078245d` is `Test2`.

This turned out to be diagnostic rather than cosmetic. In
`asphalt-cold-join-20260901-212046` the client's room list correctly shows
`1 : TEST1 ( SINGLE RACE )`, so the host's new name reaches the client in the
beacon. But the host's own lobby reads:

```text
1: TEST1
2: DR DRASTIC
```

Slot 2 is not the client's name. Neither `Asphalt GT2.sav` contains any name
string, and both saves are untouched since 29 August, so it is not a stale
save — the host is filling an occupied but unidentified slot with a default.
**The client's identity never reaches the host's game layer**, which is
independent, on-screen confirmation of the fault this section has been chasing,
visible without reading a single trace.

It also gives a much better success criterion than the current baselines. A
`player-2-present` match only says the lobby *looks* occupied, and this section
has established that can be true while nothing real has been exchanged. **A
genuinely completed join should show `2: TEST2`.** Treat that, not slot
occupancy, as the target.

One tooling consequence: the host baselines were captured with `DR DRASTIC` in
slot 1. They still classify correctly, but the distance rose from 0.0 to 3.248
against a margin of 3.617, leaving little headroom. Recapture
`player-2-present` and `player-2-dropped` with the new names before relying on
them.

## Correction: melonDS implements LAN play, and this document was wrong about it

Earlier sections of this document assert that melonDS's multiplayer is
co-located shared-memory IPC, and build an argument on that: that its
synchronisation is affordable only because both instances run on one machine
under one scheduler, and that two free-running clocks across a real network is
an inherent, unsolved problem particular to this project. **That is wrong.**

`melonDS/src/net/LAN.cpp` is a full LAN implementation over ENet — one emulated
DS per participating machine — implementing the same `MPInterface` as the local
path: `SendCmd`, `SendReply`, `SendAck`, `RecvHostPacket`, `RecvReplies`. It
faces exactly this project's problem and solves it.

It also **blocks on the network per transfer**, as this project does.
`RecvHostPacket()` uses the blocking receive, `RecvReplies()` loops on
`ProcessLAN(2)`, and `MPInterface::RecvTimeout` defaults to **25 ms** — the same
order as the 50 ms barrier here. Per-transfer blocking is therefore not the
error, and the conclusion drawn elsewhere in this document that the barrier is
fundamentally the wrong shape does not follow.

Four differences do matter:

1. **The transport is unreliable and unsequenced.** Packets are sent with
   `ENET_PACKET_FLAG_UNSEQUENCED` — no retransmission, no ordering guarantee, no
   head-of-line blocking. Excess frames are simply lost. **This project uses
   TCP**, which cannot drop anything: when the host out-produces the client,
   every frame is retained and delivered late. The bounded inbox here, and the
   generation queue that replaced it, are a hand-built and much worse
   approximation of the discard that melonDS gets from its transport.
2. **Stale packets are evicted by wall-clock age.** Every `ProcessLAN()` discards
   anything queued longer than 16 ms — one video frame — before doing anything
   else. Nothing old is ever delivered. This project delivers everything in
   order regardless of age, so the client is perpetually working through
   commands that are already irrelevant. That is the sustained rate mismatch
   measured in the queue-depth experiment, and it was misattributed there to
   unmodelled medium contention.
3. **One MP packet is consumed per service call**, deliberately: *"if we are
   receiving MP frames, if we keep going we'll consume too many even if we have
   no timeout set."*
4. **Replies are rejected on timestamp**, `header->Timestamp < (timestamp - 32)`,
   in the shared DS microsecond domain carried in `MPPacketHeader`.

The practical conclusion is the reverse of what this document previously argued.
The transport here is not too slow so much as **too reliable and too complete**.
The change to make is not deeper queues or cleverer pacing, but to drop stale
multiplayer frames by wall-clock age and stop guaranteeing delivery of traffic
whose moment has passed.

Sections below written before this correction should be read with it in mind,
particularly the architecture review's claim that the two-clock problem is
inherent, and the medium-contention hypothesis in the queue-depth experiment.

## Architecture review: the founding assumption was wrong

The project was started on the assumption that DraStic's Wi-Fi emulation is
largely present and only the transport and negotiation were left out. That is
false, and it is worth stating plainly because it changes how the remaining work
should be planned.

**DraStic's Wi-Fi is a stub that returns zero.** Three independent proofs:

- Letting `0x0094`/`0x0098` fall through to DraStic returned `0000` across 1415
  reads. It holds nothing.
- DraStic returns zero for `W_US_COUNT`, which is why this model had to take it
  over — Mario Kart abandons the join without it. A microsecond counter is
  purely local and has nothing to do with transport. An implemented-but-
  disconnected MAC would still run it.
- The client polls `W_RFStatus` 275 times, `W_RFPins` 254, `0x01dc`/`0x01de` 180
  each and `0x0044` 186 — **1075 reads, all returning zero.**

Set against what this project implements — packet RAM, the register file, the
baseband port, `W_IF`/`W_IE`, the microsecond counter and compare, IRQ delivery,
transmit slots and arming, the MP command/reply/ACK state machine, RX airtime
and the circular buffer, beacons, duplicate filtering, and now transceiver power
— the honest description is: **this is a from-scratch implementation of the DS
wireless MAC, written through a 16-bit MMIO keyhole inside a closed-source
binary.** Nothing is being reconnected.

**The assumption that actually matters is holding.** The real bet is that
DraStic's ARM7 emulation, memory and interrupt delivery are faithful enough that
Nintendo's wireless library — which is guest code, fully present and executing —
runs correctly against a hardware model supplied through MMIO. Authentication,
association, beacons and discovery all work; the room list shows the right host
name; the client reaches AID 1, receives commands, services `W_IF`/`W_IE`,
advances both RX cursors and times off a counter that is demonstrably moving.
That library gets a long way and is parked at a specific gate rather than
diverging. The expensive unknown is largely answered.

**Consequences for planning.** This is a porting problem, not a debugging
problem. `melonDS/src/Wifi.cpp` is about 2000 readable lines and is the
specification. Every comparison run against it has come back either *matches
exactly* (RX header layout, flag encoding, `SendMPReply`) or *entire block
absent* (RX filtering, power management). Enumerate the remaining work against
that file rather than chasing individual registers, which is what produced a
run of dead hypotheses: the transport reorder, LOC2, read ownership, and the RX
header.

A corollary: the curated ownership set is backwards. Everything outside it falls
through to a stub that answers zero, so each unowned register is a silent zero
waiting to gate something. The endpoint should be owning essentially the whole
Wi-Fi window — but see the power-management result below for why that has to be
earned one block at a time, not taken in one step.

**The deferred risk.** Nothing here touches the one from the earlier
architecture notes: two free-running clocks and a link two orders of magnitude
too slow for the 358 us reply window. It has not bitten only because no real MP
payload has ever flowed. `await_for_peer`'s bounded-stall-then-fabricate will
not survive one that does.

### Transceiver power management

Ported from melonDS `UpdatePowerStatus` / `SetStatus`: `W_ModeReset`,
`W_ModeWEP`, `W_TRXPower`, `W_PowerTX`, `W_PowerState`, `W_PowerForce` and
`W_PowerDownCtrl` write handlers, the `W_PowerForce` and `W_ModeReset`
overrides, the power-down-depth rules, the roughly 2 ms power-up delay
(`us_until_power_on`, counted down in `dwifi_model_advance_us`) and IRQ11 on
wake. `com_status()` stands in for melonDS's `ComStatus` so the transceiver
reports it is going down but does not stop mid-transfer.
`test_transceiver_power_management()` covers power-down, the delayed power-up
with its interrupt, `W_PowerForce` override and the `W_ModeReset` force-off.

melonDS's IRQ13 auto power-down is deliberately **not** wired up. This model
never raises IRQ15, so a station that powered down on a beacon would have
nothing to wake it — which is the hazard melonDS's own comment at `SetIRQ13`
warns about. Asphalt runs in `W_ModeWEP & 7 == 2` anyway, where melonDS does not
auto power-down either.

The block works and the guest drives it. In
`asphalt-cold-join-20260901-215658` the client wrote `W_ModeReset` 114 times,
`W_PowerTX` 79, `W_PowerState` 37, `W_PowerForce` 79 (including `8001`, a forced
power-off, 38 times) and `W_PowerDownCtrl` 111, and `W_RFStatus` reported `0001`
6391 times and `0009` 37 times where it had previously been zero throughout.

**Serving that state to the guest still regresses the join, and this now has a
precise trigger.** With `W_RFStatus`/`W_RFPins` added to
`dwifi_model_owns_read()`, the host's lobby showed `2: (EMPTY)` at `post-join` —
the client stopped registering at all. Reverting only that one ownership change,
keeping the entire state machine, restored it immediately
(`asphalt-cold-join-20260901-220053`, slot 2 occupied again). So the fault is
specifically in *serving* a partial radio state, not in modelling it — which
sharpens the older, vaguer note that "answering reads with a partial radio state
made the host spin".

What is still partial, and must land before that ownership is flipped again:
TX/RX gating on `W_PowerState` bit 9 (melonDS returns early in several paths),
IRQ15 and the auto power-down pair, and `W_PowerUS`. Flip the ownership on its
own afterwards so the result stays attributable.

### TX/RX power gating and the beacon interrupts — the guest staged a reply

Added, all ported from melonDS:

- **RX/TX gating on `W_PowerState` bit 9.** `dwifi_model_receive_frame()`
  refuses frames outright, and `fire_tx()`, `send_mp_reply()` and
  `send_mp_ack()` stay silent, matching the early returns in `CheckRX`,
  `MPClientReplyRX` and the transmit path.
- **IRQ15, pre-beacon.** Raised when the countdown to the next beacon reaches
  `W_PreBeacon` (`0x0110`), both sides masked to the 8 us timer granularity, as
  in melonDS. `raise_pre_beacon_irq()` then calls `update_power_status(1)` when
  `W_PowerTX` bit 0 is set — auto wakeup applies in every power-management mode.
- **IRQ13 auto power-down**, now wired, but only in mode 0 and only when
  `W_PowerTX` bit 1 is clear. In mode 2 — which is what Asphalt uses — the
  station does not service these interrupts and would never wake.

Tests: `test_beacon_power_interrupts()` covers the wakeup, the mode-2
no-power-down case and the mode-0 contrast; and
`test_powered_down_radio_is_deaf_and_mute()` covers the gating.

**Result: the guest staged a multiplayer reply for the first time.** In
`asphalt-cold-join-20260901-224158` the client acknowledged IRQ15 five times
(`W_IF` writes of `8000`, an interrupt this model had never raised) and then
wrote `W_TXSlotReply1` twice — `8000`, then `811a`, a genuine slot pointer.
Before this change the count across every run was zero. The trace also shows the
hardware latch working and being observed: after the write the guest reads
`0x0094` as `8000`, then `0x0098` as `8000` with `0x0094` back to `0000`.

Do not over-read it. Only two stagings occurred, both very late in the run
(sequence 23753 and 24545 of roughly 24000), and every reply frame sampled on
both sides is still 40 bytes — the empty default. Reply telemetry is sparse on
the model side (first 12, then every 128th) and logged only at powers of two in
`mp-progress`, so whether those two went out populated is **not** established
either way. The join is unaffected and still healthy.

The direction is nonetheless confirmed: the missing radio and power interrupts
were gating the library's multiplayer path, exactly as the architecture review
predicted, and supplying one of them moved it.

### Two stub constants block the ownership flip

`dwifi_model_read16()` still answers two registers with hardcoded values from
before power management existed:

```c
case REG_MODE_RST:    return 0;
case REG_POWER_STATE: return UINT16_C(0x0200);
```

So the guest is told the transceiver is permanently in state `0x0200` — bit 9
set, powered down — no matter what the model does, and that `W_ModeReset` reads
back zero after it writes one. `radio_powered_down()` deliberately reads the
internal shadow rather than going through `dwifi_model_read16()`, which is why
the gating works at all.

This also means an assertion of the form
`dwifi_model_read16(m, 0x0480003c) & (1 << 9)` passes vacuously. Tests assert on
`W_RFStatus` and `W_TRXPower` instead, which the model really serves. Retire
both constants together with the `W_RFStatus`/`W_RFPins` ownership flip, as one
deliberate change, and expect it to need its own on-device verification.

### Traces moved to their own directory

Per-run hook traces were accumulating loose in the emulator log directory and
burying everything else — 177 files on the host, 19 on the client. They now go
to a subdirectory:

```text
/mnt/SDCARD/.userdata/tg5040/logs/NDS-wifi/NDS-wifi-<timestamp>.txt
```

Changed in three places so it holds everywhere: the `nds-wifi-brick-pak` recipe
in the `Makefile`, the `launch.sh` on both devices, and the hook itself, which
now creates the parent directory and retries if the first `open()` fails with
`ENOENT`. That last one means the traces land correctly even under a launcher
that has not been updated. Existing files were moved into the new directory.

### Telemetry: populated replies are no longer sampled away

The sparse heartbeats existed to stop beacon and command traffic filling the
trace, but they were also hiding the one event under investigation. Two changes:

- `hooked_trace_receive()` always records `mp-reply-populated`, bypassing the
  1-in-128 sample. Empty defaults stay sparse.
- `log_mp_progress()` always reports a reply whose length is not the 40-byte
  default, bypassing the power-of-two filter.

**This immediately answered the question the previous section could not.** In
`asphalt-cold-join-20260901-224953`:

| evidence | result |
| --- | --- |
| model reply outcomes | 19 `mp-reply-default`, **2 `mp-reply-populated`** |
| guest reply-slot writes | `0x0094 <- 8000`, `0x0094 <- 811a` |
| client transmitted | 14 x `len=40`, **2 x `len=42`** |
| host received | 14 x `len=40`, **2 x `len=42`** |

The two staged replies from the previous run were never lost. They were
transmitted, and the host received both. **The multiplayer reply path works end
to end.** The lobby stays healthy throughout, and this was invisible purely
because the telemetry was sampling it out.

What remains is not that replies fail, but that the guest stages only twice. The
two populated replies land at `us=71407208` and `us=71408040`, 832 us apart and
about 109 ms into a multiplayer phase whose default replies run from
`us=71298480` to `us=72762216`. So the guest emits a two-frame opening burst and
then stops for the remaining 1.35 seconds — the shape of a handshake that opens
and then waits for something that never arrives.

### Mario Kart DS reproduces the fault identically

Testing moved back to Mario Kart, which is the more valuable target. The saved
macros still work unchanged, and it gets as far as Asphalt does:

- the host creates a group and advertises as `test1`. This is **not** the
  firmware rename: this document recorded the host as `test1` back in August,
  long before that change, and the client shows as `Testo`, which matches no
  configured value. Mario Kart takes its names from its own save, exactly as
  originally noted;
- the client's group list shows `NORMAL | test1`, so discovery works;
- the client enters the group, and authentication, association, MP commands and
  MP ACKs all flow.

Then the client raises **"An error has occurred."** while the host stays at
`CURRENT RACERS... 1/8`.

**The two games behave identically, down to the values.** Session
`mariokart-observe-20260901-231907` against
`asphalt-cold-join-20260901-224953`:

| | Mario Kart | Asphalt |
| --- | --- | --- |
| reply-slot writes | `0094 <- 8000`, `0094 <- 811a` | `0094 <- 8000`, `0094 <- 811a` |
| populated replies | `addr=0000 len=30`, `addr=0234 len=30` | `addr=0000 len=30`, `addr=0234 len=30` |
| gap between them | 768 us | 832 us |
| on the wire | 2 x `len=42` sent and received | 2 x `len=42` sent and received |

Same slot values, same packet-RAM addresses, same frame lengths, same two-frame
burst, in two unrelated games. This is Nintendo's shared wireless library
performing one fixed opening handshake and stopping at the same point in both,
which confirms the long-standing suspicion in this document that the fault
belongs to the library rather than to either game. **There is one bug to find,
and fixing it fixes both.**

Mario Kart is the better regression target from here: it reports the failure
explicitly instead of waiting silently, and its racer count gives a second
independent read on whether the host accepted the client.
`drastic-wifi/devbridge/mariokart-observe.test` captures the settled state.

### Procedure: check foreground, not process count

A Mario Kart run was wasted because the host was still in Asphalt.
`OPEN_GAME` is skipped when a game is already in the foreground, so the host
replayed the macro's inputs into the wrong game while the client switched
correctly. A process count of zero taken shortly after a safe-quit is **not**
sufficient — the game can come back up. Before any cold-join run, confirm
`foreground=devbridge` on **both** devices:

```sh
python3 /home/benja/dev/sbcs/my-paks/DevBridge.pak/tools/devbridge.py \
  --adb 1c000c66050607724dd=host --adb 2c000c81b5c4078245d=client exec all:STATUS
```

Together with the "confirm a new trace file appeared" rule earlier in this
section, that covers both ways a run has silently scored the wrong build or the
wrong game.

### Mario Kart reaches a real two-player lobby, intermittently

In `mariokart-timeline-20260901-232444` the host showed:

```text
1: test1
2: Testo          <- the client
CURRENT RACERS... 2/8      and the CUT OFF button, which starts the race
```

That is a genuine two-player Mario Kart lobby, and it is further than Asphalt
has ever reached: the client is listed **by its own name**, where Asphalt only
ever showed the default `DR DRASTIC` placeholder for slot 2. It held for at
least the twenty seconds the timeline covers.

It is also intermittent, and the join is transient. The client had already been
dropped by the time the follow-up hold session began about thirty seconds later,
which matches the observation that it "shows up briefly". Two later runs of
`mariokart-drop-timeline.test` never joined at all.

The failing runs fail earlier than the MP handshake, not in it. Their client
traces contain **no `mp-state` entries whatsoever** and no reply-slot writes,
against 16 default plus 1 populated reply in the successful run. They also
contain no `frame dir=` lines on either console — which means no authentication,
association or MP traffic, but says nothing about beacons, since `log_frame()`
deliberately excludes them. So in those runs the client simply never discovered
or entered the group, and the macro's fixed-timing `A` press landed on nothing.

Discovery, not the reply handshake, is what is unreliable here.

### Power gating reverted, and a correction

The transmit/receive gating on `W_PowerState` bit 9 added earlier in this
session has been reverted. Only the gating: the power state machine, the
register handlers, IRQ15 and the mode-gated IRQ13 all remain, and IRQ15 is what
first got the guest staging replies.

**The evidence first cited for reverting it was misread.** The reasoning was
that gating had silenced the radio, because no frames appeared in either log.
Beacons are excluded from that log, so it showed nothing of the kind, and
reverting the gating did not change the outcome. The gating is therefore
exonerated as the cause of those silent runs.

The reason to leave it reverted is a different and still-valid one. The guest
forces the transceiver off and on over a hundred times per run — `W_PowerForce`
is written `8001` 185 times on the client and 127 on the host in a single run —
and gating reception simply discards any frame that arrives inside one of those
windows. melonDS can gate safely because its receive path and its radio share a
timeline; here frames arrive whenever the socket has them. Deferring instead of
dropping was tried and did not help either. Restore the gating only once the two
timelines are related, which is the same deferred risk the architecture review
identifies.

### The verbose MMIO trace was slowing the guest by three orders of magnitude

Reported from watching the device, not visible in any screenshot: once the
client attempts to join, its frame rate collapses to a crawl — roughly a minute
to change screen — and only recovers when "An error has occurred." appears.

Measured from the trace, pairing each `mp-state` line against the wall clock:

| phase | emulated / wall |
| --- | --- |
| during the join | **0.001x - 0.002x** |
| at the error, ~44 s in | 0.048x, then 1.415x |
| whole run | 0.131x |

The client was running **500 to 1000 times slower than real time**. Since the
host runs normally, the two diverge completely, and the host drops a station
that is effectively frozen.

**The trace was blamed for this and that was wrong.** Disabling
`DRASTIC_WIFI_VERBOSE` appeared to restore full speed, but the comparison was
confounded: both quiet and verbose runs show a slow opening phase followed by
recovery, and the two runs differed in how much of the sampled window covered
it. Direct instrumentation of the hot path settles it — across a full run:

| measure | value |
| --- | --- |
| register loads | 3,481,735 |
| trace records formatted | 28,385 |
| time in `snprintf` | **140 ms** |
| time in buffered `write()` | **31 ms** |

About 171 ms in total, which cannot account for a multi-second stall. The
verbose trace is not the cause of the crawl. **The cause is still unknown**, and
the crawl itself is real: it was observed directly on the device, beginning when
the client attempts to join and lifting when the error appears.

Tracing is nonetheless now much cheaper, and switchable on device without a
rebuild, in the same idiom as the other toggles:

```sh
adb -s <serial> shell "touch /mnt/SDCARD/Emus/tg5040/NDS.pak/drastic/quiet_trace.enabled"   # fast
adb -s <serial> shell "rm -f /mnt/SDCARD/Emus/tg5040/NDS.pak/drastic/quiet_trace.enabled"   # verbose
```

The `mp-state` telemetry now also carries `wall_ns` alongside `us`, so emulated
against real time can be checked from any run regardless of the verbose setting.

**Consequences, which are serious.** The guest really does run at a thousandth
of real time during part of the multiplayer phase, whatever the cause. Every
timing statement in this document measured across that window is wrong by up to
three orders of magnitude: a lobby that "held for twenty seconds" may have held
for twenty milliseconds of emulated time, and runs that appeared to fail may
have failed only because the guest was too slow to answer. Check `wall_ns`
against `us` before trusting any timing.

### Trace cost reduced anyway, and crash-survivable

Independent of the above, the trace path was made cheap and crash-safe:

- **tmpfs, not SD.** The trace is written to `/tmp` — 499 MB of tmpfs, against
  vfat on SD, which is the worst case for many small writes. The launcher moves
  it to `logs/NDS-wifi/` after the emulator exits, which also runs after a
  crash; if that never happens the file is still in `/tmp`. **During a run the
  live trace is in `/tmp`, not in the log directory** — pull it from there.
- **64 KB buffering** in `trace_write()` instead of a `write()` per access.
- **Flush on crash.** `SIGSEGV`, `SIGBUS`, `SIGABRT`, `SIGILL` and `SIGFPE` are
  handled, the buffer is flushed, and the previous handler is restored and the
  signal re-raised, so DraStic's own handlers still run. A crash keeps at most
  64 KB of loss, and remains evidence.

An asynchronous writer thread was considered and is not worth building: the
measured write cost is 31 ms per run.

### Join macro: discovery needed time, not luck

The Mario Kart macro pressed `A` on the client 2.5 seconds after the host
finished creating its group, where the Asphalt macro allows ten seconds to scan.
Discovery is beacon-driven and often had not completed. Worse, the client's list
starts empty with the highlight on **CREATE GROUP**, so an early press made the
client create a rival group instead of joining — which is why those runs showed
no authentication or association traffic at all, and why discovery failures were
being scored as join failures.

`mariokart-cold-join.test` and `mariokart-drop-timeline.test` now allow eighteen
seconds and capture the client at six, twelve and eighteen seconds, so what the
scan actually saw is always on record. With that change the client is listed at
`post-join` in every run since, and `client-room-visible-client.png` shows the
highlighted `NORMAL | test1` entry at the moment of the press.

The lobby still collapses afterwards, at full speed as well as under a verbose
trace, so the collapse is real and not merely the crawl. But its timing has
never yet been measured under a quiet trace.

### The client associates fine and is then starved of beacons

On a real DS, joining a multiplayer game *is* joining the host's access point,
so the question was whether the association process is modelled at all, or
whether the game errors because an associated status never arrives.

**Association is modelled and succeeds.** Decoded from the wire:

- authentication is open system — client sends sequence 1 status 0, host replies
  sequence 2 **status 0**;
- the association response body is `2100 0000 01c0 0102 8284`: capability
  `0x0021`, **status `0x0000` (success)**, AID `0xc001` — that is **AID 1** with
  the two required top bits set, followed by the supported-rates element.

The client then writes `W_AID_LOW`/`W_AID_FULL` = 1, and its incoming MP frames
carry `0x801c`, whose `0x8000` bit means the BSSID matched. So the station is
properly authenticated, associated, holds the AID the host polls, and has its
BSSID registers right. The straightforward version of the theory is false.

**The station is then starved of beacons, which is how it loses the network.** A
station stays associated by hearing beacons and gives up after missing several.
Instrumenting delivery on the client:

| measure | value |
| --- | --- |
| beacons delivered before trouble | 123 |
| gaps over 500 ms | 10 in the sample |
| minimum gap | 669 ms |
| median gap | 1954 ms |
| maximum gap | 2174 ms |

Against an expected interval of about 100 ms, those are **16 to 22 consecutive
missed beacon periods**, repeatedly, and they begin only after multiplayer
traffic starts. Real hardware declares the network lost after a handful. This
fits the observed failure exactly: the client joins, the host lists it, and then
it errors a short time later.

The mechanism is in the transport. A beacon arriving while the radio is busy is
stashed in `pending_beacon`, which holds **only the newest** — earlier ones are
overwritten — and delivery then requires both `!model->rx_active` and 100 ms of
emulated time since the last one:

```c
if (!model->rx_active && pending_beacon_length &&
    model->us_timestamp - last_beacon_rx_us >= UINT64_C(100000)) {
```

During multiplayer `rx_active` is asserted for most of the time, because command,
reply and ACK frames occupy the modelled radio almost continuously. Beacons lose
that race and are dropped rather than delayed. The rate limit then prevents
catching up, because it is a floor on the interval rather than a budget.

This is the same lossy-inbox trade described in the architecture notes, biting a
stream that cannot tolerate loss. Command traffic can miss a generation
harmlessly; the beacon stream is what keeps the station associated.

**The transport is not what loses them.** Giving an overdue beacon first refusal
on the receive window was tried — `deliver_overdue_beacon()` now runs ahead of
`deliver_pending()`, which had been taking every free window and marking the
radio busy — and it changed nothing: gaps stayed at a median of 2038 ms against
1954 ms before. Counting both ends explains why:

| | beacons tx | rx | delivered | max gap |
| --- | --- | --- | --- | --- |
| host | **227** | 0 | 0 | — |
| client | 0 | **227** | **225** | 1.98 s |

The client receives every beacon the host sends and delivers all but two. There
was no queue to prioritise. **The host is not beaconing often enough**, and the
gaps the client sees are real gaps in transmission.

227 beacons across a whole run is roughly a third of what a 100 ms interval
would produce over the emulated time involved. The beacon is transmitted by the
host's guest software from the beacon interrupt, which this model raises in
`advance_ms()` when `W_BeaconCount1` reaches zero and reloads it from
`W_BeaconInterval`. So the next thing to establish is whether the host's beacon
interrupt fires at the interval the guest programmed, and whether every one of
those interrupts actually results in a transmitted beacon.

Note also that the host's own emulated clock may be running slow, which would
stretch the interval in wall time without the interval itself being wrong. The
`wall_ns` field makes that checkable and should be checked first, because it is
cheaper than instrumenting the beacon path.

The `deliver_overdue_beacon()` ordering change was kept: it is harmless, it is
more correct than letting multiplayer traffic monopolise the receive window, and
it costs nothing. It is simply not the fix.

Instrumentation now in place: `beacons tx=N rx=N delivered=N max_gap_us=M` is
printed by each console when its transport stops, and any delivery gap over
500 ms is reported inline.

### The host's clock is running ten times slow, and that is the whole chain

Tested directly, because it was worth knowing rather than assuming. Both
consoles emit `mp-state` lines carrying `us` and `wall_ns`, so each one's
emulated-to-wall ratio can be read from its own trace:

| console | emulated / wall | per-segment |
| --- | --- | --- |
| **host** | **0.097x** | 0.034 - 0.150 |
| client | 0.906x | ~1.00 throughout |

**The host runs at about a tenth of real time; the client runs at full speed.**
This was expected to come back clean and did not.

The host's beacon timer is not at fault. Instrumenting transmission shows it
beaconing every **217.0 ms of host-emulated time**, and remarkably steadily —
minimum 216.1 ms, maximum 217.0 ms across 24 samples. The interval is correct in
the host's own frame of reference. At 0.097x that becomes roughly **2.24 seconds
of wall time** per beacon, and the client, running at 0.906x, therefore sees
gaps of about 2.0 seconds in *its* emulated time — which matches its measured
1.98 to 2.28 second maximum gaps almost exactly.

The cause of the host's slowdown is `dwifi_transport_await_for_peer()`. The host
issues multiplayer commands every **1.44 ms of emulated time** in bursts, and
the barrier blocks for up to `DWIFI_REPLY_WAIT_ATTEMPTS` x
`DWIFI_REPLY_WAIT_STEP_US` = **50 ms of wall time** per command while the
client's reply crosses the link, with 8 outright timeouts at the full 50 ms in
this run. Only the host takes this path, because only the host has
`mp_cmd_active`. A round trip of about 3 ms against an emulated command period
of 1.44 ms is already a slowdown before any timeout.

So the complete chain, every link now measured:

1. the host blocks up to 50 ms of wall time per multiplayer command in
   `await_for_peer()`, against commands every 1.44 ms of emulated time;
2. host emulation falls to 0.097x real time;
3. its beacon timer, correct at 217 ms emulated, fires only every ~2.24 s wall;
4. the client, at 0.906x, sees ~2.0 s gaps — about twenty missed beacon periods;
5. the client concludes the access point is gone, raises "An error has
   occurred.", and the host drops it from the group.

This is the deferred risk from the architecture review coming due exactly as
described there: two free-running clocks and a link three orders of magnitude
too slow for the window being emulated, with `await_for_peer()` trading
wall-clock stalls for protocol correctness. It is no longer a theoretical
concern; it is the thing breaking the lobby.

Secondary question worth noting: 217 ms is about twice the DS nominal beacon
interval of 102.4 ms, so the programmed interval may also be wrong. That is a
much smaller effect than the 10x clock divergence and should be looked at after
it.

### The barrier is both the slowdown and the flow control

Option B — proceed on the last known reply rather than stalling for a fresh one
— was tested against a control that simply shortened the barrier from 100
attempts (50 ms) to 6 (3 ms, about one link round trip). B also added a grace
window in `finish_mp_cmd()`: a station that replied within
`DWIFI_MP_REPLY_GRACE_US` (500 ms emulated, roughly two beacon periods) stays
credited rather than being marked failed for a generation whose reply could not
cross the link in time.

| config | host clock | beacon max gap | gaps > 500 ms | client cmds received | joins | holds |
| --- | --- | --- | --- | --- | --- | --- |
| baseline, 100 attempts | 0.097x | 1984 ms | 10 | — | yes | no |
| control, 6 attempts | 0.278x | 752 ms | 4 | 512 | yes | no |
| **B, 1 attempt + grace** | **0.646x** | **234 ms** | **0** | **8** | **no** | — |
| 6 attempts + grace | 0.288x | 235 ms | 0 | 16 | yes | no |

**B is the fastest and it breaks the join.** With the host free to run, the
client receives 8 commands instead of 512 and never establishes the session.
The cause is not a flood but its mirror image: `stash_busy_frame()` keeps only
the newest frame per role, so an unpaced host overwrites its own commands before
the client consumes them. The barrier was silently doing two jobs — capping how
far the host may run ahead, and, as a side effect, costing it 90% of its clock.
That is why it cannot simply be shortened away, and the comment at
`DWIFI_REPLY_WAIT_ATTEMPTS` now records the measurements so this is not
rediscovered.

**Beacon starvation is solved, and the lobby still collapses.** The retained
configuration — 6 attempts plus the grace window — holds beacon gaps to a
maximum of 235 ms with **no gap over 500 ms at all**, against 1984 ms and ten
such gaps at baseline. The client comfortably hears its access point now. It
still joins and is still dropped.

So beacon starvation was a real fault, was correctly diagnosed, and was not the
cause of the collapse. Two suspects remain, both visible in the same table: the
client's own clock, which reads 0.010x in the retained configuration, and
command delivery, where the client receives 16 of the host's generations rather
than all of them.

The structural answer is the one the architecture review already names. Pacing
and speed are in direct conflict only because synchronisation happens per
transfer. A command **queue** instead of newest-wins would let the host run
ahead without losing generations, and a coarser synchronisation boundary would
remove the per-transfer round trip altogether.

### The queue works, and shows the real constraint is the client's clock

The newest-wins command slot was replaced with a ring of eight generations
(`DWIFI_MP_GENERATIONS`), each holding a command and its closing ACK, delivered
head-first and only once complete — preserving the invariant the two-deep
version enforced, that an ACK is never spliced onto a different host cycle.

It behaves correctly and does not regress. It also makes the actual bottleneck
impossible to miss:

| measure | value |
| --- | --- |
| client queue high water | **8 of 8 — permanently full** |
| generations dropped | **798** |
| host clock | 0.279x |
| **client clock** | **0.008x** |

**Correction: there is no global clock disparity.** The 35x figure recorded here
originally was measured only across the multiplayer sample window and was wrong
as a general statement. Over a whole run the two consoles emulate almost exactly
the same amount of time — host `wifi_us` 94.8 s against the client's 91.9 s,
with ARM9 cycle totals of 6.36e9 and 6.16e9. What is real is that the host
produces generations faster than the client's receive pipeline accepts them, the
queue fills, and generations are discarded.

This closes the loop on the barrier. `await_for_peer()` was never really a
protocol device: it was pacing the host's production to what the client's
receive pipeline could accept, and it worked because it stalled the host by
roughly the right amount.

The queue is kept. It is more correct than newest-wins, it costs nothing, and
its `high_water` and `dropped` counters are the clearest signal yet of how far
apart the two consoles are running.

### Where the wall time goes, and why depth does not help

Wall time was attributed to the three sections of the event hook separately, and
backward or zero scheduler deltas counted, since the hook silently treats a
backward counter movement as zero elapsed.

| measure | host | client |
| --- | --- | --- |
| `original_execute_events` | 63251 ms | 62824 ms |
| `dwifi_model_advance_us` | 9968 ms | 5731 ms |
| `dwifi_transport_poll` | 1279 ms | 4223 ms |
| transport polls | 1,376,314 | 1,414,803 |
| zero / backward deltas | 0 / 0 | 0 / 0 |

Three things follow. DraStic's own emulation dominates, but **symmetrically** —
63.3 s against 62.8 s — so it is not the source of a client-specific stall. The
hook's own paths are cheap. There are **no scheduler anomalies at all**, which
retires the concern that silently zeroed backward deltas were hiding something.
And with 1.4 million polls the transport is not poll-starved: delivery is not
limited by how often it is serviced.

Queue depth was then raised from 8 generations to 64:

| depth | high water | dropped | client frames received |
| --- | --- | --- | --- |
| 8 | 8 (full) | 468 | 1317 |
| 64 | **64 (full)** | 351 | 1540 |

**The queue fills to whatever depth it is given.** An eightfold increase cut
drops by a quarter. That is the signature of a sustained rate mismatch rather
than burst overflow — depth delays the overflow, it does not remove it. Reverted
to 8, since 64 costs 256 KB of static buffer for no qualitative change.

Frame accounting corroborates the mechanism exactly in every run: host
transmitted 2265, client received 1317, difference 948, against 468 dropped
generations at two frames each = 936.

**The likely cause is that medium contention is not modelled.** On real hardware
a shared radio serialises everything: the host physically cannot begin a frame
while one is still on the air, so it can never out-produce what a station's
receiver can accept. In this model the host's transmissions are not constrained
by the medium being busy, while the client's receive path *is* — `rx_active`
holds for each frame's airtime and delivery is one frame per free window. The
host therefore over-drives a medium the client is obliged to honour. That
asymmetry, not a slow client, is what fills the queue.

### Mirroring melonDS DS (libretro core)

`/home/benja/dev/sbcs/cores/melonds-ds` is much the closer reference: it drives
the same `MPInterface` through libretro's netpacket API, which is the interface
this project's own shim implements. Its multiplayer transport is about 260 lines
across `src/libretro/net/mp.cpp` and `src/libretro/platform/mp.cpp`.

Licensing note: melonDS DS is GPLv3 and this pak is MIT, so its source was not
copied. The design is mirrored — header layout, constants, timeout, flag
semantics, staleness rules — and cited in comments.

| behaviour | melonDS LAN (ENet) | melonDS DS (libretro) | this project, now |
| --- | --- | --- | --- |
| recv timeout | `RecvTimeout = 25` ms | `RECV_TIMEOUT_MS = 25` | 50 x 500 us = 25 ms |
| delivery | `ENET_PACKET_FLAG_UNSEQUENCED` | `UNSEQUENCED, UNRELIABLE, FLUSH_HINT` | **UDP datagram channel** |
| header | magic/sender/type/len/timestamp | **timestamp u64, aid u8, type u8** | **same 10-byte layout** |
| frame type | explicit | explicit | **explicit** (was MAC inference) |
| reply staleness | `Timestamp < (timestamp - 32)` | same | constant defined, not yet applied |
| reply addressing | unicast to host | unicast to host | broadcast to peer |
| stale sweep | 16 ms wall-clock | none, transport drops | 16 ms wall-clock |

Implemented:

1. **The 10-byte header**, identical in layout to melonDS DS: big-endian 64-bit
   timestamp, association id, frame type. The timestamp is the DS microsecond
   counter, which both consoles already synchronise from beacons.
2. **Explicit frame types on the wire.** The receiver no longer re-derives
   command/reply/ACK from multicast MAC addresses. That inference produced the
   LOC2 confusion and the address-3 canonicalisation workaround.
3. **An unreliable datagram channel** for multiplayer frames, on the session port
   plus one. `NetLink_send` ignores its flags — its own comment says
   "TCP_NODELAY means every packet is already flushed" — and NetLink is shared
   with the other cores, so rather than change its semantics a UDP socket is
   opened alongside it. Multiplayer frames go by datagram; everything else,
   beacons included, keeps the reliable path, falling back to it silently if the
   socket cannot be opened.
4. **16 ms staleness eviction** before delivery, from melonDS LAN's
   `ProcessLAN`.

**Frame loss is essentially gone.** Best measured run of the night:

| measure | before | after |
| --- | --- | --- |
| host tx / client rx | 2265 / 1317 (**948 lost**) | 2512 / 2484 (**28 lost**) |
| client tx / host rx | 523 / 515 | 1143 / 1140 |
| queue generations dropped | 468 - 798 | **0** |
| beacons delivered | partial | **201 of 201** |
| client clock | erratic | **0.982x** |
| lobby at the observe checkpoint | one racer | **two racers** |

### The reply staleness check does not port, and why

melonDS DS rejects a reply predating the command it answers:
`if (p.Timestamp() < (timestamp - 32)) continue;`. With the timestamp now on the
wire this was implementable for the first time. Ported literally it is actively
harmful:

| | staleness enforced | measured only |
| --- | --- | --- |
| replies rejected | 1054 of 1347 | 0 |
| barrier timeouts | 1233 of 1367 (90%) | 242 of 442 (55%) |
| barrier wall cost | 36.1 s | 7.9 s |
| host rx / client tx | 292 / 1347 | **406 / 406** |

The rejections caused the timeouts: the barrier was discarding the very replies
that would have released it. The accounting is exact — 292 delivered plus 1054
rejected is the client's 1347 transmissions.

The rule assumes both instances share a tight clock domain. Ours do not. The
client re-adopts the host's clock from every beacon while the host runs at a
fraction of real time, so the two counters diverge and re-converge continuously.
Even with the check disabled, **44% of replies would still fail it**, which
measures the divergence directly.

Currency is instead enforced by the 16 ms arrival-age sweep, which uses local
time and needs no agreement between consoles. The comparison is kept as a
counter (`would_reject_stale`) because it is the cleanest available measure of
how far apart the two clocks are running.

### The barrier's real cost was waiting for replies already in hand

Instrumenting the barrier showed it timing out on most entries. The cause was
not slow replies but ordering: the host runs at a fraction of the client's
speed, so a reply is frequently consumed by an ordinary `dwifi_transport_poll()`
*before* the model reaches its reply deadline and asks to wait. The barrier then
sat for its full timeout waiting for something that had already arrived.

`start_mp_cmd()` now records `mp_cmd_started_us`, and the barrier returns
immediately when `mp_last_reply_us >= mp_cmd_started_us`. Both are the host's own
clock, so this comparison is sound where the cross-console one was not.

| | before | **with early exit** |
| --- | --- | --- |
| barrier short circuits | — | **457** |
| barrier timeouts | 242 of 442 (55%) | **88 of 1346 (6.5%)** |
| host clock | 0.070x | **0.198x** |
| beacon gap median / max | 2233 / 3120 ms | **978 / 1203 ms** |
| client tx / host rx | 406 / 406 | 1331 / 1329 |

### Current state of the barrier trade

With everything above in place, the length trade is much narrower than it was
but has not disappeared:

| barrier | host clock | beacon gaps | client queue drops | join |
| --- | --- | --- | --- | --- |
| 50 (25 ms, melonDS) | 0.198x | median 978, max 1203 ms | 4 | joins about half of runs |
| 6 (3 ms) | **0.345x** | **2 gaps, max 593 ms** | 142 | did not join |

The tree is left at 50. Two consecutive runs at that setting gave one join and
one non-join, and neither held the lobby to the twenty-second checkpoint, so the
honest summary is: **delivery is close to lossless, the host is roughly three
times faster than it was, beacon gaps are down from 3.1 s to 1.2 s, and the
lobby still does not survive.**

A beacon gap of 1.2 s is still about a dozen beacon periods, so the station is
still losing its network — just later than before. The host's clock remains the
lever: at 0.198x its 217 ms beacon interval is still about 1.1 s of wall time.

### Hunting the multiplayer-phase CPU cost

Whole-run section totals hide the problem, because the host runs near real time
for most of a run and collapses only once multiplayer starts. The three event-hook
sections are now also accumulated **scoped to the multiplayer phase**, latched on
`mp_cmd_active || mp_reply_active || mp_ack_active` so both consoles measure it,
with per-call maxima alongside the totals — a handful of pathological calls and a
uniform slowdown look identical in a total.

| multiplayer phase | host | client |
| --- | --- | --- |
| `original_execute_events` | 189.9 s | 221.6 s |
| `dwifi_model_advance_us` | **27.0 s** | 18.0 s |
| `dwifi_transport_poll` | 16.3 s | 17.8 s |
| max single `advance_us` | **29.3 ms** | 4.1 ms |
| max single `execute_events` | 27.3 ms | **3300.8 ms** |
| max single `poll` | 3.0 ms | 103.3 ms |

Section totals can exceed wall time because `original_execute_events` is timed
with an atomic add and DraStic drives it from more than one thread; treat them as
relative weights, not absolute durations.

Inside the model loop, the cost is **not** where it was expected:

| model counter | host | client |
| --- | --- | --- |
| ticks executed | 50.4 M | 52.4 M |
| `advance_us` calls | 38.7 M | 40.2 M |
| RX halfword writes | 21 k | 109 k |
| pre-beacon IRQ raises | 660 | 298 |

About 1.3 ticks per call, so the tick loop is not the cost — the per-call
overhead is, across roughly 39 million calls. The RX halfword path and the
pre-beacon comparison added with IRQ15 are both negligible, which retires two
standing suspicions.

The host's `advance_us` costs 50% more than the client's while its emulated
clock advances five times slower, so the host's DraStic is executing fewer guest
cycles per unit of wall time. Where that goes is still open.

### Two measurement faults found while doing this

**Stale traces.** `measure.sh` pulled the newest file from `logs/NDS-wifi/`
immediately after the safe-quit, but the launcher moves the trace out of `/tmp`
only once DraStic has exited. The pull raced it and returned the *previous*
run's file — two consecutive runs reported byte-identical counters
(`ticks=14462566`), which is what exposed it. The script now waits for `/tmp` to
drain on both devices first. Any measurement taken before this fix should be
re-taken.

**Capture cost.** Measured across twelve sessions:

| command | n | median | p90 | max |
| --- | --- | --- | --- | --- |
| `screen` | 61 | 199 ms | 232 ms | 296 ms |
| `status` | 33 | 218 ms | 265 ms | 378 ms |
| `input` | 20 | 130 ms | 135 ms | 153 ms |
| `hello` | 24 | 99 ms | 111 ms | 127 ms |

`hello` is close to pure round trip, so a capture costs roughly **100 ms of
device-side work** on top of it. In a normal `measure.sh` run that is about 1% of
the time and can be ignored, but `mariokart-drop-timeline.test` fires 32 captures
at 1.5 s intervals on the host alone — around 7% duty cycle on the console whose
speed is being measured. Do not densely capture the console under timing.

### DevBridge gained VERIFY and VERIFY_WAIT

Macro steps that could silently land in the wrong place are now gated on a
screen comparison, so a bad run aborts at the step that went wrong instead of
pressing buttons into the wrong menu for another minute and being diagnosed
afterwards from the wreckage. Added to
`/home/benja/dev/sbcs/my-paks/DevBridge.pak/tools/devbridge.py`:

```text
VERIFY      REFERENCE [threshold=N] [region=X,Y,W,H]
VERIFY_WAIT REFERENCE TIMEOUT_MS [threshold=N] [region=X,Y,W,H]
```

`VERIFY` captures once and fails if the screen does not match. `VERIFY_WAIT`
checks at the start, at half the timeout and at the end, and fails only if all
three miss; it returns as soon as one matches, so a fast match costs nothing.
Both honour the `host:` / `client:` / `all:` prefix, and with `all:` every
selected device must match. Results go to `events.jsonl` with the measured
difference.

The metric is the mean absolute difference of a 64x48 greyscale reduction.
Measured separations between the reference screens are 28 to 53, and identical
screens score 0, so the default threshold of 10 is comfortable.

The `region` option exists because whole-screen comparison cannot tell a lobby
with one player from one with two: the difference is a single row of text, and
genuine two-racer frames score 1.5 to 4.2 against the reference while a
one-racer frame scores 2.95 — completely overlapping. Restricted to the racer
badge at `900,160,115,35`, two-racer frames score **0.000** and a one-racer
frame **5.358**, which separates cleanly at `threshold=2`.

References live in `drastic-wifi/devbridge/reference/`. Note that
`mk-host-seeking.png` and the one-racer lobby are the same screen — "seeking
opponents, 1/8" *is* the empty lobby.

It proved itself on the first run. Every navigation step verified, and the macro
stopped exactly at the join:

```text
host:   verified mk-main-menu.png        (difference 0.00, attempt 1)
client: verified mk-main-menu.png        (difference 0.00, attempt 1)
host:   verified mk-host-seeking.png     (difference 0.14, attempt 2)
client: verified mk-client-room-list.png (difference 0.00, attempt 1)
mariokart-cold-join.test:46: screen does not match mk-host-two-racers.png:
                             host off by 5.36 (threshold 2.00)
```

5.36 is precisely the one-racer distance, so that run reached the room list,
pressed A and did not join. Menu slips, discovery misses and genuine join
failures can no longer be confused with one another, which is what made the
earlier intermittency so hard to read.

### Launch timing measured, and the prologue cut by 25 seconds

Probing the host every couple of seconds from launch:

| time | screen |
| --- | --- |
| 3 s | still booting |
| **5 - 9 s** | **primary menu** |
| 11 s onward | attract demo (a full CPU race) |

So the menu is available from about 5 s and the demo takes over at about 11 s.
The old prologue waited 25 s — landing inside the demo every time — then pressed
A to leave it and B to undo the Single Player that A selected on the runs where
the demo had already ended. All of that is gone. The macro now waits 4 s and
`VERIFY_WAIT`s the menu with a 6 s budget, checking at 4, 7 and 10 s, entirely
inside the window. **About 25 s saved per run**, and no input is sent that could
itself land in the wrong menu.

`LEFT` was also tested as an attract-mode suppressor and it works: with LEFT
pressed every 3 s the primary menu was still up at 38 s, against a demo that
otherwise starts at 11 s. It is inert on the menu and resets the attract timer.
It is **not used**, because waiting for the menu is sufficient and sending no
input is simpler.

### Two more macro robustness fixes

**Fail fast when a game is already running.** `OPEN_GAME` is silently skipped
when a game is in the foreground, and the macro then drives whatever session is
up — one run ended up in Grand Prix cc-selection and would previously have been
recorded as a multiplayer failure. Both Mario Kart macros now open with
`all:WAIT_FOR devbridge 3000`, which errors immediately unless both consoles are
idle, and `measure.sh` safe-quits before starting.

**VERIFY keeps its closest miss.** A failed comparison previously deleted the
capture, so seeing what had actually been on screen meant repeating the whole
run. The closest miss is now written to
`verify-miss-<reference>-<device>.png` in the session directory and its path
printed. That is how the Grand Prix state above was identified, immediately,
from the run that failed.

### A real bug the verification exposed: foreign traffic injected as radio frames

`start_mp_datagrams()` bound the multiplayer socket to `INADDR_ANY` and left it
unconnected, so **anything on the network that sent to that port was stripped of
ten header bytes and injected into the Wi-Fi model as a radio frame**. Observed
in a client log: frames whose source address decoded to the ASCII `nextui`, a
BSSID of `-G2QE` and a body containing `playwithme` — another service's LAN
discovery being fed to Nintendo's wireless library.

The socket is now `connect()`ed to the peer, so the kernel drops everything
else, and `send()` replaces `sendto()`. Foreign frames went from a steady stream
to zero. This had been running since the datagram channel was added and would
have been very hard to see without the screen verification forcing runs to fail
where they actually went wrong.

### The join is now deterministic up to the failure

With the macro gated on VERIFY at every step, a run reads:

```text
client: verified mk-main-menu.png        (difference 0.00, attempt 1)
host:   verified mk-main-menu.png        (difference 0.00, attempt 2)
host:   verified mk-host-seeking.png     (difference 0.00, attempt 2)
client: verified mk-client-room-list.png (difference 0.00, attempt 1)
mariokart-cold-join.test:54: host off by 5.36 (threshold 2.00)
```

5.36 is exactly the one-racer distance. Menu navigation, host advertising and
client discovery are all confirmed; only the join fails.

And the join failure is **not** a transport failure. The management exchange
completes in full on both consoles — authentication both ways, an 82-byte
association request, a 50-byte association response — followed by 60 data frames
each way. The client authenticates, associates, and exchanges multiplayer
traffic, and the host's *game* still shows one racer.

The reply content is the same signature this document has recorded from the
beginning, unchanged by any of the transport work:

| | count |
| --- | --- |
| `mp-reply-default` | 20 |
| `mp-reply-populated` | **2** |
| client TX 40-byte (empty) replies | 15 |
| client TX 42-byte (populated) replies | **2** |

The client's wireless library emits a two-frame opening burst and then only
empty defaults. Everything else — the beacon starvation, the host's clock, the
barrier tuning — has been compensating for that one fact.

### Harness: launch timing and fail-fast

`WAIT_FOR devbridge 3000` now heads both Mario Kart macros, because `OPEN_GAME`
is silently skipped when a game is already up and the macro then drives whatever
session it finds. One run reached Grand Prix cc-selection that way. `measure.sh`
safe-quits first, and the safe-quit itself is not always reliable on the first
attempt, so it is worth confirming `foreground=devbridge` on both consoles
rather than assuming.

`VERIFY` keeps its closest miss at `verify-miss-<reference>-<device>.png` and
prints the path; that is how the Grand Prix state was identified immediately
rather than by repeating the run.

The host's advertising check needed 15 s rather than 10: Mario Kart slides its
menus in, and a capture taken mid-transition matches nothing.

### The process check was vacuous, and 144 orphaned launchers had accumulated

`ps -ef` on these devices produces **no output at all** — busybox `ps` does not
accept `-ef`. Every check of the form `ps -ef | grep -c '[d]rastic'` in this
work therefore returned 0 unconditionally, and "no DraStic running" was never
actually verified.

Under that blind spot, orphaned launcher processes accumulated: a scan of
`/proc/*/cmdline` found **144** stale `game-launch` / `NDS.pak/launch.sh`
processes on the host and 12 on the client. DraStic itself had exited each time
but its wrapper had not, which is why the host kept reporting
`foreground=game` with no emulator running, and why `OPEN_GAME` kept being
skipped.

The host is the console that has been running at a fraction of real time
throughout. 144 stale shells is not obviously enough to explain that on its own,
but it is a real confound and every host timing measurement in this document was
taken with some number of them present.

Use a `/proc` scan, not `ps -ef`:

```sh
adb -s <serial> shell "for d in /proc/[0-9]*; do \
  c=\$(tr '\\0' ' ' < \$d/cmdline 2>/dev/null); \
  case \"\$c\" in *game-launch*|*NDS.pak/launch.sh*|*drastic*) echo \${d#/proc/};; esac; done"
```

Killing them returned the host to the NextUI frontend rather than DevBridge, and
DevBridge must be the foreground application before `OPEN_GAME` will work —
`OPEN_PAK DevBridge` does not bring it back, so this needs opening on the device
by hand.

### VERIFY self-test

Because the whole harness now rests on it, `VERIFY` was tested against itself: a
check that must pass and a check that must fail, in one run.

```text
client: verified mk-main-menu.png (difference 0.00, attempt 2)
verify-selftest.test:10: screen does not match mk-client-room-list.png:
                         client off by 51.55 (threshold 10.00)
```

Both behaved correctly, and the failure value cross-checks exactly: 51.55 is the
precomputed distance between `mk-main-menu.png` and `mk-client-room-list.png` in
the reference matrix, so the live capture is pixel-identical to the main-menu
reference. The comparison reads the live screen and discriminates; the repeated
`0.00` scores are genuine, the menus being static and deterministically
rendered.

### What is still wrong

The lobby still collapses, and run-to-run variance is still high. The
unresolved tension is the reply barrier, now well characterised:

| barrier | host clock | beacon gaps | queue drops | join |
| --- | --- | --- | --- | --- |
| 50 (25 ms, melonDS) | 0.075x | median 2808 ms | 0 | reaches two racers |
| 6 (3 ms) | 0.284x | **none, max 227 ms** | 69 | did not hold |

A long barrier delivers multiplayer traffic but stalls the host, stretching its
beacon interval until the client loses the network. A short barrier keeps
beacons perfect but the exchange suffers. melonDS does not face this because its
emulation runs at full speed and 25 ms is a worst case rarely reached; here the
host reaches it often enough to lose most of its clock. The tree is left at 50,
which gets furthest.

Remaining differences from melonDS DS, in the order worth trying:

1. **Apply the reply staleness check.** `DWIFI_REPLY_STALE_US` is defined and the
   timestamp is now on the wire, but nothing yet rejects a reply older than the
   command it answers. Both references do this.
2. **Unicast replies to the host** rather than broadcasting, learning the host id
   from received command packets, as both references do.
3. **Find why the host reaches the 25 ms timeout at all**, now that replies cross
   a datagram channel with a 3 ms round trip. That, rather than the barrier
   length, is the real anomaly.

### Harness faults found tonight, which invalidate earlier runs

Two macro faults were making runs fail in ways then misread as transport
failures. Both are fixed.

- **The attract mode is a full CPU race, not a still screen**, and `A` leaves it
  for the primary menu. At the old 20 s mark the consoles were not reliably both
  inside it: whichever had already returned to the menu took the `A` as "Single
  Player" and spent the run in the wrong menu tree. The wait is now 32 s. `L`
  was tried as an alternative and does **not** exit the demo, contrary to the
  older note in this document.
- **The menu navigation gap was 300 ms**, too short for Mario Kart's animated
  menu, so the Single Player to Multiplayer `DOWN` was often dropped. It is now
  1500 ms with a `mode-highlighted` capture, so a missed press is visible rather
  than silently misread.

Any earlier run recorded as "failed at discovery" should be treated as suspect:
a material share never reached the multiplayer menu at all.

### Next steps (supersedes the 2026-08-31 list)

0. Done — see above. The suppression diagnostic confirmed the fabrication.
1. Constrain host transmission by its own airtime, so the medium is serialised
   on both sides. The host currently emits without regard to whether the medium
   is busy, while the client's receiver honours per-frame airtime — the host
   out-produces what any station could physically accept. Pacing transmission to
   airtime should remove the rate mismatch at its source, and would make the
   reply barrier's pacing role redundant rather than merely shortened.
2. Then re-measure the collapse. Beacon starvation is fixed and it persists, so
   the remaining suspects are the client's own clock and command delivery.
2. Then re-check the beacon interval itself: 217 ms against a nominal 102.4 ms.
2. Re-measure the collapse and the crawl under the cheapened trace, using
   `wall_ns` against `us`. The crawl is real and observed, but its cause is
   unknown now that the trace has been ruled out.
3. Find why reply staging stops after the opening two-frame burst.
4. Flip `W_RFStatus`/`W_RFPins` ownership and retire the two stub constants,
   as one change with its own verification run. The guest polls that state over
   a thousand times and is still told nothing, and supplying IRQ15 alone was
   enough to start it staging.
5. Implement `W_RXFilter` / `W_RXFilter2`, the other large absent block.
   The guest configures those filters expecting whole classes of frame to be
   dropped; delivering them anyway pushes traffic into its RX ring it never
   asked for. Port the `return` paths from `FinishRX()` directly.
3. Do not force `W_RXCnt` bit 72. Do not force `W_RXCnt` bit 7 or synthesise a payload to move things along.
   The bit is guest-written, and latching it with an empty REPLY1 is a no-op;
   the only way to make it do anything is to invent game data. That is the same
   fabrication as the synthetic default reply, which this section already proved
   hides the fault rather than fixing it.
2. Older thread, now lower priority: find why the client's library never stages. That is
   upstream of everything else here: association completes, the client answers
   nothing, and every downstream symptom follows. Look for what the library
   waits on before providing a reply payload. Two concrete threads: give the
   guest a chance to run between the command's RX interrupt and the reply
   transmission rather than calling `send_mp_reply()` inline, and check whether
   the client believes it holds the AID the host's `mask=0002` polls.
2. Re-time the drop precisely. On the ad hoc link the host still shows player 2
   at the twenty-second checkpoint and an empty slot by sixty seconds. A macro
   sampling every ten seconds across that window would locate the transition and
   let it be correlated against the traces. Note the hook trace caps at 16,384
   records and fills well before the drop, so the window needs either a raised
   cap or telemetry sparse enough to survive it.
3. Only then revisit the start transition, and always immediately after a
   confirmed-stable lobby rather than in a separate later macro run.

## Device addressing

The addresses in the table above are stale twice over. On the house network the
host became `192.168.0.179` and the client `192.168.0.143`. Testing has since
moved to the Netplay.pak ad hoc network documented in the 2026-09-01 section:
client/AP `10.0.0.1` on `wlan1`, host `10.0.0.20`. `/tmp/drastic-wifi-session`
lives in `/tmp` and does not survive a reboot, and its peer addresses must match
whichever network is in use.

## 2026-09-04: the two-staging ceiling was a missing write to the guest

The long-standing signature in this document — the client's wireless library
emits a two-frame opening burst and then only empty defaults, identically in two
unrelated games — has a cause, and it is in this project's code rather than in
Nintendo's library.

`model->packet_ram` is a **shadow**. A write to it reaches DraStic's own packet
RAM, which is what the guest reads through paths the hook does not intercept,
only via `model->write_packet()`. Auditing every model-generated packet-RAM
write in `model.c`, eight of the nine paired their `store16()` with that call.
One did not:

```c
uint16_t previous = reg16(model, REG_TX_SLOT_REPLY2);
if (previous & UINT16_C(0x8000)) {
    uint32_t previous_address = (uint32_t)(previous & UINT16_C(0x0fff)) << 1;
    if (previous_address + 2 <= DWIFI_PACKET_RAM_SIZE)
        store16(model->packet_ram, previous_address, UINT16_C(0x0001));
}
```

That is melonDS's "the packet has been sent" store at offset 0 of the retired
reply buffer, and it was landing only in the shadow. The comment three lines
above it states the requirement it was failing to meet: the ordering matters
"when dswifi prepares the next payload **while inspecting the previous one**".
The inspection result was the one thing never delivered.

A double-buffered writer that must observe a buffer transmitted before reusing
it can get exactly **two** stagings ahead — fill both, then block. That is the
number this document has recorded from the beginning.

### The A/B, because a passing run proves nothing on a ~50% test

Both arms are the same tree with only this one push added or removed, built by
the same toolchain, checksum-verified on both consoles before each arm, three
cold-join runs each.

| | control (`c63d884…`) | fixed (`25f056d…`) |
| --- | --- | --- |
| two-racer check | 2 of 3 | **4 of 4** |
| `0094` writes, per run | 1, 2, 2 | 1, 1, 1, **19** |
| **max stagings** | **2** | **19** |
| populated replies, best run | 2 | **19** |

The control reproduced the historical signature exactly, values and all —
`8000`, `811a`, silence. The treatment's best run staged nineteen times,
alternating `8000`/`811a` throughout, with nineteen populated replies. **The
ceiling of two is what the missing push caused**, and nineteen alternating
stagings is not reachable under the control.

### What this does not show, which matters

**The join rate is not improved by this.** 4 of 4 against 2 of 3 is well inside
the noise of a test this document already records as passing about half the
time. Nothing here should be read as fixing the join.

**The treatment is bimodal.** Three of its four runs staged only *once*. The
fix removes the ceiling; it does not make sustained staging happen every run,
and what selects between the two modes is unknown.

**Two racers is a weak probe of the reply path.** Three passing runs had
essentially no populated reply traffic at all, so the lobby is evidently
reachable on empty default replies alone. Join success and reply-path health are
measuring different things, and pass rate is the wrong instrument for the
latter. Prefer the `0094` staging count.

**Beacon starvation is untouched.** The client's `max_gap_us` sat at 1.18-1.42 s
across every run in both arms — a dozen-odd beacon periods, the same starvation
signature as before, and unchanged by this.

`test_retired_reply_is_marked_sent_for_the_guest()` covers it. It was confirmed
to fail without the push and pass with it, rather than merely observed green.

### Adjacent, not changed

`send_mp_reply()`'s populated branch also ORs bit 0 into offset **+4** of the
buffer it is transmitting. That is not a melonDS behaviour, and `finish_mp_cmd()`
treats offset +4 as a count field, so it may be corrupting one. It was left
alone so this run stayed attributable to a single change.

### Harness

`drastic-wifi/devbridge/run-session.sh` replaces the lost `measure.sh` and
encodes the procedural rules this log learned the hard way: fail fast unless
both consoles are at `foreground=devbridge`, recreate `/tmp/drastic-wifi-session`
(tmpfs, so lost to every reboot — it was missing on both consoles at the start
of this session), and wait for `/tmp` to drain before pulling.

Three faults in its own first version, all of which had precedents in this log:

- passing `--adb` to `run` collides with the `CONNECT_ADB` lines inside the
  `.test` files and aborts the macro on its first line;
- `devbridge.py ... | tee` reports **tee's** exit status, so a macro that
  aborted immediately was recorded as `macro exit: 0`;
- clearing `/tmp` is not enough. A run that never reaches the emulator leaves
  the log directory untouched, and "pull the newest trace" then returns a
  *previous* run's file — the first attempt of this session pulled a 3.3 MB
  trace from 2026-09-02 and would have scored it as live evidence. Traces are
  now archived before each run, so anything present afterwards is from this run.

`mariokart-hold.test` re-checks the racer badge every ten seconds out to sixty.
`mariokart-cold-join.test` stops six seconds after the join and therefore has
never been able to observe the collapse this document describes. Ten-second
`VERIFY` spacing is deliberate: `mariokart-drop-timeline.test` fires 32 captures
at 1.5 s, roughly a 7% duty cycle on the console whose emulation speed is the
thing under measurement.

### The collapse, measured at last, and it is not the reply path

`mariokart-cold-join.test` stops six seconds after the join, so no macro in this
document has ever been able to watch the lobby fail. `mariokart-hold.test`
re-checks the racer badge every ten seconds to sixty. Two runs on the fixed
build:

- both joined cleanly — menu, seeking, room list and the post-join two-racer
  badge all verified at 0.00;
- both lost the client **before the ten-second checkpoint**, aborting on
  `mariokart-hold.test:68 … off by 5.36`.

One of those two runs staged **21 replies** with sustained `8000`/`811a`
alternation. **The reply fix and the collapse are independent faults**, and the
staging fix should not be credited with anything downstream of the join.

The chain is measured on both consoles:

| | |
| --- | --- |
| host beacon interval, host-emulated | 202-213 ms, steady |
| gap the client actually sees | 0.93-1.30 s |
| host MP-phase `advance_us` | 9219 ms |
| of which the reply barrier | **7633 ms (83%)** |

The interval itself is exonerated: the guest writes `W_BEACONINT = 0x01f4`
(500), and beacons leave more often than that, not less. What stretches the gap
is the host's wall clock, and the barrier is where it goes.

### The barrier trade survives the reply fix, and delivery is the reason

Re-tested properly. The barrier length is now read from
`DRASTIC_WIFI_BARRIER_ATTEMPTS`, defaulting to the compiled-in 50, so both arms
run the **same checksummed library** and differ by a launch variable — earlier
passes at this question rebuilt between arms, which cannot rule out an unrelated
change riding along.

| barrier | held | barrier ms | beacon max gap | queue dropped | host tx / client rx |
| --- | --- | --- | --- | --- | --- |
| 6 | **never joined** | 1803 | 448 ms | 331 | 1240 / 288 — **23%** |
| 6 | **never joined** | 3161 | **251 ms** | 716 | 1985 / 233 — **12%** |
| 50 | post-join, drops by 10 s | 8502 | 1444 ms | **8** | 2185 / 1990 — **91%** |

Shortening the barrier fixes beacon starvation outright — 251 ms against
1444 ms, comfortably inside the programmed interval — and destroys command
delivery, which collapses from 91% to 12%. The join never completes.

**The barrier is not a protocol device at all; it is a throttle matching host
production to what the client's receive pipeline accepts.** That was already
suspected here; the delivery ratios measure it directly, and the reply fix does
not change the trade.

Note the client's queue is only 8 deep and reaches `high_water=8` in every run
of both arms, so depth is not what distinguishes them — at barrier 50 the host
simply never gets far enough ahead to overflow it.

### Next: pace host transmission by emulated airtime

This is the structural fix already named in this document's next-step list, and
it now has direct evidence behind it. The host transmits without regard to the
medium being busy while the client's receiver honours per-frame airtime through
`rx_active`, so the host over-drives a medium the client is obliged to respect.
The wall-clock barrier is compensating for that asymmetry by stalling the host
by roughly the right amount — and paying for it in beacon interval.

Constraining the host's transmit path to its own emulated airtime should remove
the mismatch at its source and make the barrier's pacing role redundant, at
which point it can be shortened to the melonDS 25 ms worst case without
delivery collapsing. That is the single change most likely to hold the lobby.

Do not read the barrier=6 beacon numbers as progress on their own: they are what
the beacon stream looks like when the host is free, and they come with a link
that is not delivering.

### Medium contention is modelled now, and it changes nothing

Implemented as the previous section proposed: `fire_tx()` refuses to transmit
while the air is occupied and leaves the slots armed, `dwifi_model_advance_us()`
retries once it clears — hardware defers, it does not discard — and `fire_tx`,
`fire_beacon` and `send_mp_reply` all occupy the medium for melonDS's
`192 + length * 4` us at 2 Mbit/s, eight per byte at 1.

The beacon deliberately occupies the medium but is never blocked by it.
Hardware reserves the air for it at the target beacon time, and deferring the
frame that keeps a station associated is precisely the starvation measured
above. `test_beacon_is_not_blocked_by_a_busy_medium()` asserts that;
`test_medium_serialises_transmission()` covers the deferral and, importantly,
the retry — `fire_tx()` runs only from the guest's register writes, so without
it a deferred frame is lost rather than delayed.

**It has no measured effect, and the reason should have been checked first.**

| | host tx | over | rate | medium busy |
| --- | --- | --- | --- | --- |
| barrier 50 | 2605 | 57.6 s | 45 frames/s | **1.58%** |
| barrier 6 | 1274 | 39.5 s | 32 frames/s | **1.13%** |

The air is idle 98-99% of the time. Airtime pacing constrains essentially
nothing, so it cannot have been what limited delivery, which is unchanged:
94.2% at barrier 50 against 91% before, 24.3% at barrier 6 against 12-23%
before. Both are noise.

The "host over-drives a medium the client is obliged to honour" hypothesis —
recorded in this document as the most promising remaining fix — is **refuted**.
Both consoles emulate near-identical `wifi_us`, the frame rate is two orders of
magnitude below what the medium carries, and the receiver was never the
bottleneck. Compute the utilisation from `tx_frames` and `wifi_us`, both already
in every summary line, before theorising about contention again.

The change is kept: it is physically correct, it costs nothing measurable, and
it removes a real divergence between the transmit and receive sides. It earns no
credit for anything.

### A deep queue buys perfect delivery and loses the join

The other half of the same theory: if the client's queue overflows while the air
is idle, give it more room. Depth is now settable at runtime
(`DRASTIC_WIFI_MP_GENERATIONS`, array sized to a 256 maximum) so this needs no
rebuild.

| depth / barrier | delivery | high water | dropped | **stale_gen** | lobby |
| --- | --- | --- | --- | --- | --- |
| 8 / 50 | 94% | 8 | 2 | **70** | reaches two racers |
| 256 / 6 | **100%** | 80 | 0 | **796** | **never joins** |
| 256 / 6 | **100%** | 57 | 0 | **537** | **never joins** |
| 256 / 50 | **100%** | 12 | 0 | **95** | **never joins** |

Frame loss is eliminated outright — 100% delivery, zero drops, and the depth was
genuinely used, with a high water of 80. **And every arm failed at the first
post-join check**, including depth 256 at barrier 50, which at depth 8 reaches
two racers reliably.

Staleness says why: evictions rise from 70 to 537-796. The deep queue does not
remove the backlog, it stores it, and the 16 ms arrival-age sweep then throws it
away — generations arrive too late to be worth anything. This is bufferbloat in
a protocol whose payload has a shelf life. Depth converts drops into latency,
and latency is the thing that cannot be traded here.

That also bounds the useful depth for good: with a ~3 ms round trip against a
16 ms staleness window, only a handful of generations can ever be in flight
before the sweep bites, so **8 is already about right**. The earlier note that
"the queue fills to whatever depth it is given" was correct, and re-testing it
produced a worse result. Reverted to 8; the runtime knob is kept for future
attribution.

### Where this leaves the collapse

Two structural hypotheses are now refuted by measurement rather than argument,
which narrows things usefully:

- it is **not** medium contention — the air is 99% idle;
- it is **not** queue capacity — unlimited capacity makes delivery perfect and
  the join worse.

What remains is what the architecture review called the deferred risk, and it is
now the only candidate left standing: two emulators with independent clocks
needing a tight round trip. The barrier couples them and costs the host its wall
clock; every alternative to it either starves delivery (short barrier) or
delivers stale data (deep queue). The host's stall is what stretches its beacon
interval to 1.2-1.4 s and loses the station.

Do not reach for throughput again. Both remaining levers are latency: reduce
what the host waits for, or stop the host's beacon obligation from being frozen
by that wait.

## 2026-09-04: the lobby dies on a fixed budget of emulated time

Every measurement of "how long the lobby held" in this document, including the
ones added earlier today, is confounded. `mariokart-hold.test` checks the racer
badge on a **wall-clock** schedule while the failure is bounded by **emulated**
time, and the host's emulation rate varies by a factor of three between
configurations. Improving the wall-clock number by slowing the host down is not
progress, and that is exactly what happened.

The multiplayer phase, measured host-side from the first to the last `mp-state`
line, across twelve runs spanning every configuration tested today:

| run | MP emulated (s) | MP wall (s) | ratio |
| --- | --- | --- | --- |
| hold1 | 1.46 | 10.23 | 0.142 |
| hold2 | 1.90 | 12.93 | 0.147 |
| b50a | 1.52 | 11.63 | 0.131 |
| m50a | 1.78 | 11.51 | 0.154 |
| bibona | 1.78 | 11.24 | 0.158 |
| bibonb | 1.56 | 13.91 | 0.112 |
| biboff | 1.52 | 10.53 | 0.145 |
| rc1 | 1.46 | 11.05 | 0.132 |
| rc2 | 1.65 | 12.28 | 0.135 |
| w4a | 1.66 | **30.54** | 0.054 |
| w4b | **2.48** | **43.87** | 0.057 |
| w16a | 1.74 | **32.39** | 0.054 |

**Emulated duration is 1.46-2.48 s in every single run. Wall duration ranges
from 10.2 s to 43.9 s.** The configurations behind those rows include barrier 50
and 6, queue depth 8 and 256, beacons serviced in the barrier and not, the
credit window at 4, 16 and off, and medium contention present throughout. None
of it moves the emulated number.

The three window runs "held past the ten-second checkpoint", one of them to
twenty seconds — further than anything in this document. That was the host
running at 0.054x instead of 0.132x, stretching the same one-and-a-half seconds
of emulated life across two and a half times as much wall clock. It is a
measurement artifact, not a fix.

**What this rules out.** The failure is invariant to every transport parameter
available, which is strong evidence it is not in the transport at all. Frame
delivery, queue capacity, medium contention, beacon cadence and barrier length
have now each been varied across their useful range, several of them by a
factor of thirty, with no effect on the emulated lifetime of the lobby.
Something in the emulated hardware or in the guest's own state machine gives up
after roughly ninety to a hundred and fifty frames of multiplayer.

**Consequences for measurement.** Report the MP window's emulated duration as
the primary result from here. The wall-clock checkpoints in
`mariokart-hold.test` are still useful as a pass/fail on whether a join
happened at all, but they must never again be quoted as a measure of stability
without the ratio beside them.

Note also that `rc1` staged 192 populated replies — 21% of all replies, which is
about one per emulated frame against a command rate near 600/s, and plausibly
correct behaviour — and died at 1.46 s just the same as `rc2`, which staged two.
Reply content does not discriminate either.

### The credit window never engaged, and the bug is instructive

The window was meant to let the host run N generations ahead rather than
blocking on every transfer. It reported `window_credits=4` with a window of 4,
and `window_credits=16` with a window of 16 — the credit was taken exactly N
times and never again — while `barrier entries=935 timeouts=935` says every
subsequent entry ran to a full timeout, tripling the barrier's cost.

`mp_replies_in` was incremented at the barrier's own two recognition points,
inside `await_for_peer()`. But this document already records that the host runs
slower than the client and that **a reply is usually consumed by an ordinary
`dwifi_transport_poll()` before the model reaches its reply deadline** — the
same fact the barrier's short-circuit exists to exploit. So most replies were
never counted, `mp_outstanding()` grew without bound, the exit condition became
unsatisfiable, and the window turned the barrier into an unconditional
full-length stall.

Counting now happens in `process_incoming_packet()`, where a reply is actually
ingested, on every path. The window arm above is therefore **not** a test of the
window idea; it is a test of a broken implementation. Given the invariance
result it is left off by default, and the idea remains untested.

## 2026-10-23: superseded by DSperate's radio

The hand-written model and transport described in this log are no longer the
shipped library. drastic-wifi now carries DSperate's port of melonDS's Wi-Fi
block and LAN transport, attached at the boundaries this log established (see
`docs/drastic.md`). The fixed emulated-time budget above was the client's
clock running free of the host's: melonDS locks the client to the host's
timestamps and blocks it for each host frame, which the model never did. With
the port, Mario Kart DS gets past CUT OFF and races, with DraStic as host and
as guest.
