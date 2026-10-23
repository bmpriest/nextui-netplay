# Link-timing optimizations drawn from the GB-Link project

## What this is

A reading of the [GB-Link](https://github.com/GB-Link) family of repositories —
`GBLink-Firmware`, `gb-pokemon-web`, `gb-tetris-web`, `gb-drmario`, `gblink-pro`,
`gblink-netplay-bridge` — against this codebase, and a plan for the parts that
actually apply.

## Scope, honestly

GB-Link solves a different problem than Netplay.pak. It drives *real* Game Boy
hardware over a *physical* link cable from an RP2040, and its central trick is
that the adapter is always the clock master while the console is always the
serial slave. That lets it burst a byte out at ~986 kHz and then insert a
precisely timed dead gap, so the console's shift register sees a 120x overclock
while the *game* sees its native ~977 µs byte cadence.

None of that transfers. We have no wire, no clock to own, and no shift register.
Both consoles here are software, and the timing that matters is when emulated
serial data becomes available to a core relative to the frontend's frame loop.

What does transfer is the layer above the wire — how GB-Link keeps a
latency-bound link from stalling, and how it decides what to send when the
network is late. Four ideas survive translation, and this plan covers those.
Two more are recorded at the end as deliberately rejected, because writing down
what was considered and dropped is worth as much as the backlog.

It is also worth stating plainly: the instanced-link design in
`docs/multi-instance.md` already independently arrived at GB-Link's single most
important architectural conclusion — keep the cable local and put only inputs on
the network. Tetris, Dr. Mario and Yoshi are online on real hardware for exactly
that reason (the server pre-shares the RNG sequence and the bottle layout so
nothing needs per-frame sync). The items below are refinements to the paths that
still carry serial traffic over Wi-Fi, not a redesign.

## Summary

| # | Item | Path affected | Complexity | Expected gain | Status |
|---|---|---|---|---|---|
| 1 | Honour `poll_receive` mid-frame | network serial | Low–Medium | Up to one frame (~16.7 ms) removed per serial exchange | **done** |
| 2 | Pacing telemetry for the link path | network serial | Low | Makes items 1, 3, 4 measurable; currently blind | **done** |
| 3 | Bounded starvation instead of open-ended blocking | network serial | Medium | Turns an indefinite freeze into a reported, recoverable stall | **done** |
| 4 | Negotiated input delay from measured RTT | instanced link, shared screen | Medium | 1–3 frames (17–50 ms) of input lag on ad hoc | **done** |

All four are implemented. The "What is there now" sections below describe the
code before these changes, and their `shim/shim.c` line numbers refer to that
version, not the current file.

Do them in that order. Item 2 is the cheapest and makes the rest arguable
instead of speculative; it is listed second only because item 1 is the one worth
doing regardless.

## What changed on implementation

Two of this plan's assumptions did not survive contact with the code, and both
mattered enough to record.

**Item 4's RTT source did not exist.** The plan proposed timing the existing
`CMD_PING`/`HEARTBEAT_MS` exchange — "recording a timestamp and a subtraction,
not new traffic". `CMD_PING` is never echoed (`netlink.c`: *"needs no handling
beyond refreshing last_rx"*), so there was no round trip to time; and it is only
sent when the link is otherwise idle (`ms_since(&nl.last_tx) > HEARTBEAT_MS`),
so during a session carrying per-frame input it never fires at all. Implemented
instead as `CMD_RTT_PROBE`/`CMD_RTT_ECHO`, echoed on the transport thread so the
figure measures the link rather than either side's frame loop. Nine bytes ten
times a second, and it runs continuously rather than only when idle.

**Item 4's formula was sized from the wrong statistic.** The plan proposed
`ceil(rtt_ms / 16.7) + 1` from a smoothed RTT. Checked against the measurements
already recorded in `app/netsetup.h`, taken on this A30/Brick pair:

| link | RTT min/avg/max | hand-tuned | from median | from max |
|---|---|---|---|---|
| ad hoc | 1.7 / 4.3 / 21.5 ms | 3 | 2 | **3** |
| via AP | 6.2 / 26–43 / 118–300 ms | 10 | 4 | **9–19** |

A median-derived window proposes 4 frames for an access-point link that was
measured as needing 10, and `netsetup.h` already records what that costs:
*"Applying that same 3 over the access point gave 33-47fps and 63-74% stalled
frames, because a 50ms budget cannot absorb a 300ms spike."* The delay is
therefore derived from the **maximum** observed round trip over a rolling
window, which reproduces both hand-tuned constants. The median is still
measured and logged, because it is the honest description of the link.

**`input_delay=N` is the transport floor when automatic measurement is on.** A
one-sided pin would break the exact-agreement invariant, so both sides still
adopt the higher of their two values. The app writes `input_delay_auto=1`, which
allows measured RTT to raise its transport-aware baseline but never lower it.
H700 testing showed why: two quiet startup samples proposed 2 frames on ordinary
Wi-Fi, then the session averaged 54.7 FPS with roughly 2,400 stalls per device.
An old session file carrying only `input_delay=N` remains an exact pin.

The test that asserted mismatched delays are *refused* now asserts that they
*converge*, and additionally that both peers then run an identical input stream
— which is the property the refusal existed to protect.

---

## 1. Honour `poll_receive` mid-frame

### What is there now

`shim_netpacket_poll_receive()` at `shim/shim.c:1181` is an empty function:

```c
// The core may call this to read mid-frame rather than waiting for the next
// poll. Receiving happens on the netlink thread, so there is nothing to pump
// here - packets are already queued and will be delivered by deliver_packets().
static void shim_netpacket_poll_receive(void) {
}
```

The comment is half right. Packets genuinely are already queued — the netlink
worker thread reads the socket independently, which is the good part of the
design. But they are only *delivered to the core* by `pump_netpacket()`
(`shim/shim.c:2379`), which runs once per frame at the top of `retro_run`
(`shim/shim.c:3457`). A core that calls `poll_receive` from inside its own
`retro_run` — which is the entire reason the libretro netpacket interface has
that callback — gets nothing back, and must return to the frontend and wait for
the next frame boundary before it can see a byte that has been sitting in our
queue for 16 ms.

### Why it matters

This is GB-Link's most-repeated lesson in a different costume. `PacedEmitter`
delivers on a 15 ms cadence because that is what gpSP consumes
(`gblink-netplay-bridge/src/engine/types.ts:178-182`); the GB firmware's raw
relay flushes partial buffers every 10 ms rather than waiting for a full
64-byte packet (`GBLink-Firmware/src/sections/rawRelaySection.cpp:59-67`). In
both cases the rule is: *deliver at the cadence the consumer needs, not the
cadence that is convenient for the producer.* Our producer cadence is the video
frame, and for a serial exchange that is the wrong clock entirely.

The cost is worst exactly where this project is most sensitive: a Gen 3 trade or
a Gen 1/2 party exchange is hundreds of sequential serial transactions. If each
one eats a frame boundary that it did not have to, a trade that should take two
seconds takes ten, and it feels like the sluggishness the instanced-link work
was written to escape.

### Change

- `shim/shim.c:1181` — implement the body: drain `NetLink_popPacket` into
  `core_netpacket.receive`, subject to the same `MAX_PACKETS_PER_FRAME` budget
  (`shim/shim.c:60`) shared with `pump_netpacket` so a core that polls
  aggressively cannot starve the frame.
- `shim/shim.c:2379` — factor the drain loop currently inlined in
  `pump_netpacket` into a `deliver_packets(int budget)` helper that both call.
  The comment at 1181 already refers to `deliver_packets()` by name, so the
  intended shape was there; only the function is missing.
- Add a re-entrancy guard. `poll_receive` is called from *inside*
  `core_netpacket.receive`'s own call stack in the pathological case, and a core
  that calls `poll_receive` from within its `receive` handler must not recurse.
  A static depth counter that makes the nested call a no-op is sufficient and
  costs one branch.

### Complexity

**Low–Medium.** The drain logic already exists and is correct; this is
extraction plus a guard. The medium half is entirely reentrancy review: confirm
that `NetLink_popPacket` is safe to call while the emulator thread is inside the
core (it is — the queue is worker-filled and consumer-drained under the netlink
mutex), and that no core we ship re-enters `send` from `receive`.

### Gain

Up to one frame per serial exchange on the network-serial path. Nothing on the
instanced-link path, which does not use netpacket for serial traffic.

### Risk

Low, and bounded by the budget. The failure mode to watch for is a core that
polls in a tight loop and now always finds a packet, never returning to the
frontend — hence keeping the per-frame budget shared rather than per-call.

### Verification

`shim/test/link.sh` with a fake core that calls `poll_receive` between sends;
assert it observes a packet without an intervening `retro_run`. Then a real
Gen 1 trade, timed end to end, before and after.

---

## 2. Pacing telemetry for the network-serial path

### What is there now

Two of the three session paths report how they are actually pacing:

- shared screen — `netplay_reportPacing()` at `shim/shim.c:2261`, called from
  `shim/shim.c:3563`
- instanced link — `dual_report_pacing()` at `shim/shim.c:3236`, called from
  `shim/shim.c:3448`

The third — the plain network-serial link branch at `shim/shim.c:3565` — reports
nothing. It is the path that carries actual GB/GBA serial traffic over Wi-Fi,
which makes it the one whose timing is most fragile and the only one we cannot
see into.

### Why it matters

Every timing constant in GB-Link's firmware was measured, not derived.
`GBLink-Firmware/analyser.py` exists solely to post-process Saleae "Async
Serial" captures in 18-line chunks — 9 word pairs, exactly one protocol round —
and the constants in `packetLayer.hpp:52-54` (30097 / 1378 / 12953 PIO
iterations) were read off those captures of real console-to-console traffic.

We cannot put a logic analyser on a Wi-Fi link, but the discipline is the point.
The existing `netplay_reportPacing` comment already makes the argument better
than I can:

> Attribution is the whole point: fps well under the core's own rate means this
> device cannot keep up, while a high stall share with healthy fps means we are
> waiting on the peer. Both feel like "lag" and the fixes are opposite.

That is exactly as true on the link path, where we currently have no attribution
at all.

### Change

- `shim/shim.c:3565` — add a `link_report_pacing()` on the same
  `PACING_INTERVAL` (`shim/shim.c:72`) cadence as the other two, reporting:
  frames, packets delivered, packets dropped (`NetLink_droppedPackets()` is
  exported at `shim/netlink.h:249` and defined at `shim/netlink.c:1231`, but has
  no caller anywhere in the shim today — the "desync warning sign worth logging
  rather than hiding" in its own comment is currently being hidden), peer-paused
  frames, and queue high-water.
- Add a histogram of **time between successive netpacket deliveries** — the
  closest thing we have to GB-Link's inter-word interval. Four buckets
  (<1 frame, 1–2, 2–4, >4) is enough to distinguish "the link is flowing" from
  "the link is frame-locked", which is precisely the before/after signal item 1
  needs.

### Complexity

**Low.** It mirrors two functions that already exist, and the counters it needs
are mostly already maintained or already exported.

### Gain

No runtime gain. It is what makes items 1, 3 and 4 defensible rather than
plausible, and it is the difference between "trading feels slow" as a bug report
and a number.

### Risk

None beyond log volume. `PACING_INTERVAL` is 600 frames, so roughly one line
every ten seconds.

---

## 3. Bounded starvation instead of open-ended blocking

### What is there now

`NetLink_setCoreRunning()` (`shim/netlink.h:83`, call sites at
`shim/shim.c:1495`, `2892`, `3226`, `3591`) deliberately excludes time spent
inside the core from stall accounting, and the header explains why:

> Time spent inside it is not a frontend stall, however long it lasts: a
> link-capable core may legitimately block there waiting on its peer. Reporting
> that as a stall makes the peer pause, which removes the very data the blocked
> core is waiting for - a deadlock.

That reasoning is correct and should not change. But it has a consequence: a
core that blocks in `retro_run` waiting for a serial byte that will never arrive
blocks for as long as it likes, and we neither observe it nor bound it. The
frontend appears frozen and the session has no way to say why.

### Why it matters

GB-Link never lets a link stall, and it does it in two layers.

The firmware layer keeps a jitter buffer and substitutes a benign filler word
when it runs dry — `0x00` in `usbLinkCommand.cpp:30` against a 200-deep queue at
`usbLinkCommand.cpp:13`, `0x7FFF` (CMD_NONE) in `rawRelaySection.cpp:91`. The
console's link keeps its cadence and the game reads "nothing to report".

The protocol layer bounds the wait explicitly. `GSCTrading.js` forces a transfer
if 800 ms pass without one (`MAX_MS_BETWEEN_TRANSFERS`, `GSCTrading.js:2925`,
`:2950`), feeds `NO_INPUT` (0xFE) when the peer's byte has not arrived
(`:2960`), counts it as a dropped byte, and aborts loudly after 20 such drops
(`:2970-2974`) rather than hanging.

We cannot inject a filler byte from the shim — the serial framing lives inside
the core, and forging protocol bytes for gambatte or gpSP from outside would be
exactly the "forge 0x00 protocol bytes forever" mistake that
`gb-pokemon-web`'s own `failedRead()` refuses to make
(`TradingProtocol.js`, `failedRead`). What we *can* do is the second half:
observe the wait, bound it, and fail it into the existing recovery machinery
instead of into a frozen screen.

### Change

- `shim/shim.c` — timestamp immediately before and after each link-core
  `retro_run`; if one continuous call exceeds 5000 ms while the peer remains
  connected, route into the existing recoverable-failure path with a message
  naming the blocked call.
- Do not inspect queue occupancy. H700 gpSP testing proved that an empty queue
  can persist through normal 59.5 FPS menu navigation; accumulating that silence
  produced a false failure every five seconds.
- Do **not** send `CMD_PAUSE` on this path. The header's deadlock argument
  stands; this is detection and reporting only.

### Complexity

**Medium.** The mechanism is small, but the threshold is a judgement call and
needs device testing. Queue state is intentionally excluded: returning from
`retro_run` proves the core was not blocked, even if no packet arrived.

### Gain

No throughput gain. It converts the worst failure mode on this path — an
indefinite freeze with no explanation — into a bounded, named, recoverable one.
Item 2 is a prerequisite for choosing the threshold from data rather than taste.

### Risk

Low for quiet games because silence no longer advances the watchdog. A core
call that really takes longer than the threshold enters recovery after it
returns; the shim cannot safely interrupt arbitrary core code mid-call.

---

## 4. Negotiated input delay from measured RTT

### What is there now

`INPUT_DELAY_DEFAULT` is 6 (`shim/shim.c:66`), overridable per session via
`input_delay=N` in the session file (`session_input_delay`, `shim/shim.c:650`,
clamped 1–20). Both sides must agree exactly, and the handshake enforces it —
`shim/shim.c:1827` for shared screen, `shim/shim.c:3058` for instanced link.

The comment at `shim/shim.c:62-65` is already the right analysis: each frame
buys ~16 ms of tolerance at the cost of input lag, and the tradeoff is
session-specific. The gap is that nothing *measures* which side of the tradeoff
a given pair of devices is on. The value is a compile-time default that a user
must know to override by editing a file.

### Why it matters

Six frames is 100 ms of input lag. On an ad hoc link — which the README already
identifies as the best-performing configuration — the RTT may well support 3,
and 50 ms of recovered input latency is the difference between a Pokémon battle
feeling native and feeling remote.

GB-Link's equivalent is `MAX_TOLERANCE_BYTES = 3` with `SAFETY_AMOUNT = 1`
(`gb-pokemon-web/src/services/GSCTrading.js:2922-2923`): a window sized to
absorb jitter, deliberately kept as small as it can be, with a documented
comment about what it buys —

> Network latency adds LAG, not per-byte cost, so a section takes ~length ×
> pacing instead of length × round-trips.

The tuning direction there is the same as ours: pick the smallest window the
link will tolerate, and have a defined behaviour for when it is exceeded. We
already have the second half — the stall path at `shim/shim.c:3401-3420` handles
a missing remote input correctly. We are missing the first.

### Change

- `shim/netlink.c` — time the existing `CMD_PING` / `HEARTBEAT_MS`
  (`shim/netlink.c:41`) exchange to produce a smoothed RTT estimate. The
  heartbeat already runs; this is recording a timestamp and a subtraction, not
  new traffic.
- `shim/shim.c` handshake (`1827`, `3058`) — carry the proposed delay in
  `NetLinkSessionIdentity` (it already has an `input_delay` field, and both
  sides already refuse to proceed when they differ). Change the rule from
  "must match" to "both propose, both adopt the higher" — which preserves the
  exact-agreement invariant that the timeline priming depends on, while letting
  the pair settle on a value neither had to be told.
- Treat the app's `input_delay=N` as the transport floor when it also writes
  `input_delay_auto=1`: measurement may raise but never lower it. A session file
  without the automatic flag still pins the exact value for tests and manual
  overrides.
- Choose from measured RTT with a floor of 2 and the existing ceiling of 20:
  roughly `ceil(rtt_ms / 16.7) + 1`.

### Complexity

**Medium.** The transport change is small and the identity field already exists.
The care is all in the invariant: both sides must end at the *same* number,
derived deterministically, before any state transfer is accepted. Adopting the
higher of two proposals is order-independent and needs no extra round trip,
which is why it is preferable to a negotiation protocol.

### Gain

1–3 frames (17–50 ms) of input lag on a good ad hoc link, on both the
instanced-link and shared-screen paths. Nothing on a poor link, where it will
correctly choose the current value or higher — which is itself a gain, since
today a bad link silently produces stalls rather than raising the delay.

### Risk

Medium. Getting it wrong desyncs a session at bootstrap rather than degrading
it. Mitigations: the handshake already refuses mismatched delays, so a bug
manifests as a clean refusal rather than a divergence; and the file override
gives a way to pin a known-good value while testing.

### Verification

`shim/test/twoinstance.c` with injected latency; assert both sides derive the
same delay across a range of RTTs including asymmetric ones. Then the A30/Brick
pair on ad hoc, comparing `dual_report_pacing` stall percentages at the
negotiated value against the fixed 6.

---

## Considered and rejected

**Sub-frame serial delivery to the core.** GB-Link's firmware reproduces exact
inter-word intervals — 30097 PIO iterations (~16.3 ms, one video frame) between
handshakes, 1378 (~747 µs) between the eight words of a block
(`GBLink-Firmware/src/layers/packetLayer.hpp:52-54`), fed per-transmission
through the PIO FIFO so the gap is hardware-timed and jitter-free
(`linkLayer.h:25-29`). The equivalent here would be servicing emulated serial at
sub-frame granularity, which is what mGBA does with per-console `mCoreThread`s
and a cycle-accurate lockstep coordinator. `docs/multi-instance.md` already
worked through this and reached the right conclusion: it requires a coordinator
that can yield and resume an emulator mid-frame, gambatte's `SerialIO::send()`
is synchronous, and neither mGBA libretro build exposes any of it. Item 1 gets
the cheap 90% of the benefit; the rest is a core rewrite.

**A shim-side filler byte when the peer is late.** The direct analogue of
`0x00`/`0x7FFF`/`0xFE` filler injection. Rejected because the shim sits below the
serial protocol, not inside it: it moves opaque netpacket payloads and has no
idea what a valid idle word looks like for gambatte's GameLink or gpSP's RFU. A
filler that happens to collide with a real protocol value corrupts a trade
silently, which is strictly worse than a stall — and GB-Link itself guards
against exactly that collision in two places
(`RESERVED_CMD_WORDS` in `gblink-netplay-bridge/src/engine/bridge/pumps.ts:16`,
and the `0xFE → 0xFF` laundering in `GSCTrading.js:2943`). If filler is ever
wanted it belongs in the fork, next to the code that knows the protocol.

## Note on the fork

The gpSP RFU queue was already deepened from 4 to 16
(`cores/patches/gpsp-002-rfu-queue-size.patch`) for precisely the reason
GB-Link's firmware keeps a 200-deep packet queue: TCP delivers in bursts and the
game drains at its own pace, so the buffer between them has to absorb the
difference. That patch and this plan are the same idea applied at two different
layers, which is a good sign that the layer boundaries are in the right place.
