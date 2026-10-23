# Implementation review

Status of the shim-based rewrite: what is proven, what is assumed, and what is
left. For how the shim works see [shim-architecture.md](shim-architecture.md).

Testing throughout is a TrimUI Brick (`tg5040`, aarch64) and a Miyoo A30
(`my282`, armv7). Both NextUI builds are ours, which matters more than it
sounds - see [Determinism](#determinism).

## Where the original pak stood

`Netplay.pak` shipped patched `minarch.elf`, `gambatte` and `gpsp` built against
one exact NextUI build. Every NextUI release meant six rebuilt binaries and a
merge against a moving `minarch`. Two features:

| Feature | Screens | Cores |
|---|---|---|
| **Netplay (LAN)** | shared, inputs synced | any |
| **Link** | independent | GB/GBC (gambatte), GBA (gpsp) |

## Where we are

**`minarch.elf` is no longer patched.** A shim between minarch and the core
forwards all 25 libretro entry points and intercepts only what multiplayer
needs. With no session armed it is a pure passthrough.

**The pak ships two link cores**, for reasons that are build-flag deep rather
than source deep: NextUI compiles gambatte without `HAVE_NETWORK` (Game Link
compiled out entirely), and its gpsp lacks the original pak's two RFU fixes.
It also ships the paired cores for instanced link play
(`gambatte_dual_libretro.so`, `mgba_dual_libretro.so`), an optional `MGBA.pak`,
and, in `Netplay-Full.pak.zip`, the shared-screen compatibility cores.

### Working on hardware

- Passthrough on stock NextUI, both architectures, save/config paths unchanged.
- **GBA link** (gpSP, Advance Wars 2, `mul_aw2`): 213s unbroken, ~20k packets
  each way. Also Pokémon.
- **GB link** (gambatte, Tetris): connects and plays, latency-bound.
- **Shared-screen netplay**: Streets of Rage, Streets of Rage 2 and Gunstar
  Heroes all ran with synced inputs. Periodic disagreement now triggers a
  host-authoritative state barrier instead of leaving the screens diverged.
- **Session app**: host/join, UDP discovery, arming, on both devices.
- Two independent emulator consoles per process - see [Dual instance](#dual-instance).

### Verified host-side only

Three suites (`shim/test/run.sh`, `shim/test/link.sh`, `launcher/test.sh`), each
regression confirmed to fail against the bug it covers. The loopback suite also
forces a desync, restarts each role independently, verifies host checkpoint
restore, and rejects different ROM content before state loading.

### Diagnostic game logs

`Verbose debugging logs` is enabled by default. While a session is armed, each
emulator process mirrors its complete stdout/stderr to:

```
$USERDATA_PATH/logs/netplay-games/<game>/<session-id>/<timestamp>-<role>-<pid>.log
```

The usual NextUI per-system log is still written normally. The archive is
separate because NextUI truncates that log on the next launch, which otherwise
erases a crash when the player attempts to rejoin. Rejoins in the same session
share the game/session directory but receive a new file; output from different
processes is never appended into one ambiguous stream. Each file records the
ROM, requested and selected core paths, role, process id, and normal exit
status. A missing exit trailer indicates that the process or its wrapper was
terminated without completing its normal shutdown path.

### Durable state

All mutable Netplay state lives outside the replaceable pak at:

```
/mnt/SDCARD/.userdata/shared/Netplay/
```

This includes settings, the session template and active session, broker status
and logs, Wi-Fi recovery records, mount/stub ownership, Game Switcher backups,
and the ROM scan cache. The test harness owns the nested `netplay-harness/`
directory. On first use, the app and launcher migrate files from the historical
`Netplay.pak/state/` location without replacing newer shared files; harness
artifact backups likewise move from the old platform-specific userdata path.

### Also working on hardware

- **Ad hoc networking**, end to end and in play. The Brick hosts on `wlan1`
  while staying on the house network; the A30 joins; the launch stub re-joins
  per game. Measured 60.0 fps and 0% stalled frames, against 18 fps and 79%
  through an access point. See [adhoc.md](adhoc.md).
- **Host discovery without a shared network.** Announcements over UDP broadcast
  and an SSID scan for the `nextui-` prefix, shown as one list. The second finds
  a host that has already moved to its own network and can be found no other
  way.
- **WiFi recovery.** Proven by artificial network drops: the watchdog arms on
  join, detects the loss, counts three failures over ~60s, restores the client
  stack and clears the dead session pointer.
- **Wi-Fi power save**, when the opt-in *Disable Wi-Fi Power Saving* setting is
  on, is disabled while a session is armed and restored on disarm. On H700 this
  takes a driver reload and lasts until reboot.
- **H700 on hardware.** Shared-screen (Streets of Rage 2), instanced GB
  (Tetris) and instanced GBA (Mario Kart: Super Circuit) sessions against a
  Brick, in the scripted runs under `testing/devbridge/`.
- **SFC across all five platforms.** Compatibility fallback now loads pinned
  Supafaust on both architecture families. Its encoded state, round trip, and
  frame hashes matched between A30 and Brick in a Kirby Super Star probe.
- **Bind mounts instead of writing into `Emus/`.** Covered systems are wrapped
  by mounting a staged copy over the pak in place, so the user's `Emus` tree
  gains nothing. Verified on both devices: 7 paks mounted, the tree left empty,
  `down` restoring the original bytes by checksum, and the boot hook remounting
  after a simulated boot.

### Untested

- `tg5050` - builds, no hardware.
- `my355` - **builds** (app, shim, and the gambatte/gpsp overrides), never run
  on hardware. It and `h700` are separate NextUI forks with their own pinned
  source trees, so they are built through `build-platforms`, which owns the
  image-digest-to-source-commit pairing.
- Hosting from a device with no AP-capable interface. The UI omits the option
  rather than offering something that must fail.
- FBN, SUPA, 32X, SEGACD, GG, SMS, SG1000, FDS.

## Where the mounts go

`bind-mount.sh` covers the built-in emulator paks; `wrap-pak.sh` covers EXTRAS
paks that own their SD path. Both stage a copy and mount it over the original,
so nothing on disk is modified and a reboot reverts everything.

The mount target is the pak's **own directory**, not the SD path and not the
`launch.sh` file:

- The SD path does not exist for a system pak - creating it is exactly what this
  replaced.
- File-level bind mounts do work on these kernels, but shadowing `launch.sh`
  hides the original at its own path, leaving our replacement no way to reach
  it. Reaching a stashed copy instead breaks `$0`, and `EMU_TAG` is
  `basename $(dirname "$0") .pak`. Mounting the directory keeps `$0` at the
  pak's real path, so everything derived from it stays correct.

Because mounts do not survive a reboot, this registers a hook in
`$USERDATA_PATH/auto.sh` - present on both measured devices and already the
convention other paks use. The `boot.d` directory that `run_hooks.sh` reads
would be tidier, but it exists on tg5040 and not on my282.

Mount ownership is resolved from `/proc/self/mountinfo`, not `/proc/mounts`.
On the Brick's exFAT SD card, `/proc/mounts` reports every bind source as the
backing block device (`/dev/mmcblk1p1`) and discards the staged source path.
`mountinfo` retains that path as the mount root, allowing `down` to unmount
only an exact Netplay stage/target pairing. Preflight likewise detects a
mounted Netplay wrapper and validates its preserved `launch.sh.old`; otherwise
an already-active wrapper would make the app report that no emulator paks exist.

`install-stubs.sh` remains for devices where mounting is unavailable, and so an
install predating the mounts can still be uninstalled. `bind-mount.sh up`
removes any stubs it finds first: `getEmuPath` checks the SD card before the
system paks, so a leftover stub would shadow the mount and wrap twice.

## Determinism

The dominant finding of this phase. Shared-screen netplay and dual-instance both
require every device to compute **bit-identical** results from identical inputs.
Three independent obstacles sit in the way. The first two are about the cores;
the third is about how state is moved between them and is documented separately
in [state-sync.md](state-sync.md).

### 1. Architecture-specific emulation

picodrive builds ARM assembly into far more than the 68000 core. On armv7 with
`ARCH=arm` the Makefile enables Cyclone (68k), DrZ80, and hand-written assembly
for memory, render, YM2612 and misc. On aarch64 none of that applies - it is the
portable C path by construction, because that assembly is 32-bit ARM only.

Same ROM, same inputs, different simulation. That is what desynced Streets of
Rage: characters interacted (inputs shared) while enemies differed (state
diverged).

**The fix is `NO_ARM_ASM=1` on the my282 build, and nothing else.** Measured
across every picodrive we produced, Streets of Rage 2, 3000 frames from cold:

| build | arch | configuration | hash @3000 |
|---|---|---|---|
| stock tg5040 | aarch64 | `-Ofast -ffast-math -flto` | **`3006365b`** |
| tg5040 strict-portable | aarch64 | `NO_ARM_ASM DETERMINISTIC` | **`3006365b`** |
| my282 portable | armv7 | `NO_ARM_ASM`, `-Ofast -ffast-math -flto` | **`3006365b`** |
| my282 portable strict | armv7 | `NO_ARM_ASM`, `-O2` strict FP, no LTO | **`3006365b`** |
| my282 portable unoptimized | armv7 | `NO_ARM_ASM`, `-O0` | **`3006365b`** |
| my282 stock | armv7 | Cyclone + DrZ80 + ARM asm | `507cab39` |
| my282 fame_cz80 | armv7 | FAME/C + CZ80, **ARM asm kept** | `c3102fdb` |

Three conclusions, two of which correct earlier assumptions recorded here:

- **Optimisation level and fast-math are irrelevant.** `-O0`, strict `-O2` and
  `-Ofast -ffast-math -flto` all produce identical state, on both architectures.
  The `DETERMINISTIC=1` profile was built on a theory that `-ffast-math` was
  corrupting YM2612 state. That theory was wrong and the profile is unnecessary;
  it survives in the tree only because reproducibility is worth having.
- **Swapping only the CPU cores is not enough.** `fame_cz80` uses FAME/C and CZ80
  yet still diverges, differently from stock. The remaining ARM assembly is doing
  it, and only `NO_ARM_ASM=1` disables all of it.
- **The tg5040 side never needed rebuilding.** The stock Trimui core was already
  bit-compatible with the A30's portable builds. Rebuilding it was wasted effort.

Gunstar Heroes, the game that failed, matches through 2000 frames:

```
Brick 2.01-b0be121          A30 2.01-strict-portable
frame  500  82f91c3f        frame  500  82f91c3f
frame 1000  b23479ea        frame 1000  b23479ea
frame 1500  684cd29b        frame 1500  684cd29b
frame 2000  948ff734        frame 2000  948ff734
```

**Matched cores are necessary but not sufficient** - see obstacle 3. With
identical builds on both devices, Streets of Rage 2 still desynced at every
checkpoint until the state-transfer bugs in [state-sync.md](state-sync.md) were
fixed. Matching cores makes the two emulators agree; it does not make the
handshake correct.

### 2. Core revision drift

Most cores were **unpinned**, so each platform took whatever `HEAD` was on its
clone date. Nothing to do with architecture; the two trees were simply cloned at
different times. Now pinned in `my282/cores/makefile` to the revisions tg5040
builds:

| Core | Was | Now | Brick |
|---|---|---|---|
| fceumm | `b5e3566` | `afe65ef` | `afe65ef` |
| gpsp | `5b6e751` | `69e86eb` | `69e86eb` |
| pcsx_rearmed | `94f15b3` | `050981b` | `050981b` |

Pinning invalidated `patches/fceumm.patch`, written against the old `HEAD`;
regenerated, original kept as `.bak`. **This recurs on every pin bump** - the
patches are context-sensitive.

### Per-core outlook

| Core | Assessment |
|---|---|
| **fceumm** | Zero arch-specific files, zero inline asm. Pure C. **Cross-architecture identical**: armhf and arm64 builds of `afe65ef` produce the same state hash (`0733dee4`) and the same state size. Revision drift does change behaviour, but it also changes the state size, so a mismatched pair is refused rather than desyncing. |
| **picodrive** | Solved, and now confirmed across architectures: `2.01-strict-portable` (armhf) and `2.01-b0be121` (arm64) both produce `59f99520`. `NO_ARM_ASM=1` alone is necessary and sufficient. The only core pinned identically on all five platforms. |
| **gambatte** | Same commit both sides (`9b3b5e3`, both ours). No asm cores. Untested. |
| **pcsx_rearmed** | Deterministic **only with `pcsx_rearmed_drc_thread=disabled`** - see below; the shim now forces it. Cross-architecture remains untested, and the arch-specific subsystems are still there: `assem_arm.c` vs `assem_arm64.c` (MIPS recompiler) and 33 NEON files in `gpu_neon`. Same-architecture play is plausible; my282-to-arm64 is not established. |
| **SNES** | **Resolved through compatibility fallback.** Full Snes9x 1.63 remained deterministic but only sustained 46–50 fps in live A30/Brick lockstep. Snes9x 2005 was rejected because it serializes raw native structs, making its state architecture-dependent. Pinned Supafaust uses an encoded 277,033-byte state; A30 and Brick produced identical hashes (`f0e38316` after 120 frames and `bf872104` after a further 30-frame serialize probe), exact round trips, and about 81 headless fps on A30 for Kirby Super Star. |

### How many builds are actually in the wild

Cores that float take whatever `HEAD` was on the build date, so the practical
question is not "can two devices differ" but "how often do they, and does it
matter". Surveyed across the 22 NextUI releases from 2026-01-27 to 2026-07-19,
reading CRC32s out of the release zips and then running each distinct binary
through `statecheck`:

| core | shipped binaries | source revisions | **distinct behaviours** |
|---|---|---|---|
| picodrive | 2 | 1 | **1** |
| snes9x | 4 | 3 | **1** |
| fceumm | 6 | 5 | **3** |
| pcsx_rearmed | 22 | 7 | **7** |

Three things follow, and none of them was obvious beforehand.

**A different binary is usually not a different emulator.** snes9x shipped four
binaries in six months and every one produces identical state - three of them
differ by *seven bytes*, the embedded git hash in the version string, and the
fourth differs in 61% of its bytes from a toolchain change and still emulates
identically. pcsx_rearmed changes in every single release only because it embeds
a literal `build time: Jan 27 2026 22:29:37`.

**Behaviour tracks source revision exactly.** Every pcsx_rearmed binary sharing a
revision produced the same hash, across seven revisions and 22 binaries. That is
also the control that says this measurement method works.

**The consequences differ per core.** picodrive and snes9x were immune to six
months of churn. fceumm changed behaviour three times but changes state size with
it (13772 / 13788 / 13804), so the shim's existing size check refuses the pairing
before a game starts. pcsx_rearmed changed behaviour seven times at a constant
state size - the only core where drift desyncs silently.

### pcsx_rearmed is nondeterministic by default

Five runs of one binary, same ROM, same frame count, produced five different
state hashes. Not architecture, not ASLR, not the harness - `picodrive` through
the identical code path was stable 3/3. The cause is one core option:

```
(none, core defaults)                       ed85c6ad c557f389 228d565f  -> varies
pcsx_rearmed_spu_thread=disabled            c1b8b306 0c1bf105 db4fea9d  -> varies
pcsx_rearmed_gpu_thread_rendering=disabled  78b7a251 79adef01 d5791e43  -> varies
pcsx_rearmed_drc_thread=disabled            20c68cf1 20c68cf1 20c68cf1  -> DETERMINISTIC
```

`drc_thread` compiles recompiler blocks on a worker thread; until a block is
ready the emulation thread runs the interpreter, and the two paths disagree on
cycle timing. So the state depends on when the compiler thread happened to
finish. **Two devices running the identical build would desync**, which no
amount of pinning or core-shipping could have fixed. The shim now forces this
option for shared-screen sessions (`REQUIRED_OPTIONS` in `shim.c`).

This was originally measured as "pcsx_rearmed is nondeterministic", which was
wrong: `statecheck` answered `false` to every `GET_VARIABLE`, leaving the core on
compiled-in defaults that no frontend would ever produce. The harness now
answers core options from `$STATECHECK_VARS`. **A harness that does not configure
a core is not testing the core as it is really run.**

### Measuring determinism is itself error-prone

Three separate times a test reported something other than what it appeared to.
Worth internalising before trusting any future result:

1. **Unwritten buffer padding.** Hashing a `malloc`'d state buffer includes bytes
   the core never wrote, so results vary run to run on one device. Zero first.
2. **Host clock in the state.** gambatte writes `std::time(0)` into RTC and HuC3
   base-time at power-on, even for carts without RTC. Two boots seconds apart
   differ by 16 bytes of 26,882, and a whole-state hash amplifies that into
   apparent divergence. This invalidated a cross-device gambatte result.
3. **Tests that cannot fail.** The desync regression passed against the bug it
   was written for, twice, because it induced a *frontend* stall (retro_run not
   called) rather than an *input* stall (retro_run called, returns early).

The shim's `DESYNC` check shares fault 2 and needs a volatile-field exclusion
list before it is trustworthy for RTC carts. MD has no RTC, so its Gunstar
reports were valid.

### 3. Moving state between the two cores

Matched cores compute the same thing. That still leaves the question of whether
the two devices *start* from the same state and stay comparable while running,
and three assumptions there turned out to be false: a save state is not
necessarily a serialize/unserialize fixpoint, the host cannot keep its own state
while sending a copy, and `retro_serialize` is not necessarily side-effect free -
which made the divergence check a cause of divergence.

Full detail, evidence and the `statecheck` tool: **[state-sync.md](state-sync.md)**.

This is the obstacle that actually kept Streets of Rage 2 desyncing after the
cores were matched. With all three cleared it now runs nine consecutive in-sync
checkpoints at 60 fps.

### Consequence

Cross-architecture shared-screen netplay needs matched cores - a port-level fix
(align my282 with tg5040), not a pak-level one - **and** a state transfer that
puts both sides through the same `unserialize` and keeps `serialize` calls
symmetric. Matched cores alone are not enough.

**Link play has no such requirement** - each device runs its own game and the
cores exchange messages, which is why Brick↔A30 GBA link works across two
architectures, and why Tetris over gambatte ran 13½ minutes clean while MD was
still desyncing.

## Sharing cores between devices (frozen historical design)

> **Current behavior:** executable transfer is compiled out. The option remains
> in Settings and defaults off. Devices exchange metadata only: matching
> installed builds remain first choice; otherwise, when enabled on both sides,
> both select their own locally packaged compatibility core. The transfer design
> and measurements below are retained as history, not as active behavior.

If two devices want to play and their builds differ, one can simply **send the
other its core**. This is a better answer to build drift than pinning, because it
needs no coordination with upstream: whoever hosts defines the build for that
session.

It works because of the survey result above - behaviour depends on source
revision, not on which platform's toolchain produced the binary. Verified
directly by loading three foreign cores on a Brick:

```
tg5050: LOADS  [FCEUmm (SVN) afe65ef] hash=17ad8a35
h700:   LOADS  [FCEUmm (SVN) b5e3566] hash=d9239008
my355:  LOADS  [FCEUmm (SVN) 3a84a6f] hash=3d1d9be9
--- native ---
tg5040:        [FCEUmm (SVN) afe65ef] hash=17ad8a35
```

The tg5050 build and the Brick's own build are the same revision and produce the
**identical hash**, despite different CRCs, different sizes, a different
toolchain, and a much higher glibc floor. The other two differ only because they
are different revisions. Toolchain does not perturb emulation; revision does.

The binaries themselves are *not* interchangeable-looking - every core differs
between tg5040 and tg5050, sizes included, and picodrive produces four different
binaries from one pinned revision. That difference is simply not the thing that
matters.

### What can be sent where

| platform | arch | glibc floor | extra libs |
|---|---|---|---|
| tg5040 | AArch64 | GLIBC_2.17 | - |
| h700 | AArch64 | GLIBC_2.17 | - |
| my355 | AArch64 | GLIBC_2.17 | - |
| tg5050 | AArch64 | **GLIBC_2.33** | - |
| my282 | **ARM** | GLIBC_2.15 | - |

**my282 is a hard boundary.** 32-bit ARM cannot load an AArch64 object, and no
copying fixes it. An A30 can only receive a core from another A30.

Among the arm64 four it works, with one asymmetry: cores built for
tg5040/h700/my355 need only GLIBC_2.17 and load essentially anywhere, while a
tg5050-built core wants 2.33 and could be refused by an older device. **Prefer
the donor with the lower floor.**

Per-core runtime demands, which decide how portable each one is:

| core | C++ | needs |
|---|---|---|
| fceumm, picodrive, gpsp | no | `libm`, `libc` |
| gambatte | GLIBCXX_3.4.21 | `libstdc++`, `libgcc_s`, `libm`, `libc` |
| snes9x | GLIBCXX_3.4.21 | `libstdc++`, `libz`, `libgcc_s`, `libm`, `libc` |

GLIBCXX_3.4.21 is GCC 5.1-era; even the A30's libstdc++ 6.0.22 provides 3.4.22,
so the C++ cores are portable in practice. This is only a hazard for locally
built cores: the my282 snes9x build needed `-static-libstdc++` because GCC 13.2
emits a `GLIBCXX_3.4.32` dependency the device cannot satisfy.

**Negotiate by trying, not by predicting.** Check architecture first - that is a
hard abort with a clear message - then have the recipient `dlopen` the donated
core and report the result. Parsing ELF version tables to guess is how the
GLIBCXX problem gets missed.

Practical: fceumm is ~3MB, a couple of seconds over the ad hoc link (1.7/4.3ms
RTT, 78ms TCP connect). `cores/staged/` and the existing state-transfer path are
the natural models.

### What was built before the freeze

Two halves, because the negotiation has to be settled before a core is opened
and the app cannot know which core a game will use until one is launched.

**At arm time, through the broker.** The detached host broker serves on port
55439 for the entire armed session - a non-blocking accept, so it costs nothing
until a client connects. The client exchanges manifests once, immediately after
arming, and stages whatever it needs into `cores/staged/`. The client drives,
and can push as well as pull:
if it cannot load the host's build but the host could load its own, it uploads
instead of giving up. Every read and write carries a 30s deadline.

**At launch, in the shim.** `staged_core()` runs before anything is exchanged -
if the app already settled this, there is nothing to wait for. Otherwise the
shim negotiates over the netlink handshake, which now carries core identity
(CRC, size, ELF machine, `library_version`, glibc floor, runtime glibc).

Matching is by **revision first**, CRC second. Of six shipped fceumm builds the
CRC gives six groups and the version five, so CRC-only matching would transfer
3MB between two builds that already agree.

Verified on hardware against a Python peer speaking the protocol
(`corepeer.py`): manifest correct against the release CRCs, a 3,293,712-byte
GET arriving byte-identical to the build pulled from GitHub and running to the
expected state hash, and a PUT landing in the host's staged directory. A
malformed manifest was rejected and the connection closed cleanly, which was
unintended but is the validation that matters most.

Wire structs are `__attribute__((packed))` with fixed-width fields: 78 bytes on
x86_64, aarch64 and armhf alike. That is the thing that would silently break
between a 32-bit and a 64-bit device.

### Drift found in review

Two gaps between what these docs described and what the code did. Both are
fixed; recorded because the shape recurs.

**The host only served cores while the setup app was alive.** Ticking from more
UI screens fixed navigation but still made setup-process exit a silent service
boundary. `netplay-broker.elf` now owns the listener and announcements until the
session ends, independently of which screen or game is active.

**The checks screen could not be left.** It had no input case at all: the render
switch handled nine screens and the input switch eight, so it drew "B BACK"
while nothing read B, and the only escape was a power cycle. There is no global
back handler, so every screen must handle its own.

Both are the same class - a behaviour that lives in one switch and is silently
absent from another. Worth checking, when adding a screen or a periodic task,
that the render switch and the input switch have the same number of cases.

## Guest mode

A thin-client role for the joining device, and the natural companion to core
sharing. The guest joins, then **stays in Netplay.pak** while the host leaves,
browses and picks a game. The host checks the guest's core build, they settle on
a mutually loadable one, and the host sends the ROM (and the save, conditionally
- see below). The guest loads it all and play begins pre-synchronised.

The point is that a guest needs to own nothing: no matching ROM, no matching
save, no curated library, no navigating to the same title.

**This removes the entire launch-interception machinery from the guest role.**
`install-stubs.sh`, `bind-mount.sh`, `wrap-pak.sh`, the boot hook and the
staleness tracking all exist for one reason: to intercept games launched through
NextUI's browser. A guest that never touches the browser needs none of it - the
app can exec the launcher directly with an explicit core and ROM. That is a much
smaller deployment surface, and it removes the failure mode where a NextUI update
invalidates staged copies mid-session.

A separate `Netplay-lite.pak` carrying only the join path is therefore small.
Two things it must keep:

- **WiFi recovery.** A guest is exactly the device that joins an ad hoc network
  and gets stranded when the host leaves. The watchdog and *Restore WiFi* are the
  part of Netplay.pak a guest needs most, not least.
- **A per-architecture shim**, which still ships per platform.

Better as a mode inside Netplay.pak than a fork, so host and guest cannot drift
apart as two codebases.

### Saves: send the host's, or keep your own?

Mode decides it, and the two modes want opposite things:

- **Shared screen**: both devices run one logical console in lockstep, so the
  save is part of the emulated state. The host's `.sav` **must** be sent or the
  two diverge immediately.
- **Link cable**: each console is genuinely itself. The guest uses **its own**
  save - existing, or freshly initialised - and the host's is never sent.

Pokémon is the case that forces this: trading *requires* two distinct trainer IDs
and parties. Copying the host's save gives you one trainer trading with himself,
and silently replaces a real save file.

The rule needs no per-game knowledge, which matters because the intuition does
not survive contact with the library. Cartridge type at header byte `0x147` says
whether a game has battery-backed RAM at all - across 2095 GB/GBC ROMs, 749 do
and 1346 cannot save:

```
Pokemon - Crystal          type=0x10  MBC3+TIMER+RAM+BATT   -> SAVE
Zelda - Link's Awakening   type=0x1b  MBC5+RAM+BATT         -> SAVE
Tetris DX                  type=0x03  MBC1+RAM+BATT         -> SAVE
```

Note **Tetris DX has a battery** - it saves high scores. So "Pokémon needs its
own save, Tetris does not" is wrong at the edges, and any rule built on
classifying games will keep finding cases like it. "The guest's saves are the
guest's" is correct for both without knowing which is which.

The header check is therefore not load-bearing for correctness. It is worth
having for two UX purposes: deciding whether to write a guest's save back to the
SD card rather than losing it with `/tmp`, and warning up front when a guest has
no save for a game that has one.

## Shared-screen netplay

Implemented. One instance per device, same game, inputs synced.

- **Input delay**, not rollback. Each side sends input for frame `F + delay`
  so the peer's has arrived when needed. Missing input stalls rather than
  running ahead. The app writes a transport floor into the session (3 frames
  ad hoc, 10 over an access point) with `input_delay_auto=1`; the shim may
  raise it from the measured round trip, and both sides adopt the higher of the
  two proposals. A session file with only `input_delay=N` pins it. The shim's
  own default of 6 applies only when the session gives no value.
- **State handshake**: the host serialises and ships its state (chunked); the
  client adopts it. Without this the two differ from frame one.
- **Divergence detection**: FNV-1a state hash every 300 frames, compared and
  logged. On a mismatch the client requests an authoritative snapshot; both
  devices pause, load the host's exact state, discard the old input timeline,
  and resume only after an acknowledged commit.
- **Save states unavailable while armed**: the shim reports a serialization
  size of zero and rejects frontend save/load calls. Stock minarch hardcodes
  its Save and Load menu rows, so they remain visible but fail closed; actually
  removing the rows requires shipping a patched frontend. Direct calls into the
  wrapped core remain available for handshake and recovery.
- **Port mapping**: stock minarch returns 0 for every port above 0, so the shim
  serves both ports from the synced buffers. Port 0 is the host's input on both
  devices.

## Ad hoc networking

**Working and validated in play.** [adhoc.md](adhoc.md) is the authoritative
account - hardware capabilities, the driver constraints that shape the design,
the full failure catalogue. Only the summary lives here, because the detailed
version used to be duplicated in this file and drifted badly out of date.

The shape, and how each part differs from the original design:

- **The host does not leave the house network.** The Brick's radio advertises
  `#{managed} <= 2, #{AP} <= 1, total <= 3`, so `hostapd` runs on `wlan1` while
  `wlan0` stays associated. Only one device moves. The earlier plan put the AP
  on `wlan0` and treated hosting as leaving the network.
- **`#channels <= 1`**, so the AP must take whatever channel `wlan0` is already
  on, read at runtime. A hardcoded channel is rejected by the driver.
- **The SSID does not depend on announcements.** It is `nextui-XXXX`, shown on
  the armed main screen, with a fixed passphrase; the client scans for the
  `nextui-` prefix and the user matches the code. Joining does not depend on
  the LAN announcements (which still carry the SSID and passphrase for clients
  on the same network), so the bootstrap problem is gone - a host that has
  already moved can still be found, which an announcement-only scheme cannot do.
- **No manual switch step.** The host raises its AP and stays reachable
  throughout.
- **Recovery is not physical.** Four routes back: the watchdog armed on join,
  the check at app launch, the join-failure path, and *Tools → Restore WiFi*.
  Proven on hardware by artificial network drops.

What it buys, same devices and game: **60.0 fps with 0% stalled frames**, versus
18 fps and 79% through the access point.

Hosting currently needs an AP-capable interface *and* `udhcpd`. The A30 has
neither, so on this pair the Brick hosts - a measurement, not a rule. Static
addressing would remove the `udhcpd` half.

## Network findings

Latency is **airtime contention, not topology** - as measured *through the
access point*, where two hops were no slower than one:

| Path | min | avg | max |
|---|---|---|---|
| A30 → AP (one hop) | 7.6 | 36.5 | 109 |
| Brick → AP (one hop) | 7.0 | 41.3 | 143 |
| A30 → Brick (two hops) | 7.4 | 31.6 | 130 |

26 APs on 2.4 GHz, four on the same channel and nine on the overlapping channel
6. Neither radio supports 5 GHz.

That reasoning predicted an ad hoc link would help by getting a **clear
channel** rather than by removing the router, and the ad hoc measurements bear
it out in direction but understate the size: device to device averaged **4.3ms
against 26-43ms**, a far larger gap than losing one hop accounts for. Note the
concurrent case cannot pick a clear channel at all - it inherits `wlan0`'s - so
some of the win is the direct path after all.

WiFi power save on the Brick cost 25× average latency (307ms → 12ms) and reverts
on reboot. With *Disable Wi-Fi Power Saving* on, the pak disables it while a
session is armed and restores the prior value on disarm. It is a real win on average but not a fix: measured A/B
over ad hoc, `off` averaged 26.3ms against `on` at 33.9ms, and neither brings the
worst case inside the input-delay window.

## Why GB link is latency-bound

gambatte's `NetSerial` is asymmetric: the serial **master** writes then does a
**blocking** read - one round trip per transfer, on the emulator thread - while
the **slave** polls with `ioctl(FIONREAD)` and carries on. Identical bytes each
way; only the master waits. At 35ms RTT the master is capped near 25
transfers/second against a 16.7ms frame budget.

The role is set by the game writing bit 0 of the `SC` register, unrelated to
which end is the TCP server. Confirmed on device: the lag follows whichever unit
initiates the match.

We added a receive deadline (`SO_RCVTIMEO`) so a stalled peer can no longer wedge
the emulator thread forever - previously that took the whole UI down, since input
is polled from inside `retro_run`.

## Why GBA link only covers some games

gpSP does not emulate the link cable. From `serial_proto.c`:

> Since real link cable emulation is very hard, here we partially emulate the
> other devices (GBAs) and using some fake data and real data recreate some
> serial protocols.

- `mul_poke`, `mul_aw1`, `mul_aw2` - per-game shims, each reverse-engineered
- `rfu` - generic wireless-adapter emulation, any adapter-aware game

Auto-detection matches the ROM header and covers **only the Pokémon family**.
Games using raw multi-player SIO - Mario Kart Super Circuit, for example -
cannot work through gpSP's link implementation.

mGBA has real cycle-accurate SIO for every mode in `src/gba/sio/lockstep.c`.
The stock libretro adapter exposes none of it, so Netplay's separately named
paired frontend builds the driver, owns two local GBA cores, and presents the
same dual ABI as paired Gambatte. Wi-Fi synchronizes inputs between mirrored
replicas rather than carrying individual SIO transfers. Host tests exercise raw
multiplayer SIO, and a two-Brick Mario Kart: Super Circuit race completed with
audio over both ad hoc and ordinary Wi-Fi. The enclosing paired call measures
16.25-16.29 ms because it includes presentation pacing; direct phase profiling
shows about 5 ms of actual headroom on Brick. A Brick/H700 pair has also
completed a race. It remains experimental because game coverage is narrow and
my282 cannot sustain the paired core.

## Dual instance

Run both consoles locally, network only controller inputs. Serial becomes an
in-process link at zero latency and input delay absorbs the network.

**The blocking risk is disproved.** Two independent instances coexist in one
process on both devices, by two methods:

| | two `.so` copies | `dlmopen(LM_ID_NEWLM)` |
|---|---|---|
| Brick | independent | independent |
| A30 | independent | independent |

Driven with different inputs they reach different states - shared globals would
have produced identical ones. Two file copies is preferred: plain `dlopen`, no
glibc namespace machinery, no 16-namespace ceiling.

Capacity is not a constraint either:

| | one instance | two concurrent |
|---|---|---|
| gambatte, A30 | 915 fps (15×) | 884 + 904 fps |
| mGBA, Brick | 291 fps (486%) | 267 + 267 fps |
| mGBA, A30 | 141 fps (234%) | 142 + 138 fps |
| gpSP, A30 (reference) | 483 fps (805%) | — |

The first Gambatte implementation used two physical `.so` copies with their
`retro_run` calls run concurrently and serial on a loopback TCP connection. It
worked and was too slow: the clock-owning console's synchronous send held an
A30/Brick pair to 22-23fps whichever device owned the clock. It is retired, and
retained disabled in `shim/shim.c` — see [Dual instance](multi-instance.md) for
the measurements that killed it.

What ships instead is a paired core, `gambatte_dual_libretro.so`, holding both
consoles and an in-memory serial coordinator behind a versioned ABI. One core
call advances the pair per frontend frame; the shim keeps devices, transport,
identity, input lockstep, and policy.

Startup is symmetric: each device sends its own console's raw SRAM/RTC, the peer
adopts it into the replica, and the host ships one paired checkpoint. A ready
barrier prevents either input timeline from beginning before both devices hold
the same two consoles.

Linked cartridges may differ when both devices own both of them; no ROM crosses
the network, so a pairing that cannot be hosted locally relaunches once on the
network-serial core. Both devices hash the paired state every 300 frames and
compare; a mismatch, or a lost connection, offers the Wait / Continue solo /
Exit overlay, and a reconnect re-pairs from scratch rather than resuming the
old timeline. On hardware it completed a Pokémon trade across 13,500 frames
with no desync, and the scripted Tetris run in `testing/devbridge/`.

## Durability: when does a frozen pak stop working?

| # | Assumption | If it changes | Fails |
|---|---|---|---|
| 1 | Emu paks invoke `minarch.elf` **unqualified** | our wrapper never runs | silently, safely |
| 2 | `getEmuPath` prefers SD `Emus/<PLATFORM>/<TAG>.pak` | stubs never found | silently, safely |
| 3 | `core.name` = `basename` truncated at last `_`, feeding `config_dir`/`states_dir` | staged shim yields a different name | **silently, relocating save states** |
| 4 | libretro core ABI - 25 `retro_*` entry points | a newly required symbol is missing | loudly, at core load |

Three of four fail safe: netplay stops engaging and games launch as stock.
**Assumption 3 is the only one that can cost user data.**

The app runs these as **arm-time checks** and refuses to arm on a failure,
turning silent breakage into a message. Verified on device: 7/7 paks resolve
minarch through PATH.

## Maintenance ledger

| | before | now |
|---|---|---|
| Rebuild trigger | **every NextUI release** | core source or toolchain change |
| Binaries per release | 6 + merge conflicts | 0 |
| Cores we own | 0 | gambatte, gpsp, paired gambatte and mGBA, compatibility set |
| Coupling to NextUI internals | deep (`minarch`, `api.h`) | none - libretro ABI only |

## Upstreaming would shrink this further

1. `HAVE_NETWORK=1` for gambatte in NextUI's cores makefile - one line, and we
   stop shipping a 3MB core
2. The gambatte serial receive timeout - upstreamable robustness fix
3. gpSP's two RFU patches - the original author's work, still unmerged
4. `SET_NETPACKET_INTERFACE` in `ma_environment.c` - ~10 lines, would let any
   netpacket core link without a shim

## Known gaps

- The shim's `DESYNC` hash needs a volatile-field exclusion list.
- `CORE MISMATCH` folds in `library_version`, so it flags builds differing only
  in flags - now reported, never enforced.
- **pcsx_rearmed cross-architecture is untested.** Same-architecture determinism
  is established (with `drc_thread` off); my282-to-arm64 is not. Blocked on
  having the same revision built for both - the A30 currently runs `94f15b3`,
  which no arm64 release ever shipped.
- **The A30's deployed cores predate its own pins.** `my282/cores/makefile` pins
  pcsx_rearmed to `050981b`; the device reports `94f15b3`. Rebuild and redeploy,
  or the pinning is documentation rather than fact.
- **Guest mode is designed, not built.** Its former dependency on executable
  core sharing must be redesigned around packaged compatibility cores.
- **snes9x on the A30 has thin margins.** 2.1x realtime headless, on an ordinary
  ROM; SuperFX and SA-1 titles are much heavier and untested.
- **Hosting still requires `udhcpd`**, which the A30 lacks. Removable: the
  client already knows the host is at `10.0.0.1`, so a static `10.0.0.2` would
  drop the dependency and widen hosting to any device that can raise an AP.
## Suggested order

1. **Static addressing** to drop `udhcpd`, so hosting is not gated on a binary
   half the measured fleet lacks.
2. Volatile-field exclusion for the state hash.
3. Ship or pin fceumm - the only remaining core whose release drift changes
   behaviour without changing state size is pcsx_rearmed, and fceumm is the
   cheapest to bundle (pure C, ~3MB).
4. Authentication/signing before reconsidering core sharing; redesign guest
   mode around packaged compatibility cores in the meantime.
