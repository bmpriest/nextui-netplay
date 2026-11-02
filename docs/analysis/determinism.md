# Determinism

Shared-screen netplay and instanced link play both need every device to compute
**bit-identical** results from identical inputs. This is what was measured about
that, core by core. How state moves between the devices is the third half of the
problem and lives in [state-sync.md](state-sync.md).

Measured on a TrimUI Brick (`tg5040`, AArch64) and a Miyoo A30 (`my282`, ARMv7).
Three independent obstacles sit in the way. The first two are about the cores;
the third is how state moves between them.

## 1. Architecture-specific emulation

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

## 2. Core revision drift

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

## Per-core outlook

| Core | Assessment |
|---|---|
| **fceumm** | Zero arch-specific files, zero inline asm. Pure C. **Cross-architecture identical**: armhf and arm64 builds of `afe65ef` produce the same state hash (`0733dee4`) and the same state size. Revision drift does change behaviour, but it also changes the state size, so a mismatched pair is refused rather than desyncing. |
| **picodrive** | Solved, and now confirmed across architectures: `2.01-strict-portable` (armhf) and `2.01-b0be121` (arm64) both produce `59f99520`. `NO_ARM_ASM=1` alone is necessary and sufficient. The only core pinned identically on all five platforms. |
| **gambatte** | Same commit both sides (`9b3b5e3`, both ours). No asm cores. Untested. |
| **pcsx_rearmed** | Deterministic **only with `pcsx_rearmed_drc_thread=disabled`** - see below; the shim now forces it. Cross-architecture remains untested, and the arch-specific subsystems are still there: `assem_arm.c` vs `assem_arm64.c` (MIPS recompiler) and 33 NEON files in `gpu_neon`. Same-architecture play is plausible; my282-to-arm64 is not established. |
| **SNES** | **Resolved through compatibility fallback.** Full Snes9x 1.63 remained deterministic but only sustained 46–50 fps in live A30/Brick lockstep. Snes9x 2005 was rejected because it serializes raw native structs, making its state architecture-dependent. Pinned Supafaust uses an encoded 277,033-byte state; A30 and Brick produced identical hashes (`f0e38316` after 120 frames and `bf872104` after a further 30-frame serialize probe), exact round trips, and about 81 headless fps on A30 for Kirby Super Star. |

## How many builds are actually in the wild

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

## pcsx_rearmed is nondeterministic by default

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

## Measuring determinism is itself error-prone

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

## 3. Moving state between the two cores

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

## Consequence

Cross-architecture shared-screen netplay needs matched cores - a port-level fix
(align my282 with tg5040), not a pak-level one - **and** a state transfer that
puts both sides through the same `unserialize` and keeps `serialize` calls
symmetric. Matched cores alone are not enough.

**Link play has no such requirement** - each device runs its own game and the
cores exchange messages, which is why Brick↔A30 GBA link works across two
architectures, and why Tetris over gambatte ran 13½ minutes clean while MD was
still desyncing.

## Core portability

Behaviour follows source revision, not toolchain: a core built for one AArch64
platform runs identically on another of the same revision. This was measured for
a design that sent the host's core to the guest. That design is frozen (executable
transfer is compiled out; the packaged compatibility cores replaced it), and its
implementation is in git history (`docs/implementation.md` at `442efa2`). The
measurements still decide which compatibility builds can serve which devices.

Verified by loading three foreign cores on a Brick:

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

### What can run where

| platform | arch | glibc floor | extra libs |
|---|---|---|---|
| tg5040 | AArch64 | GLIBC_2.17 | - |
| h700 | AArch64 | GLIBC_2.17 | - |
| my355 | AArch64 | GLIBC_2.17 | - |
| tg5050 | AArch64 | **GLIBC_2.33** | - |
| my282 | **ARM** | GLIBC_2.15 | - |

**my282 is a hard boundary.** 32-bit ARM cannot load an AArch64 object, and no
copying fixes it. An A30 can only run a core built for an A30.

Among the arm64 four it works, with one asymmetry: cores built for
tg5040/h700/my355 need only GLIBC_2.17 and load essentially anywhere, while a
tg5050-built core wants 2.33 and could be refused by an older device. **Build
shared cores against the lower floor**, as the compatibility cores are.

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

**Load by trying, not by predicting.** Check architecture first, then `dlopen`
the core and report the result. Parsing ELF version tables to guess is how the
GLIBCXX problem gets missed.
