# Systems and cores

Every emulator pak in NextUI's Base and Extras bundles, and what Netplay.pak can
do with each. Pak and core names come from the NextUI tree
(`skeleton/BASE`, `skeleton/EXTRAS`, `makefile`), not from memory.

## How to read this

**Multiplayer?** - whether the original hardware supported two or more players on
one console and one screen. That is what shared-screen netplay reproduces: a
single emulator instance, both devices in lockstep, inputs exchanged per frame.
A handheld says "no" here because there is nothing on one console to share.

**Link Cable?** - two separate facts, because they are routinely conflated:

- `no` - the hardware had no link capability at all.
- `hardware only` - a real cable or adapter existed, but **the core does not
  emulate it**. Nothing the pak can do reaches these; the gap is upstream.
- `yes` - the core emulates the serial hardware, so the shim can carry its
  traffic via `SET_NETPACKET_INTERFACE`.
- `partial` - the core emulates *some* of the hardware's multiplayer, for
  *some* games. GBA is the only entry in this state; see below.

**Implementation** - what this pak actually does:

- `link` - link-cable play. Two instances, each its own screen; the core
  generates the traffic and we transport it.
- `netplay` - shared screen, one instance in lockstep.
- `-` - the shim does not drive this core, so launches are stock.

The mode is chosen by the shim from the core it wraps, not from a menu:
gambatte and gpSP get link, everything else gets shared screen.

## Base

Seven systems, installed by default.

| System | Core | Multiplayer? | Link Cable? | Implementation |
|---|---|---|---|---|
| FC | fceumm | yes (2, or 4 via Four Score) | no | **netplay** - played cross-device without desync |
| GB | gambatte | no | **yes** - Game Link emulated | **link** - proven (Tetris) |
| GBC | gambatte | no | **yes** - Game Link emulated | **link** |
| GBA | gpsp | rarely (single-cart multiboot) | **partial** - wireless adapter only, see below | **link** - proven for the supported titles, 213s unbroken (Advance Wars 2) |
| MD | picodrive | yes (2, or 4 via multitap) | hardware only - EXT port not emulated | **netplay** - proven (Streets of Rage 2, Gunstar Heroes) |
| PS | pcsx_rearmed | yes (multitap) | hardware only - SIO1 link not emulated | netplay in principle; **blocked by determinism** |
| SFC | snes9x (tg5040, tg5050, h700, my355) | yes (multitap) | no | shared-screen; packaged Supafaust fallback pairs with my282 |
| SFC | snes9x2005 (my282) | yes (multitap) | no | shared-screen; packaged Supafaust fallback pairs with arm64 |

The SFC split is not a build-flag difference. Four of the five platforms build
Snes9x 1.63; my282 builds Snes9x 2005, a 1.43-era fork - **different
emulators**, which cannot stay in sync directly at any input delay. Compatibility
negotiation resolves the split by loading the same pinned Supafaust revision on
both devices. Supafaust uses an explicitly encoded state stream: ARMv7 and
AArch64 produced the same 277,033-byte state and hashes for Kirby Super Star.

`PS` is the hardest determinism case in the set: pcsx_rearmed has two
architecture-specific subsystems (`assem_arm.c` vs `assem_arm64.c`, and 33 NEON
files in `gpu_neon`), so cross-device lockstep would need a portable CPU *and*
GPU path on the weakest device for the heaviest system. Out of scope.

## Extras

Thirty paks. Eight use a core the shim drives; the rest launch stock.

| System | Core | Multiplayer? | Link Cable? | Implementation |
|---|---|---|---|---|
| 32X | picodrive | yes | no | **netplay** - untested |
| FBN | fbneo | yes (arcade, 2+) | no | **netplay** - untested |
| FDS | fceumm | yes | no | **netplay** - untested |
| GG | picodrive | no | hardware only - Gear-to-Gear stubbed | **netplay** - poor fit, see below |
| SEGACD | picodrive | yes | no | **netplay** - untested |
| SG1000 | picodrive | yes | no | **netplay** - untested |
| SMS | picodrive | yes | no | **netplay** - untested |
| SUPA | mednafen_supafaust | yes (multitap) | no | **netplay** - untested |
| A2600 | stella2014 | yes | no | - |
| A5200 | a5200 | yes (up to 4) | no | - |
| A7800 | prosystem | yes | no | - |
| C64 | vice_x64 | yes | no | - |
| C128 | vice_x128 | yes | no | - |
| VIC | vice_xvic | yes | no | - |
| PLUS4 | vice_xplus4 | yes | no | - |
| PET | vice_xpet | no | no | - |
| COLECO | gearcoleco | yes | no | - |
| CPC | cap32 | yes | no | - |
| LYNX | handy | no | hardware only - ComLynx not emulated | - |
| MGBA | mgba | rarely | **yes** - full GBA SIO | **instanced link (experimental)** - paired core completed a Mario Kart race on two Bricks |
| SGB | mgba | yes (via SNES multitap) | no | - |
| MSX | bluemsx | yes | no | - |
| NGP | race | no | hardware only - link not emulated | - |
| NGPC | race | no | hardware only - link not emulated | - |
| P8 | fake08 | yes (2) | no | - |
| PCE | mednafen_pce_fast | yes (up to 5 via multitap) | no | - |
| PKM | pokemini | no | hardware only - IR link not emulated | - |
| PRBOOM | prboom | no | no (netgame not in this core) | - |
| PUAE | puae2021 | yes | hardware only - null-modem not emulated | - |
| VB | mednafen_vb | no | no - the link accessory never shipped | - |

## Core builds per platform

Which revision of each core a platform ships is not documented anywhere in the
NextUI tree, so this is read from the build definitions:

| platform | source |
|---|---|
| my282 | local NextUI checkout |
| tg5040, tg5050 | local NextUI checkout |
| h700 | `pvaibhav/NextUI`, branch `h700` |
| my355 | `apommel/NextUI`, branch `my355-latest-rebase` |

**The four arm64 platforms are byte-identical.** `workspace/<platform>/cores/makefile`
for tg5050, h700 and my355 does not differ from tg5040 by a single character -
same 28 cores, same three pins, same flags. Whatever is true of one is true of
all four. my282 is the only platform with its own core set.

| core | my282 | tg5040 | tg5050 | h700 | my355 |
|---|---|---|---|---|---|
| picodrive | `b0be121b` | `b0be121b` | `b0be121b` | `b0be121b` | `b0be121b` |
| fbneo | - | `6a5cc250` | `6a5cc250` | `6a5cc250` | `6a5cc250` |
| pokemini | - | `78656d46` | `78656d46` | `78656d46` | `78656d46` |
| fceumm | `afe65ef1` | floats | floats | floats | floats |
| gpsp | `69e86ebe` | floats | floats | floats | floats |
| pcsx_rearmed | `050981b6` | floats | floats | floats | floats |
| gambatte | floats | floats | floats | floats | floats |
| snes9x | - | floats | floats | floats | floats |
| snes9x2005 | floats | - | - | - | - |
| mednafen_supafaust | - | floats | floats | floats | floats |

### What "floats" means, and why it matters here

From `workspace/all/cores/makefile`:

```make
cd src && git clone --depth 1 ... $(1)
$(if $($1_HASH), ... git checkout $($1_HASH) ...)
```

With no `_HASH`, the build takes a shallow clone of **HEAD at the moment the
release was built**. The revision is therefore a function of the build date, not
of anything recorded in the repository - so two platforms, or two releases of
the same platform, can ship different code with no visible difference in the
tree. That is precisely how my282 and tg5040 diverged on fceumm.

The my282 pins for fceumm, gpsp and pcsx_rearmed were added by this project to
stop that; the comment in that makefile records why. They are the exception,
not the norm.

### Consequences for shared-screen play

- **picodrive is the only core pinned identically across all five platforms**, so
  MD, SMS, SG1000, 32X, SEGACD and GG are the safest systems for cross-device
  shared screen. That is not luck - it is the one core someone pinned deliberately.
- **fbneo and pokemini are pinned across the arm64 four** but absent on my282.
- **Everything else floats.** Two devices of the *same* platform, flashed from
  releases built weeks apart, can carry different fceumm or gambatte builds and
  desync with no way to tell from the version string. `CORE MISMATCH` folds
  `library_version` in for this reason.
- **SFC cannot cross the my282 boundary at all**, and no pinning fixes it: it is
  snes9x versus snes9x2005, two different emulators.

Verified on hardware: `gambatte v0.5.0 9b3b5e3` and `picodrive 1.2.3` report
identically on the A30 and the Brick.

## Notes on the awkward cases

**Game Gear is the clearest "so close" entry.** The hardware had Gear-to-Gear,
and picodrive's Game Gear I/O area is a plain 8-byte array:

```c
unsigned char io_gg[0x08];                 /* pico/pico_int.h:383 */
```

Six references in the whole tree. Port `0x00` (start/region) and `0x06` (PSG
stereo) do real work; ports `0x01`-`0x05` - parallel data, direction, TX, RX,
serial control - are pure storage, written and read back with nothing between
them. `case 5: d = Pico.ms.io_gg[5] & 0xf8` masks off the low three status bits,
so a game polling "has a byte arrived?" is told no, permanently.

That is a deliberate quiet stub rather than an oversight, and it means GG link
is blocked in the core, not in the transport. Shared screen is a poor substitute
for the games you would want it for - Sonic 2 versus, Columns head to head - as
those give each player their own view, which one shared instance cannot show.
The same masking covers SMS.

**GBA is narrower than "link cable" suggests.** gpSP does not emulate the GBA
link cable (the serial multi-play port used by most multiplayer GBA games). What
it emulates is the **Wireless Adapter (RFU)**, which was a later accessory that
comparatively few games ever supported - and gpSP's implementation is
effectively special-cased around a short list:

- Advance Wars 1 and 2
- Pokemon Generation III (Ruby/Sapphire/Emerald/FireRed/LeafGreen)

So the GBA row is real and proven, but it covers those titles rather than the
GBA multiplayer library. A game that used the serial cable and never supported
the wireless adapter has nothing for the shim to carry, no matter how well the
transport works. This is the main argument for the mGBA row below.

**mGBA emulates GBA link properly, but not over libretro's netpacket
interface.** Checked against the source rather than assumed: `libretro.c` at
revision `925f0f0` contains no reference to `SET_NETPACKET_INTERFACE`. An
earlier draft of this file claimed it did; that was wrong.

What it has instead is better suited to the instanced route. mGBA ships a
**lockstep coordinator** - `src/gba/sio/lockstep.c`, with a matching
`core/lockstep.c` - designed to link several GBAs running in one process, which
is what mGBA's own desktop frontend uses for multiplayer. It arbitrates transfer
start, mode changes and hard sync across up to four players behind a mutex.

Two limitations found by inspecting the stock build were:

- The libretro build compiles `src/core/lockstep.o` but **not**
  `src/gba/sio/lockstep.o`, so the GBA SIO driver that would use it is absent
  from the shipped core.
- It is built with `-DDISABLE_THREADING`, which matters because the coordinator
  is written to be driven from multiple threads.

The experimental paired core now takes the second route. Its build adds the GBA
lockstep driver and a separate libretro frontend owning two `mCore`s, a
`GBASIOLockstepCoordinator`, and a cooperative `runLoop` scheduler. It implements
the same dual ABI used by paired Gambatte, so Wi-Fi carries controller inputs and
checkpoints rather than SIO exchanges. Synthetic tests complete hundreds of raw
multiplayer transfers without a lockstep assertion, and two Bricks completed a
Mario Kart: Super Circuit race with audio. This is an experimental
implementation, not a replacement for the stock mGBA core. Beta artifacts build
for tg5040, my282 and h700. Paired hardware play is validated on two Bricks and
on a Brick with an H700 (`testing/devbridge/mkart.test`); my282 cannot sustain
the current same-thread paired core.

**Shared screen requires the same core build on both devices.** Every `netplay`
row above assumes it. `NO_ARM_ASM=1` is necessary and sufficient for picodrive,
established against every available build including Trimui's own. See
[state-sync.md](state-sync.md).

**"Untested" means untested, not expected-to-fail.** The eight Extras rows with
a supported core should work; none has been run. FBN is the most likely to
surprise, because arcade cores vary in how much state they keep outside the
serialised block.
