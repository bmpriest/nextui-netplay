# mGBA as a second paired core — groundwork

Gambatte gave us instanced link play for Game Boy. mGBA is the obvious next
target: it covers GBA as well as GB/GBC, and it is the emulator whose
multiplayer support actually works in the wild.

The received wisdom is that mGBA's linking lives in its Qt frontend and is
therefore unavailable to a libretro build. That is half right, and the half that
is wrong is the important half. What follows is verified against the checkout at
`cores/mgba-libretro` (`e31759b24`), not from memory.

## What is actually where

**The lockstep coordinator is core code, not Qt.**

```
src/core/lockstep.c                 generic state machine + threaded user
src/gb/sio/lockstep.c               Game Boy cable, 2 nodes
src/gba/sio/lockstep.c              GBA cable, up to 4 nodes
include/mgba/internal/gb/sio/lockstep.h
include/mgba/internal/gba/sio/lockstep.h
```

`MAX_GBS` is 2 and `MAX_GBAS` is 4. `GBSIOLockstep` holds the players and the
pending transfer bytes; `GBSIOLockstepNode` is a `GBSIODriver` that attaches to
one console. The API is `GBSIOLockstepInit`, `GBSIOLockstepNodeCreate`,
`GBSIOLockstepAttachNode`, `GBSIOLockstepDetachNode`. None of it includes
anything from Qt.

**The libretro build does not compile the drivers.**

`libretro-build/Makefile.common` includes `src/core/lockstep.c`, `src/gb/sio.c`,
`src/gba/sio.c` and `src/gba/sio/gbp.c` — but neither `src/gb/sio/lockstep.c`
nor `src/gba/sio/lockstep.c`. The generic state machine is compiled; the two
drivers that would use it are not in the build at all.

**The libretro frontend has no concept of a second console.**

`src/platform/libretro/libretro.c` owns exactly one core:

```c
static struct mCore* core;                       /* line 86 */
```

`retro_load_game_special` is a stub:

```c
bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info) {
	UNUSED(game_type); UNUSED(info); UNUSED(num_info);
	return false;                                 /* line 2250 */
}
```

and the string `lockstep` appears zero times in the file.

So the position is: the cable is written, it is good, and nothing in the
libretro layer reaches it.

## Why this is a better starting point than gambatte was

This is the part worth dwelling on, because it decides how much of the work we
did for Gambatte has to be repeated.

Gambatte's `SerialIO::send()` is **synchronous**. The clock-owning console
blocks inside `runFor()` until its peer answers. That single fact forced
everything else: two threads, a condition-variable coordinator, a frame barrier
so neither console could leave a frame mid-transfer, cycle-stamped requests so
two devices resolved the same transfer identically, and a long tail of deadlocks
and determinism bugs to go with it.

mGBA does not work that way. `GBSIOLockstepNode` schedules an `mTimingEvent` on
its own console's timing and its handler returns **how many cycles until it next
needs attention**:

```c
static void _GBSIOLockstepNodeProcessEvents(struct mTiming*, void* user, uint32_t cyclesLate);
node->nextEvent += LOCKSTEP_INCREMENT;
```

There is no blocking call in the driver. Coordination is expressed as cycle
deltas, which is exactly the shape a cooperative scheduler wants: advance
whichever console owes cycles, then the other.

And the blocking policy is not baked in. `struct mLockstep` is an interface:

```c
void (*lock)(struct mLockstep*);
void (*unlock)(struct mLockstep*);
bool (*signal)(struct mLockstep*, unsigned mask);
bool (*wait)(struct mLockstep*, unsigned mask);
void (*addCycles)(struct mLockstep*, int id, int32_t cycles);
int32_t (*useCycles)(struct mLockstep*, int id, int32_t cycles);
int32_t (*unusedCycles)(struct mLockstep*, int id);
```

`mLockstepThreadUser` — the thread-sleeping implementation, guarded by
`#ifndef DISABLE_THREADING` — is *one* implementation of that policy, the one Qt
uses. A single-threaded frontend can supply its own `wait`/`signal` that never
sleep a thread and instead hand control back to a scheduler.

If that holds, the paired mGBA core needs **no threads, no frame barrier and no
in-process cable of our own**. The three hardest problems from the Gambatte work
are already solved upstream, by the person who wrote the emulator.

## Verified since: the revision, and the determinism question

**The revision this was written against is not the one we ship, and it does not
matter.** The notes above were taken from `cores/mgba-libretro` at `e31759b24`.
NextUI pins mGBA at `925f0f0b` — deliberately, because commit `89404771`
(2026-08-04) moved the libretro build out of the repo root. We build the pinned
revision, and `src/core/lockstep.c`, `src/gb/sio/lockstep.c`,
`src/gba/sio/lockstep.c` and `include/mgba/internal/gba/sio/lockstep.h` are
byte-identical between the two, so every reading below transfers. `925f0f0b`
also carries the same `-DDISABLE_THREADING -DMINIMAL_CORE=2`.

**Question 1 above — is lockstep resolution a pure function of emulated state? —
now looks answered yes, for the GBA driver.** Three findings, all in
`src/gba/sio/lockstep.c`:

- *No wall clock anywhere.* Every decision runs off `mTimingCurrentTime` and
  `player->cycleOffset`. No timeouts, no `TryLock`, no timed waits. The Gambatte
  trap is simply not present.
- *Pacing is cycle-bounded and asymmetric.* `_untilNextSync` grants headroom only
  to player 0, and only `LOCKSTEP_INTERVAL` (4096) cycles of it; every other
  player is hard-capped at `coordinator->cycle`. A secondary console cannot
  outrun the coordinator no matter how it is scheduled.
- *The barrier is total.* `GBASIOLockstepCoordinatorWaitOnPlayers` sets `waiting`
  to every other attached player before sleeping, so arrival order cannot change
  what gets resolved.

Together these mean the schedule is fixed by cycle bookkeeping rather than by
thread arrival — which is what a mirrored replica needs, and also what makes a
serial and a threaded implementation interchangeable (see below).

**Question 2 — does the savestate cover lockstep and SIO state? — yes.**
`GBASIOLockstepDriverSaveState` serialises player `cycleOffset`, `asleep`,
`dataReceived`, all four `otherModes`, and the event queue with `timestamp` /
`finishCycle`; from player 0 it also stores the coordinator's `cycle`, `waiting`,
`nextHardSync`, `multiData`, `normalData`, `transferMode` and `transferActive`.
All through explicit `STORE_32LE` / `STORE_16LE`. Note that `asleep` and
`waiting` are included, so a checkpoint taken mid-barrier is meaningful.

Question 3 is now handled by the paired ABI: both devices exchange cartridge
epochs and the wrapper installs `RTC_FAKE_EPOCH` independently on consoles A and
B before execution. The broader `MINIMAL_CORE=2` audit remains open.

### Serial now, parallel later, and the one rule that keeps them compatible

Early per-thread measurements predicted at most 12.66 ms for a serially paired
GBA on Brick and at least 23.42 ms on A30. The first live Brick race corrected
the useful number: the shim surrounds the complete paired libretro call,
including the frontend's blocking audio callback, and measured 16.25-16.29 ms.
Direct phase profiling later showed that the enclosing paired call includes the
frontend's presentation wait: emulation plus cable averaged roughly 5.3-9.8ms
and left about 5ms of real headroom on Brick. The old conclusion that serial GBA
was intrinsically out of reach on A30 was therefore based on the wrong boundary;
the my282 build needs device measurement rather than rejection from an estimate.
See `testing/MGBA.pak/README.txt` for the component measurements.

So the A30 would eventually want a *threaded* paired core while the Brick runs a
*serial* one. Those two can interoperate, because the only policy surface is
`mLockstepUser` — four callbacks, `sleep` / `wake` / `requestedId` /
`playerIdChanged` — and everything that decides emulated outcomes sits beneath
it in shared code.

**The rule: both schedulers must take their run quantum from `_untilNextSync`,
never decide it themselves.** A serial cooperative scheduler is tempting to write
as "advance console A one frame, then console B one frame". That is
deterministic, and it is deterministically *different* from a scheduler honouring
4096-cycle windows. Two replicas built that way would diverge while each looked
perfectly stable in isolation. The harness in step 2 should run both schedulers
over one input script and hash both consoles, which catches exactly this.

## What still has to be true

Determinism is the requirement that killed us repeatedly with Gambatte, and
none of the above establishes it. Before writing a line of wrapper:

1. **Is the lockstep resolution a pure function of emulated state?** Gambatte's
   coordinator originally decided transfers by wall clock and thread arrival;
   ours had to be rewritten to decide by emulated cycles. mGBA's uses
   `mTimingEvent` and cycle counts, which is promising, but `signal`/`wait` and
   the `TRANSFER_*` phase machine need reading with the mirrored-replica question
   in mind: *given identical initial state and identical inputs, do two devices
   compute identical results?*
2. **Does mGBA's savestate cover the lockstep and SIO state?** Our paired
   checkpoint has to serialize both consoles *and* the cable. Gambatte needed
   fixes here (uninitialised `SaveState` PODs) before two devices could agree.
3. **Does the RTC read the host clock?** GBA cartridges have RTCs too, and we
   have just been through what that costs. Expect the same `TimeSource` shape.
4. ~~What does `DISABLE_THREADING` exclude?~~ **Answered.** The libretro build
   defines `-DDISABLE_THREADING -DMINIMAL_CORE=2`, so `mLockstepThreadUser` is
   compiled out entirely. There is no threaded fallback to retreat to: a paired
   libretro core *must* supply its own non-blocking `mLockstep` policy. That is
   the design we wanted anyway, and it is now forced rather than chosen. Worth
   checking what else `MINIMAL_CORE=2` removes.

## Shape of the work

Roughly in order, each step verifiable before the next:

1. ~~**Build the drivers.**~~ **Done.** `cores/patches/mgba-001-build-lockstep-drivers.patch`
   adds `src/gb/sio/lockstep.c` and `src/gba/sio/lockstep.c` to
   `libretro-build/Makefile.common`; `make cores-mgba` builds it. Both objects
   land in the link line and the tg5040 core carries 9 `GBSIOLockstep*` and 24
   `GBASIOLockstep*` symbols. Single-player behaviour is unchanged by
   construction: `lockstep` still appears zero times in
   `src/platform/libretro/libretro.c`, so the drivers are linked but
   unreachable. (`nm -D` will not show them — `link.T` localises everything that
   is not `retro_*`; check the static symtab.)
2. **A host harness, before any pak integration.** **Built** —
   `cores/tests/mgbalockstep.c`, run by `cores/tests/mgbalockstep.sh`, wired into
   `make test`. Two `mCore`s, one `GBASIOLockstepCoordinator`, two drivers, a
   cooperative scheduler that owns no quantum of its own, a hand-encoded GBA test
   ROM that programs SIOCNT for multiplayer, and an FNV-1a hash of each console's
   `saveState`. No device, no frontend, no commercial ROM.

   It reproduces: 4 runs x 120 visible frames hash identically on both consoles,
   with an identical slice count, so the schedule itself is reproducible and not
   merely the end state. The normal run completes 117 multiplayer transfers and
   a 600-frame stress run completes 597. Every mGBA FATAL assertion now fails the
   test; both runs complete with zero.

   **The scheduler quantum was the key finding.** The first implementation used
   `mCore::runFrame`. That looked reasonable because
   `GBASIOLockstepPlayerSleep` sets `cpu->nextEvent = 0` and interrupts the CPU,
   but `_GBACoreRunFrame` is itself a loop: after `ARMRunLoop` returns at that
   event boundary, it calls it again until video advances. A cooperative sleep
   therefore returned to code that deliberately kept executing the sleeping
   console. The initial harness logged 8 reproducible
   `Multiplayer desynchronized` FATALs and only completed 4 transfers; the bad
   schedule was deterministic enough to hide behind matching hashes.

   The cooperative scheduler must call `mCore::runLoop`, re-check the user's
   asleep flag after every slice, and switch consoles there. With that change,
   attach and repeated transfers need no special-case register-write queue and
   produce no desync assertions. `runLoop` is the upstream-defined event quantum
   we wanted; `runFrame` is a frontend presentation operation and must be built
   *around* the paired scheduler instead of used inside it.

   The test ROM now only selects multiplayer mode and idles. After the two-frame
   attach period the harness injects one primary SIOCNT start write per visible
   frame through `GBASIOWriteSIOCNT`, the same public path used by emulated MMIO.
   This makes transfer coverage explicit and asserts at least 117 completed
   transfers in the standard 120-frame run. Completion is defined by player 0's
   visible frame with an idle coordinator; player 1 may correctly be asleep one
   frame behind at that boundary.

   Two build notes that cost time and will again:
   - The harness must be compiled with the library's *exact* defines. `struct
     mCore` has members behind `ENABLE_VFS`, `ENABLE_DIRECTORIES` and
     `MINIMAL_CORE`; a mismatch shifts field offsets and the first call through a
     function pointer segfaults. `mgbalockstep.sh` scrapes the flags out of the
     build log rather than restating them.
   - `mCoreInitConfig(core, NULL)` must precede `core->init`. `_GBACoreReset`
     reads config, and an uninitialised config hash table faults rather than
     returning a default.
3. ~~**Implement the cooperative paired wrapper.**~~ **Done.**
   `cores/mgba/libretro_dual.c` owns two GBA `mCore`s, two lockstep drivers and
   one coordinator. `mLockstepUser::sleep` / `wake` mark runnable consoles and
   the scheduler alternates `mCore::runLoop` slices until both consoles produce
   their next frame and the coordinator is quiescent. The stock frontend remains
   the ordinary single-player core.
4. ~~**Mirror the `retro_dual_*` ABI.**~~ **Done.** ABI v2 exposes two-content
   loading, visible-console selection, per-console memory, exact paired
   checkpoint round-trips, targeted reset and per-console RTC epochs.
   `cores/tests/mgbadual.sh` drives 120 frames of real multiplayer transfers,
   verifies independent-process state hashes, and proves that resets of A, B and
   both return to checkpoint-safe frames.
5. ~~**Then the pak.**~~ **Done for tg5040, my282 and h700 builds.** Manifest word five is
   now a capability mask (bit 0 Gambatte, bit 1 mGBA), so both devices must have
   enabled settings and local artifacts before `instanced_mgba=1` is written.
   `testing/MGBA.pak/launch.sh` selects the paired artifact only for that agreed
   session and honors the shim's demotion marker.

## First two-Brick field result

Mario Kart: Super Circuit now reaches its multiplayer menu, connects both
consoles, and completes a race through the paired mGBA core. The session is
playable and stays synchronized; this is no longer only a synthetic-ROM result.

The first device build produced no audible output because the paired wrapper
always ran mGBA's resampler, even when the emulated source and requested output
rates were both 65,536 Hz. The wrapper now bypasses that same-rate conversion
and logs the active path once:

```
mGBA Dual audio active: output=65536 source=65536
```

Audio was present on both Bricks after that change. The host averaged about
16.29 ms per enclosing paired call and the client about 16.25 ms. Later phase
profiling established that this includes presentation pacing: emulation plus
cable varied roughly 5.3-9.8 ms, audio stayed near 0.9 ms and input below 0.1 ms,
leaving about 5 ms of actual headroom.

The remaining visible chop correlated with synchronized missing-input stalls,
not a spike in emulator work. Both peers showed the same bad ten-second windows,
sometimes near 49.5 FPS with stall shares up to 17%. At the time, automatic RTT
negotiation had reduced the session to a three-frame delay. Subsequent Netplay
work treats the transport-specific value as a floor: ordinary Wi-Fi retains ten
frames, ad hoc retains three, and measured RTT may only raise it. A subsequent
ordinary-Wi-Fi race used that ten-frame floor and held essentially 60 FPS with
near-zero input stalls. More games and longer sessions still need beta coverage.

One diagnostics question remains for beta logs: the shim schedules paired-state
comparisons every 300 frames, but the latest retained device logs visibly showed
only the frame-zero agreement. Preserve both MGBA.txt files so the periodic
exchange can be confirmed across more sessions; a mismatch already stops play
with an explicit paired-desync error.

## What not to repeat

- Do not start with the device. Every Gambatte determinism bug was diagnosed on
  the host once a harness existed, and the ones we chased on hardware first cost
  hours each.
- Do not assume the frontend's savestate is the paired checkpoint. It is not;
  it deliberately excludes presentation and diagnostics so two devices can hash
  it.
- Do not let a search, scan or wait run unbounded inside a step the peer is
  blocked on. That produced a freeze indistinguishable from a crash.
- Write the test that reproduces the failure *before* the fix, and check that it
  fails — twice now a test that "passed" was only passing because it could not
  reach the bug.
