# State synchronisation

Shared-screen netplay starts by copying one device's emulator state to the
other, then keeps both in lockstep and periodically compares a hash of that
state to check they have not drifted apart.

Three assumptions in that sentence are false for at least one core we ship. Each
one produced a desync, each was hidden behind the one above it, and together they
cost more debugging time than everything else in this pak combined. All three are
now handled; this is what they were and how to test for them.

Everything here was measured with `shim/test/statecheck.c` on a Miyoo A30
(`my282`, armv7) and a Trimui Brick (`tg5040`, aarch64) running picodrive
`2.01-strict-portable`, Streets of Rage 2, `serialize_size` 678519.

## The three false assumptions

### 1. serialize/unserialize is not a fixpoint

`serialize(unserialize(x))` does not reliably equal `x`.

```
roundtrip: DIFFERS 1/678519 bytes, first at 140429      (cold boot + 60 frames)
roundtrip: DIFFERS 649/678519 bytes, first at 537088    (mid-session)
roundtrip: exact                                        (other points)
```

Whether it holds depends on *when* you serialize, so observing `exact` once
proves nothing. Worse, the difference is **live emulator state**, not cosmetic
padding — it grows:

```
amplify: after 1800 more frames, host=da7ecfa7 client=1bcef0e4
amplify: DIVERGED 34/678519 bytes, first at 21
```

One byte at the handshake becomes 34 bytes of genuine divergence within 30
seconds, which is eventually visible on screen.

**Consequence:** the host cannot keep its own state and send a copy. Whatever it
sends, it must adopt itself.

### 2. Only the client needs to load the transferred state

Follows from the above, and it was the original bug. The host serialized, sent,
and carried on from what it already had; the client loaded those bytes. Two
different states from frame 0.

**Fix:** after sending, the host calls `unserialize` on the same buffer. Both
sides then hold `unserialize(A)`, which is well defined because:

```
preframes=0     post-load hash = aae74e79
preframes=3000  post-load hash = aae74e79
preframes=9000  post-load hash = aae74e79
```

`unserialize` **is** a pure function of its bytes — the result does not depend on
what the emulator was doing beforehand, even after 9000 frames of divergent
history. That is the property the fix rests on, and it is worth re-testing for
any new core.

### 3. retro_serialize is not side-effect free

The one that took longest to see, because the divergence check was causing it.

```
serprobe: after 900 frames, clean=15d20df3 probed=fe01ce4a
serprobe: DIFFERS 25/678519 bytes, first at 66007
```

A single extra `serialize()` call costs 25 bytes of divergence within 900 frames.

The divergence check used to hash **asymmetrically**: the host every
`HASH_INTERVAL` frames unconditionally, the client only when it reached the frame
a host hash described. A hash that arrived late was recorded as a missed check —
meaning the host had serialized and the client had not.

The signature is unmistakable once you know it:

```
in sync at frame 300
in sync at frame 600
missed sync check for frame 900     <- only the host serialized here
DESYNC at frame 1200                <- and never recovers
```

**Fix:** both sides hash unconditionally at the same frames, and the comparison
is decoupled from the hashing. The client keeps a small ring of its own hashes so
a late peer hash still finds its frame.

The same trap applies to any diagnostic. The round-trip check added while chasing
this did an extra client-side `serialize` the host never made — the instrument
perturbing the thing it measured. It now re-loads the buffer afterwards to undo
its own footprint.

## Invariants for the shim

Anything touching state during a session must preserve these:

1. **Outside an authoritative-state transaction, both sides call `serialize`
   the same number of times, at the same frames.** Not "roughly" — exactly.
2. **Both sides reach a shared starting state via the same `unserialize` call.**
   Sending bytes one way is not enough.
3. **Diagnostics are not free.** A probe that serializes must undo itself, or run
   on both sides identically.
4. **Every authoritative transfer ends with both sides loading the same blob.**
   This erases serialization side effects on the host and makes the recovered
   state independent of either device's prior trajectory.
5. **Frontend save states are unavailable while a session is armed.** The shim
   still calls the wrapped core's serialization functions directly for initial
   synchronization and acknowledged desync recovery.
6. **Only the host persists shared-screen SRAM/RTC.** Every authoritative blob
   includes the raw save-memory regions; the guest uses them in memory but
   exposes no persistent memory to its frontend.

## Identity, confirmed checkpoints, and process rejoin

These mechanisms apply only to shared-screen lockstep netplay. Link-cable play
continues to exchange the cores' native serial/RFU traffic and never loads a
peer emulator state.

Before accepting any state on each TCP connection, both sides require an exact
match on the SHA-256 of loaded ROM content (including files referenced by
CUE/M3U), core identity, serialized-state size, SRAM size and RTC size. A
mismatch fails closed before either core advances or calls `unserialize`.
Protocol version 7 makes an
older shim fail the greeting rather than misread these messages.

Every 300 frames the host retains the exact authoritative bundle it hashed as a
*candidate*. The bundle contains serialized state plus raw SRAM and RTC, so a
checkpoint or rejoin does not depend on whether a particular core happens to
include persistent memory in `retro_serialize`.
The guest hashes at the same frame and reports agreement. Only an exact
frame/hash match promotes the candidate, using write + `fsync` + atomic rename
to `/tmp/netplay-host-<session-id>.state`. A new arm generates a new session ID,
so an earlier play session cannot be resumed accidentally. The file carries the
ROM hash, core identity, state size, frame and a checksum, all revalidated before
loading. `/tmp` survives an emulator-process crash but not a reboot and avoids
SD-card writes.

On guest restart, the still-running host sends its current state. On host
restart, the new host loads its last confirmed checkpoint and sends that same
blob to the surviving guest; both may rewind by at most 300 frames. Both paths
use the BEGIN/state/ACK/COMMIT barrier, reset queued input, and jump the frame
namespace so delayed packets from the abandoned timeline cannot collide.

## A bug that hid the evidence (protocols before version 6)

The core-identity hash was sent as `CMD_HASH` frame 0. `netplay_checkDivergence`
also sends a state hash at frame 0, since `0 % HASH_INTERVAL == 0`. Only the most
recent hash is retained, so the client compared whichever arrived last — an
identity against a state hash.

Every `core identity differs` message logged before this was fixed is
meaningless. It caused a picodrive rebuild that was not needed for the reason it
was done. Identity first moved to a reserved hash frame. Protocol 6 removes that
overload entirely: ROM/core/state identity has its own message, while frame 0 is
only a state agreement check:

```
state at frame 0: ours XXXXXXXX, host YYYYYYYY - handshake equalised both sides
```

That single line answers "did the handshake work" without inference.

## statecheck

`shim/test/statecheck.c` loads a core headless — no frontend, no video, no audio,
neutral input — so it can be driven over ssh. Cross-build it with the platform
toolchain and copy it to the device.

```
statecheck <core.so> <rom> [frames] [--save f] [--load f]
                          [--preframes N] [--amplify N] [--serprobe N]
```

| flag | question it answers |
|---|---|
| *(default)* | is a round trip a fixpoint at this point in the run? |
| `--save` / `--load` | do two devices compute identically from one shared state? |
| `--preframes N` | is `unserialize` a pure function of its bytes? |
| `--amplify N` | does a round-trip difference stay put, or grow into divergence? |
| `--serprobe N` | is `retro_serialize` side-effect free? |

Cross-device determinism, the check worth running when adding a core or a
platform — run on both devices with the same state file and compare:

```
A30    after 600 : 8922cd1e
Brick  after 600 : 8922cd1e
```

Note the ROM must be a plain file; picodrive sets `need_fullpath` and does not
read zips. minarch leaves an extracted copy in `/tmp/nextarch/<TAG>/`.

## statecheck is not determinism.c

Both live in `shim/test/`, they answer different questions, and neither subsumes
the other.

`determinism.c` runs frames with inputs derived from the frame number and hashes
via `serialize` every `CHECKPOINT` frames. It answers **do these two builds
emulate identically**. It never calls `unserialize`, so it cannot see any of the
three problems above - which is precisely how picodrive passed it (Gunstar
matching through 2000 frames) while Streets of Rage 2 desynced at every
checkpoint.

Two things to keep in mind when reading its output:

- Because `serialize` mutates state, its own checkpointing alters the trajectory
  it measures. Cross-device comparison stays valid, since both sides are
  perturbed identically - but only while the cadence matches.
- `CHECKPOINT` there is 500; netplay's `HASH_INTERVAL` is 300. The two exercise
  different trajectories, so passing one does not imply passing the other.

Use `determinism.c` for "do these builds agree", `statecheck` for "can state be
moved between them".

## The pathologies are build-independent

Worth knowing before anyone tries to fix these with compiler flags. Every
picodrive we produced - stock armv7, stock aarch64, FAME/CZ80, and all three
`NO_ARM_ASM` variants at `-O0`, strict `-O2` and `-Ofast -ffast-math -flto` -
reports the same behaviour:

```
roundtrip: DIFFERS 1/678519 bytes, first at 140429
serprobe : DIFFERS 7-8/678519 bytes, first at 66013
```

Identical offsets across every build and both architectures. These are
properties of picodrive's state format, not of any build configuration. No
compiler setting avoids them; the shim has to accommodate them.

## Transport: why it is TCP, and when UDP would be worth it

Recorded because "wouldn't UDP be faster" is a reasonable question with a
non-obvious answer.

**Broadcast is a trap and must not be used for per-frame data.** On WiFi,
broadcast and multicast frames are sent at the lowest basic rate (1-6 Mbps,
against the 39-54 Mbit/s these links negotiate for unicast), are never
acknowledged so there is no retransmit, and are buffered by the AP until the
DTIM beacon - typically 100-300 ms. That is larger than the entire input-delay
budget. Broadcast is correct for discovery, which is where the pak already uses
it (UDP on 55438, every 500 ms); it is wrong for input.

**UDP would not lower per-packet latency.** `TCP_NODELAY` is already set, so
nothing is held for coalescing. An input packet is 13 bytes (5-byte header +
8-byte payload); TCP's extra 12 bytes of header at 60 Hz is ~700 B/s. Measured
ICMP round-trip - connectionless, a fair proxy for UDP - is 1.7 / 4.3 / 21.5 ms,
and TCP with NODELAY on an unloaded link is not meaningfully worse.

**What UDP would actually buy is a lower `input_delay`.** TCP's redundancy is
retransmission: detect loss, ask again, deliver late but intact. For a 60 Hz
input stream that is the wrong shape - it spends latency guaranteeing delivery of
data whose value has already expired, and a retransmit costs at least one RTT
(often a 200 ms RTO floor). UDP allows *preemptive* redundancy instead: put
frames N, N-1, N-2 in every datagram, so a single loss is covered by the packet
arriving ~17 ms later with no detection step. The cost is bandwidth, which at
8-byte payloads is free (~1.4 KB/s at 3x instead of 0.5 KB/s).

So the trade is not speed against redundancy. It is *which kind* of redundancy,
and for input the preemptive kind is strictly better shaped.

### The protocol is not uniform

Different messages want different things, which is why "switch to UDP" is the
wrong unit of change:

| message | requirement | on UDP |
|---|---|---|
| `CMD_INPUT` | frame N's buttons before frame N runs | **better** - redundancy beats retransmit |
| `CMD_HASH` | diagnostic only | fine - already matched by frame |
| `CMD_PING` | liveness | fine - loss is itself the signal |
| `CMD_PAUSE` / `CMD_RESUME` | edge-triggered, must arrive | **worse** - a lost RESUME strands the peer indefinitely; the old 30 s forced resume was removed because it broke two-menu resume |
| `CMD_DATA` | link-cable core traffic, ordered, every byte | **much worse** |
| `CMD_STATE` | 678 KB, complete | **much worse** |
| `CMD_HELLO` | handshake | worse |

The receive path is already UDP-shaped for the messages that would move:
`nl.inputs[frame % INPUT_RING]` is frame-indexed and validates the frame number,
so reordering and duplication are already tolerated.

**There is also a non-reliability dependency on TCP.** `netlink.c` deliberately
uses TCP flow control as backpressure: when the frontend stalls, the worker stops
reading and lets TCP throttle the peer, so the session resumes intact rather than
with a hole in it. UDP has no equivalent. For `CMD_INPUT` a hole is harmless
(frame-indexed, redundant); for `CMD_DATA` it is a corrupted serial stream that
gambatte and gpSP cannot recover from.

### If it is ever done

Not "UDP instead of TCP" but **UDP for messages whose value expires, TCP for
messages that must arrive**: `CMD_INPUT` and `CMD_HASH` on a second socket,
everything else unchanged. That splits along the mode boundary too - link-cable
play would keep the current transport untouched, which is worth something given
it has been the trouble-free half throughout.

### Why not yet

Measured loss is **0%** and stalled frames are **0%** at `input_delay=3`. There
is nothing for UDP to route around. Transport RTT is 4.3 ms of a 49.9 ms budget -
about 9% - so eliminating it entirely would save ~4 ms of 50. The stalls that do
appear are peer-slowness, not loss: a 22-frame stall cannot come from a 1.3-frame
RTT, and UDP does nothing for it.

The trigger to revisit: set `input_delay=2` and look at the *shape* of any new
stalls. Loss-shaped (short, correlated with missed hashes) argues for UDP;
peer-slowness-shaped does not. Rollback would also change the calculus, since
there late packets are useful rather than merely late.

## Diagnosing a desync

In order, because each step is invalidated by the one before it:

1. **Does it desync from the very first checkpoint, or later?** From the first
   means the handshake did not equalise the two sides. Later means something
   during play. Look at `state at frame 0`.
2. **Does a `missed sync check` immediately precede the first DESYNC?** That is
   the asymmetric-serialize signature, even if the cause is a new one.
3. **Are the two cores actually the same?** Compare the `core identity:` lines,
   which print `library_name`, `library_version` and `serialize_size` — not just
   the hash. Different `serialize_size` means the builds cannot exchange state at
   all, and the shim warns about that explicitly.
4. **Run `statecheck --serprobe` and `--preframes`** on the core before assuming
   the bug is in the shim. Two of the three causes here were core behaviour the
   shim simply has to accommodate.

## What was actually wrong with picodrive, and what was not

Worth separating, because the record is easy to misread:

- The two platforms **did** build genuinely different emulators — ARM assembly
  throughout on armv7 versus the portable C path on aarch64. Matching them was
  necessary, and `NO_ARM_ASM=1` on the my282 build is the whole fix. See the
  Determinism section of `implementation.md` for the measured matrix.
- Matching them was **not sufficient**, and was not what fixed Streets of Rage 2.
  With identical builds on both devices, the session still desynced at every
  checkpoint until the three problems above were fixed.
- **The tg5040 side never needed rebuilding.** aarch64 cannot use Cyclone or
  DrZ80 — they are 32-bit ARM assembly — so the stock Trimui core was already on
  the portable path and already bit-compatible with the A30's portable builds.
  That rebuild was wasted effort, undertaken because the identity-hash collision
  above made the two cores look mismatched when they were not.

## Result

Streets of Rage 2, ad hoc, `input_delay=3`, after all three fixes:

```
DESYNC=0   in-sync=9   missed=0
in sync at frame 300 ... in sync at frame 3600
pacing: 600 frames in 10000ms (60.0 fps), stalled 0 frames (0%), delay 3
```

Nine consecutive checkpoints over a minute of play, zero divergence — where every
previous session desynced, most at the first checkpoint.

The state still fails to round-trip (`DIFFERS 649/678519`) and `serialize` still
mutates. Neither matters now, because both sides do the same things in the same
order. That is the point: the fix is not making the core well behaved, it is
making the two devices treat a badly behaved core identically.

### Still open

- The client now queues eight incoming hash checkpoints, so a late arrival no
  longer overwrites an older one before the emulator thread can compare it.
- Cross-device determinism is confirmed at 600 frames. The 1800-frame comparison
  was started but never finished; the Brick produced `ae8a2b95` and the A30 leg
  did not complete.
