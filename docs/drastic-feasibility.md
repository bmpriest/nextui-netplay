# DraStic Wi-Fi and Multiplayer Feasibility

Status: initial investigation plus A30 baseline package, 2026-08-16

## Implementation outcome (2026-08-31)

Feasibility is now proven on the TrimUI Brick, beyond the initial research in
this document. The AArch64 hooks, cycle/interrupt adapter, Wi-Fi state model,
and local NetLink transport support live two-device discovery, authentication,
association, multiplayer commands, and a stable Asphalt Urban GT 2 lobby. The
guest remains present for minutes before gameplay starts.

The implementation is not complete: pressing the host's required `A` action to
start currently leaves player 2 visible on the host while the client reports a
communication error. Work is paused at command completion and IRQ-visible state
across that transition. The A30 remains a valid earlier hook target, but active
multiplayer development and hardware validation are Brick-first.

## First follow-up question

Before implementing anything, determine whether the relevant DraStic build and
hooking approach are viable on the Miyoo A30, or whether this is effectively a
TrimUI Brick-class target only.

The investigation should establish:

- the A30 CPU architecture, ABI, available RAM, graphics stack, audio stack,
  and firmware/kernel constraints;
- which bundled DraStic executable can run on it (especially the ARMv7
  `drastic32`/2.5.0.4 build);
- whether Steward Fu's replacement SDL2 and detour libraries already had an
  A30 target in repository history, and why that support was later removed;
- whether Advanced DraStic has a compatible 32-bit preload library, or whether
  a new platform build is required;
- performance headroom after adding the Wi-Fi state machine, packet transport,
  and optional virtual access point; and
- whether two local DraStic instances are realistic on the A30, if that becomes
  useful for testing, versus testing with two physical handhelds.

Do not assume that the MinUI package's current `tg5040` target restriction means
the emulator itself cannot run on the A30. Packaging support, binary ABI,
graphics integration, and actual performance are separate questions.

## Executive conclusion

There is enough information in Steward Fu's `nds` repository to build a serious
Wi-Fi proof of concept around the 32-bit DraStic 2.5.0.4 binary. There is a clean
MMIO interception boundary, an existing runtime detour mechanism, useful symbol
and DWARF information, and visible access to DraStic's ARM7 memory, interrupt,
event, and DMA structures.

This is not a small socket shim. The six Wi-Fi MMIO handlers can be replaced,
but a working implementation also needs a DS Wi-Fi hardware state machine,
cycle-derived timing, ARM7 interrupt injection, reset handling, a packet
transport, and either a virtual access point for WFC or a low-latency protocol
for local multiplayer.

The likely order of difficulty is:

1. Instrument the existing fake Wi-Fi handlers.
2. Implement register and packet-RAM behavior.
3. Reach a synthetic access point from the DS Wi-Fi settings.
4. Add WFC/infrastructure networking.
5. Add local DS multiplayer, whose timing is substantially less forgiving.

The reconstructed assembly applies directly to the 32-bit 2.5.0.4 binary. The
2.5.2.0 and 2.5.2.2 AArch64 binaries require their own addresses, layouts, and
hook validation.

## A30 baseline result

The A30 is a real target for this work, not merely a hypothetical alternative
to the TrimUI Brick. Steward Fu's history contains an explicit Miyoo A30 port:

- commit `3e96e1ac` removed `Makefile.miyoo_a30` with the message "Dropped A30
  support";
- its parent, `68456b9f`, still contains the A30 build flags, launch assets,
  input mapping, video backend, ALSA backend, and runtime libraries;
- the build targets ARMv7-A, Cortex-A7, NEON, and the hard-float ABI; and
- the packaged DraStic 2.5.0.4 executable has the matching ARMv7/VFPv4
  hard-float ABI and uses `/lib/ld-linux-armhf.so.3`.

There is also a maintained downstream confirmation: MyMinUI's current `my282`
release ships an A30 build of the same steward-fu compatibility layer. Its
release archive deliberately omits the proprietary DraStic executable and
expects the user to supply it separately.

An initial NextUI package has now been produced as
`dist/NDS-my282.pak.zip`. It combines that current A30 runtime with the local
`steward-fu/nds` `drastic32` executable, uses a NextUI-aware launcher, and
installs a `Roms/Nintendo DS (NDS)` directory. Static validation confirms:

- the release input matches its published SHA-256 digest;
- the executable and replacement libraries are 32-bit ARM EABI5 objects;
- the executable targets ARMv7 with VFPv4 hard-float arguments;
- the archive passes a complete ZIP integrity test; and
- the expected DraStic binary is 2.5.0.4, the version to which the recovered
  symbols and Wi-Fi hook addresses apply.

This settles binary and platform feasibility much more favorably than the
initial evidence did. It does **not** yet settle real performance, graphics,
audio, suspend/resume, or input behavior under NextUI. Those require an A30
smoke test with a ROM. The sensible decision gate is now measured A30
performance, not whether an A30 port exists.

## What the Steward Fu repository is

`steward-fu/nds` is not an independent source-level Nintendo DS emulator. It is
a porting and binary-hooking framework built around original closed-source
DraStic executables.

Its normal build produces:

- `libcommon.so` for configuration and shared support;
- `libdtr.so` for hard-coded DraStic detours;
- a replacement `libasound.so.2`;
- a heavily customized SDL2 implementation for display, input, layouts, menus,
  and platform integration; and
- assets and device launch scripts.

The launch scripts still execute an original DraStic binary. The detour layer
patches known function addresses at runtime and accesses known global/state
addresses. Advanced DraStic descends from this work but moves much of the
hooking into `libadvdrastic.so` loaded with `LD_PRELOAD` and supports multiple
DraStic versions.

The repository's `re-drastic/32/main.s` is a roughly 348,000-line ARM assembly
reconstruction of DraStic 2.5.0.4. It is valuable reverse-engineering material,
but it is not currently an independently rebuilt emulator. Its `patch.py`
assembles and links `main.s`, then copies only the generated `.plt` section into
the original `drastic32` executable to produce `re-drastic`.

Workspace source examined:

- `/home/benja/dev/sbcs/cores/nds/re-drastic/32/main.s`
- `/home/benja/dev/sbcs/cores/nds/re-drastic/32/patch.py`
- `/home/benja/dev/sbcs/cores/nds/drastic/drastic32`
- `/home/benja/dev/sbcs/cores/nds/detour/hook.c`
- `/home/benja/dev/sbcs/cores/nds/detour/hook.h`
- `/home/benja/dev/sbcs/cores/nds/sdl2/src/video/nds/nds_video.c`

## The usable Wi-Fi hook boundary

DraStic routes ARM7 accesses to the DS Wi-Fi range through six isolated
functions:

| Function | DraStic 2.5.0.4 address | Current behavior |
| --- | ---: | --- |
| `store_wifi_8` | `0x08007fe4` | No-op |
| `load_wifi_8` | `0x08007fe8` | Returns zero |
| `load_wifi_16` | `0x0800b10c` | Fake register/RAM behavior |
| `load_wifi_32` | `0x0800b4d4` | Returns zero |
| `store_wifi_32` | `0x0800b4dc` | No-op |
| `store_wifi_16` | `0x0800b4e0` | Fake register/RAM behavior |

The inferred ABI is:

```c
uint8_t  load_wifi_8(void *memory, uint32_t address);
uint16_t load_wifi_16(void *memory, uint32_t address);
uint32_t load_wifi_32(void *memory, uint32_t address);

void store_wifi_8(void *memory, uint32_t address, uint32_t value);
void store_wifi_16(void *memory, uint32_t address, uint32_t value);
void store_wifi_32(void *memory, uint32_t address, uint32_t value);
```

DraStic's ARM7 memory-map initialization installs these functions as external
handlers for the Wi-Fi address range. Replacing them gives an external module
control over Wi-Fi register reads/writes and packet RAM accesses without
patching ARM7 game code.

The existing detour system already overwrites target instructions with a branch
to replacement code. Adding these six functions to its hook table should be a
routine extension, although the hooks will be specific to this exact binary.

## Recovered internal state

The unstripped `drastic32` binary contains unusually useful DWARF information.
Together with `main.s`, it exposes these `memory_struct` offsets:

```text
memory_struct + 0x17040  wifi_mem
memory_struct + 0xFB580  wifi_io_registers
memory_struct + 0xFB980  wifi_baseband_buffer
```

It also exposes:

- `system_struct.cpu_arm7`;
- the `cpu_struct` layout and `active_interrupts` field;
- ARM7 I/O-register storage;
- `ARM_CPU_IRQ_WIFI = 0x01000000`;
- `DMA_START_TYPE_ARM7_WIFI_GBA = 3`;
- the generic event list and scheduler;
- `execute_arm_trigger_interrupts`;
- `trigger_event_dma`; and
- reset and savestate entry points.

This is enough information to build adapters instead of blindly scanning memory
at runtime.

## What hooking the MMIO functions provides

Replacing the handlers gives control over:

- Wi-Fi register reads and writes;
- Wi-Fi packet RAM;
- TX and RX buffer ports;
- RF and baseband registers;
- command, beacon, local, and reply TX slots;
- Wi-Fi interrupt enable/status registers;
- counters and compare registers; and
- writes that request packet transmission.

This is the correct boundary between the emulated ARM7 software and a new Wi-Fi
hardware model.

## Missing integrations

| Requirement | DraStic status | Required work |
| --- | --- | --- |
| Wi-Fi MMIO interception | Clean hook boundary | Replace six handlers |
| Wi-Fi RAM | Existing fake storage | Reuse carefully or use shadow state |
| Wi-Fi register model | Mostly fake | Implement hardware behavior |
| ARM7 Wi-Fi IRQ | Generic IRQ machinery exists | Add safe IRQ injection and CPU wakeup |
| Wi-Fi DMA | Generic start type exists | Determine and trigger required cases |
| Microsecond timing | No Wi-Fi event | Integrate a cycle-derived periodic tick |
| Reset | Hookable | Reset external Wi-Fi state with DraStic |
| Savestates | No real Wi-Fi state | Serialize state or disconnect on load |
| WFC access point | Absent | Implement AP plus network backend |
| Local multiplayer | Absent | Implement timestamped low-latency transport |

### Timing

melonDS does not merely send a packet when a TX register is written. Its Wi-Fi
model advances microsecond counters, processes multiple transmit and receive
phases, changes RF status, fires Wi-Fi sub-interrupts, manages command/reply
windows, and consumes timestamped packets.

DraStic has a generic event scheduler but no Wi-Fi event. Candidate approaches
are:

1. Add an external Wi-Fi event to DraStic's event list.
2. Hook the event executor and advance Wi-Fi from elapsed emulated cycles.
3. For an early WFC-only experiment, advance at scanline or another existing
   event boundary.

The first two are preferable. A scanline-level update may be adequate to prove
that DS software can discover and associate with a synthetic AP, but it is
unlikely to be precise enough for reliable local multiplayer.

The timer must derive from emulated DS cycles, not a host wall-clock thread.
Wall-clock timing would make behavior dependent on frameskip, load, and host
scheduling and would make multiplayer synchronization much harder.

### Interrupts

A correct Wi-Fi sub-interrupt sets the relevant bit in the Wi-Fi `W_IF`
register. If `W_IF & W_IE` becomes nonzero, the hardware raises ARM7 interrupt
bit 24.

The adapter must then follow DraStic's normal device IRQ path:

1. Set bit 24 in ARM7 `IF`.
2. Recalculate pending interrupts against `IE` and `IME`.
3. Update DraStic's active-interrupt state.
4. Wake a halted ARM7.
5. Ensure the interpreter/recompiler observes the interrupt promptly.

Merely writing the ARM7 `IF` storage may not wake the CPU or invalidate its next
action. Existing game-card, timer, and display event functions show the required
pattern and should be used as references.

## Using melonDS as a reference

melonDS has a suitable conceptual split:

```text
Wifi::Read / Wifi::Write
    |
    +-- register, RF, and baseband model
    +-- packet RAM and TX/RX state machines
    +-- timer and interrupt generation
    +-- WifiAP for infrastructure/WFC
    +-- Platform::MP_* for local multiplayer transport
```

The MMIO boundary aligns well with DraStic's six handlers. The reusable ideas
are the register semantics, TX/RX phases, interrupt behavior, packet formats,
and separation between emulated hardware and host transport.

It is not a drop-in component. A direct port must remove or adapt dependencies
on melonDS's `NDS` object, scheduler, firmware classes, savestates, IRQ methods,
and platform network APIs.

### Licensing constraint

melonDS is GPLv3. Copying its implementation into a shared object loaded into
closed-source DraStic creates a serious combined-distribution licensing issue.
The surrounding Steward Fu code being LGPL does not make the bundled DraStic
core open source.

A cleaner path is to use public DS hardware documentation and observed behavior
to write an independent implementation, using melonDS for validation rather
than copying its code. Distribution of patches or binaries involving DraStic's
proprietary binary and reconstructed assembly also needs separate review.

## RetroArch and melonDS DS

The newer melonDS DS libretro core has two distinct networking paths:

- WFC uses melonDS's virtual access point and host networking.
- Local DS multiplayer passes melonDS's timestamped multiplayer packets through
  RetroArch's netplay session.

This is explicit emulator networking integration, not ordinary input-only
RetroArch netplay automatically making DS wireless work.

A standalone DraStic implementation does not require RetroArch. It can expose a
transport equivalent to melonDS's multiplayer platform interface and carry
those messages over low-latency UDP between handhelds. The difficult part is
preserving DS command/reply timing and packet timestamps.

Local DS wireless expects extremely low latency. Initial testing should use two
devices on the same physical LAN, preferably wired where supported. Internet
tunnels and ordinary Wi-Fi latency may cause sessions to drop even after the
emulation is functionally correct.

## Recommended proof-of-concept sequence

### 1. Establish the target device

Answer the A30-versus-Brick question at the top of this document. Select one
known executable, ABI, and hook library before modifying the emulator.

The 32-bit 2.5.0.4 build is the best research target because its assembly and
DWARF data are available. If it cannot run acceptably on the A30, the project
must decide whether to target the Brick first or reverse engineer an applicable
AArch64 build.

### 2. Instrument DraStic 2.5.0.4

Hook all six Wi-Fi functions. Initially log address, width, value, ARM7 cycle or
timestamp, and game state while preserving the original fake behavior. Capture
traces from:

- opening Nintendo WFC settings;
- scanning for an access point;
- entering a game's local multiplayer lobby; and
- attempting to host and join a session.

This validates the ABI and hook stability before implementing hardware.

### 3. Implement the hardware-facing core

Add independently owned state for:

- register I/O;
- 8 KiB packet RAM and mirrors;
- baseband and RF registers;
- power state;
- microsecond and beacon counters;
- TX/RX slots and phase timing;
- Wi-Fi sub-interrupt flags; and
- RX/TX queues connecting to a host backend.

Keep this module independent of DraStic. Connect it through a narrow adapter for
MMIO, cycles, IRQ, firmware identity, and transport.

### 4. Prove a synthetic AP

Implement enough management frames for the DS to:

1. power on Wi-Fi;
2. scan;
3. see a synthetic AP;
4. authenticate;
5. associate; and
6. complete a connection test up to the point that IP transport is required.

This exercises most MMIO, timing, RX, TX, and IRQ plumbing without tackling
local multiplayer's strict reply windows.

### 5. Add WFC transport

Add Ethernet/IP translation and a lightweight userspace network backend. Test
DNS and replacement WFC services. WFC should be the first end-to-end networking
target because it is more tolerant of timing than local wireless multiplayer.

### 6. Add local multiplayer

Implement the equivalent of timestamped send-packet, send-command,
send-reply, send-ack, receive-host-frame, and receive-replies operations. Use a
small UDP protocol with session identity, channel, sender identity, emulated
timestamp, message type, sequence number, and payload.

Start with two physical handhelds on the same LAN. Download Play and games with
tight synchronization should come after simpler multi-card local multiplayer.

## Current risk assessment

- **MMIO hook feasibility:** high for the exact 32-bit 2.5.0.4 binary.
- **Ability to reconstruct necessary DraStic state:** moderately high because
  the binary contains DWARF data.
- **Synthetic AP/WFC prototype:** plausible but substantial.
- **Stable WFC support:** medium risk, especially around firmware and network
  translation.
- **Stable local multiplayer:** high risk because of timing and latency.
- **Multi-version support:** high effort; offsets and patch mechanics differ.
- **A30 viability:** confirmed on device with Mario Kart DS and Pokemon
  Platinum. Rendering, backlight handoff, menu entry, and clean exit work;
  touch mapping and occasional menu instability remain unrelated integration
  issues.
- **Distribution:** significant DraStic-proprietary and GPL compatibility
  concerns if melonDS code is copied directly.

## Primary upstream references

- melonDS Wi-Fi hardware model:
  <https://github.com/melonDS-emu/melonDS/blob/master/src/Wifi.cpp>
- melonDS virtual access point:
  <https://github.com/melonDS-emu/melonDS/blob/master/src/WifiAP.cpp>
- melonDS DS libretro documentation, including WFC and LAN netplay:
  <https://github.com/libretro/docs/blob/master/docs/library/melonds_ds.md>

## Implemented trace milestone (2026-08-17)

`drastic-wifi/` now builds passthrough trace hooks for both current targets:

- A30: 32-bit ARMv7 DraStic 2.5.0.4 at fixed addresses `0x0800b10c`
  (`load_wifi_16`) and `0x0800b4e0` (`store_wifi_16`).
- TrimUI Brick: AArch64 PIE DraStic 2.5.2.0 `59a7`, build ID
  `5665dbdad6bec617b3f828afb2f69b0a14460764`, at image-relative offsets
  `0x13660` and `0x138f0`.

Both hooks validate the exact instructions they will replace and preserve those
instructions in a trampoline. Passthrough mode calls the original handler;
model mode intercepts after the same validated entry. The Brick offsets were
taken from the symbol-bearing executable with the same build ID and verified
against the stripped executable shipped by the MinUI pak. The first build
intentionally hooks only 16-bit accesses: it is enough to capture the
substantive register model while avoiding the adjacent tiny 8/32-bit ARMv7
stubs.

Installable research builds are emitted as `dist/NDS-WiFi-a30.pak.zip` and
`dist/NDS-WiFi-brick.pak.zip`. They cap each trace at 16,384 events and do not
yet implement a radio, access point, transport, or multiplayer behavior.

### First device test and launcher correction (2026-08-18)

The A30 ran Mario Kart DS through its multiplayer menu without crashing, but
the launcher froze during exit. The emulator environment showed that
`LD_PRELOAD` and `DRASTIC_WIFI_TRACE` had been exported by the launcher. That
caused every subsequent launcher utility (`sleep`, cleanup commands, and so
on) to load a library that is valid only inside the DraStic address space. It
also explains why the 76-byte trace contained only the A30 header: a later
utility repeatedly opened and truncated the same file before failing its hook
installation. The corrected launcher applies both variables only to the
DraStic command.

The Brick exited with SIGSEGV before loading each tested ROM. All three trace
files were exactly 83 bytes, the length of the Brick trace header, proving that
the fault occurs during first-hook installation. Its crash report showed fault
address `0x2000`. The next diagnostic build derives the PIE base directly from
the kernel auxiliary vector (`AT_PHDR`/`AT_PHNUM`) rather than relying on an
empty-name `dl_iterate_phdr` entry, and records each installation stage and
address. This is not yet considered a confirmed Brick runtime fix; its next
trace will identify the exact failing stage if it still faults.

### Brick Advanced DraStic initialization boundary (2026-08-25)

The auxiliary-vector image-base fix was correct: repeated Brick traces showed
both AArch64 prologues validating, both trampolines being created, and both
targets being patched. Nevertheless Advanced DraStic then faulted at address
`0x2000` before either hooked handler was entered. Reversing the two
`LD_PRELOAD` entries did not change that result.

Advanced DraStic applies its own executable detours during early `main`, after
preload constructors have run. Installing the Wi-Fi jumps in our constructor
therefore changes code while the frontend wrapper is still establishing its
own hooks. The Brick path now validates the binary during construction but
starts a detached installer at the real `main` boundary and waits three seconds
before writing the two jumps. The A30 fixed-address path is unchanged.

An on-device Asphalt GT2 test ran normally for about a minute. Its trace reached
`status=ready-deferred` and exited cleanly without any Wi-Fi MMIO traffic, as
expected for ordinary single-player play. This proves Advanced DraStic and the
AArch64 trampolines can coexist when installation is ordered correctly. The
next Brick test is a Wi-Fi-capable game's multiplayer menu, which must produce
actual 16-bit accesses through the deferred hooks.

Mario Kart then entered its multiplayer menu and exited cleanly with the same
deferred passthrough build. Both the replacement and return trampoline executed:
the trace recorded `load16=entered`, `load16=original-returned`, and a final
summary of 846,043 reads, zero writes, and no model activity. The 16,384-record
trace cap was dominated by 16,278 reads from offset `0x019c`, hiding almost all
later activity. Tracing now retains the first 16 reads of that hot poll address
and one per subsequent 1,024 reads while still counting every access. A repeat
run can therefore characterize the remaining register sequence and determine
whether any later path reaches the validated store hook.

The sampled repeat also entered and left Mario Kart multiplayer cleanly. It
covered the complete session: 713,445 raw reads became 5,452 retained events,
with zero writes and no dropped retained events. This exposed a stable polling
loop over `0x00d0=0581`, `0x0040=0000`, `0x003c=0200`, `0x0214=0000`,
`0x0180=0000`, `0x015e=0000`, `0x0044=0000`, `0x0012=e03f`,
`0x01aa=0400`, `0x0038=000f`, and sampled `0x019c=0000`. Less frequent reads
also covered `0x00f8`, `0x00fa`, and the packet-RAM window near `0x0280`.
Status-like offset `0x015c` changed among several values during entry and exit,
including `00bf`, `006d`, `00a0`, `001f`, `0000`, and `00b4`.

That zero-write result was a hook-selection error, not hidden mutation. The
standalone raw Wi-Fi store at Brick offset `0x138f0` is only a fallback. The
actual ARM7 16-bit I/O dispatcher at `0x128d0` inlines its Wi-Fi write at
`0x131a8`. Hooking the dispatcher produced a stable Mario Kart run with 12,761
Wi-Fi writes, including interrupt, RF/baseband, timing, and packet-RAM setup.

Static cross-architecture matching then recovered `execute_events` at Brick
offset `0x216e0`. A cycle-only Mario Kart run completed with 4,800,229 event
callbacks, 2,435,704,932 master cycles, and 36,338,632 converted microseconds.
Its callers establish embedded CPU objects at `system + 0x15c6d50` (ARM9) and
`system + 0x25cd340` (ARM7). DraStic's interrupt paths independently establish
ARM7 `active_interrupts` at CPU offset `0x2108`, state at `0x2110`, and the ARM7
I/O region at `memory + 0x23070`.

An initial adapter attempt crashed because it followed an unrelated memory-map
pointer; the launcher was restored and the variables were separated. With the
event-supplied system pointer and embedded ARM7 offset, model shadowing, event
timing, adapter initialization, and finally IRQ24 injection each completed
Mario Kart multiplayer without a crash. The final run recorded 572,496 reads,
11,501 writes, 4,007,718 event callbacks, and 26,760,872 emulated microseconds.

## Independently owned MMIO state milestone (2026-08-23)

`drastic-wifi/src/model.c` is now the first executable part of the
hardware-facing core described in step 3. It owns, without depending on either
DraStic executable's internal structure:

- the DS Wi-Fi 8 KiB packet RAM and its mirrored window;
- the 4 KiB register window and mirrors;
- the 256-byte baseband register file; and
- access counters which distinguish model traffic from passthrough tracing.

The initial register behavior retains DraStic's existing 2.5.0.4 placeholder
baseline: fixed mode/power/busy reads, ordinary register storage, and the
baseband read/write port. It additionally implements `W_IF`, `W_IE`, and
`W_IFSet`, including write-one-to-clear flags, the hardware force-set mask, and
one callback on each aggregate IRQ low-to-high transition. That callback is not
yet connected to DraStic's ARM7 IRQ24 path on AArch64. On the A30, recovered
2.5.0.4 DWARF and the emulator's own device-event assembly establish the
version-specific path: `memory_struct.system` is at `0x0fba1c`, the embedded
ARM7 CPU is at `system + 0x1dad188`, its I/O pointer is at CPU offset `0x2080`,
and `active_interrupts` is at `0x2100`. The adapter latches bit 24 in ARM7 `IF`
and recomputes active interrupts from `IME`, `IE`, and `IF` using DraStic's
normal event-device formula. The exact MMIO-handler prologue remains the binary
version gate. Native tests prove the update semantics and the ARMv7 cross-build
succeeds, but an on-device test must still prove prompt CPU wakeup.

The portable model now has the other half of the timing boundary as well. Its
explicit `dwifi_model_advance_us` input accumulates elapsed emulated time onto
the DS Wi-Fi hardware's 8 us boundaries; it owns the four-halfword 64-bit
`W_USCount` and `W_USCompare` registers, masks their control bits, implements
forced compare IRQ14, and raises IRQ14 when an enabled counter crosses the
matching 1024 us boundary. Native tests cover partial-tick accumulation,
disabled counters, register halves, compare masking, and both scheduled and
forced interrupts. This API is deliberately not driven from `CLOCK_MONOTONIC`.
The A30 adapter below now drives it; the Brick still needs an independently
recovered event or cycle boundary.

### A30 event/cycle adapter (2026-08-23)

The symbol-bearing 2.5.0.4 executable identifies `execute_events(system_struct
*)` at `0x08018c74`. Its exact first two ARM instructions are validated before
any hook is installed. The function adds `system.cycles_to_next_event` at
offset `0x10` to the 64-bit `system.global_cycles` at offset `0x08`, then
dispatches every event due at that boundary. Video scheduling constants such as
the 3072-cycle visible scanline phase confirm that this is the DS
67,027,964 Hz ARM9/master-cycle domain.

In A30 model mode, the wrapper records `global_cycles` before and after the
original dispatcher and advances the portable Wi-Fi clock by that delta. The
converter uses an exact rational numerator and retains fractional cycles across
calls, avoiding cumulative drift from an integer cycles-per-microsecond
approximation. If reset or savestate activity moves the absolute counter
backward inside a callback, that single boundary contributes zero elapsed time
instead of wrapping to a huge delta.

This hook sits at the right interrupt boundary. The interpreter calls
`execute_arm_trigger_interrupts` for ARM9 and ARM7 immediately after
`execute_events` returns, while the recompiler checks pending interrupt state
at the corresponding return path. Timer-driven Wi-Fi sub-interrupts therefore
flow through the IRQ24 adapter before DraStic's existing CPU checkpoint. The
hook is absent in passthrough mode and remains unavailable on the Brick until
the AArch64 event and system layouts are independently verified.

### A30 model-mode test 1 (2026-08-24)

Mario Kart DS loaded normally but aborted with `double free or corruption
(!prev)` on entering its multiplayer menu in two runs. Advance Wars: Dual
Strike reached its wireless error path without aborting, then DraStic wedged
while exiting through its menu. Every captured Wi-Fi trace was exactly 70
bytes: only the trace-v3 constructor header survived, with none of the hook,
adapter, MMIO, event, or shutdown records. The ordinary launcher logs contain
the two status-134 aborts but no ARM kernel fault.

That trace signature is consistent with the earlier inherited-preload failure
in a narrower form. The launcher already scopes `LD_PRELOAD` to DraStic, but
DraStic itself can pass that environment to helpers invoked from its UI. The
working hypothesis is that a helper loads the fixed-address hook and reopens
the parent's trace with `O_TRUNC`.
The constructor now rejects commands other than DraStic before opening the
trace and removes all preload/hook variables from DraStic's environment before
its main function runs. The next test will determine whether this also explains
Mario Kart's heap abort; the truncated traces do not contain enough MMIO or IRQ
evidence to attribute that crash to the hardware model.

For recovery independent of DraStic's UI, the A30 launcher now writes a scoped
PID file and watches a hard-quit request. The packaged `hard-quit.sh` validates
that PID's command line, requests launcher cleanup, and escalates to SIGKILL
after six seconds. Stale or unrelated PIDs are refused.

The first guarded-build retest produced clean launcher recovery with status 137
for both Advance Wars and Mario Kart. This status is the A30 SDL compatibility
layer's ordinary menu Exit behavior: `prehook_select_quit` calls
`quit_drastic()` and starts a one-second fallback thread which executes
`kill -9` against DraStic's own PID. It is not evidence that the packaged
hard-quit helper ran. The runs produced no new Wi-Fi trace because the command gate accounted
for a direct loader target but not the actual packaged argument sequence:
`ld-linux-armhf.so.3 --library-path ./lib ./drastic`. The parser now searches
the arguments after a verified `ld-linux*` executable for an exact `drastic`
basename. Regression tests cover the packaged form as well as direct launches,
ordinary helpers, and a misleading non-loader command. The corrected ARMv7
library was installed on the A30 with the rejected build retained as
`libdrastic_wifi.so.pre-gate-fix`.

The corrected-gate retest produced full traces for both titles. Advance Wars
made 389 16-bit reads before its expected communication error; Mario Kart made
390 before aborting. There were no 16-bit writes in either trace, every returned
value was zero, and the access sequences were otherwise effectively identical.
Consequently no modeled interrupt-enable, interrupt-status, or timer-control
write occurred, so neither a Wi-Fi sub-interrupt nor ARM7 IRQ24 can be the
immediate abort trigger. To isolate the remaining high-risk boundary, the A30
event trampoline is now separately opt-in through `drastic/wifi_cycle.enabled`.
The installed test build retains model mode but has no cycle marker; its trace
must say `event-cycle-hook=disabled`.

That cycle-disabled Mario Kart retest still aborted with status 134 after 390
reads and no writes. The trace confirmed that the event hook was absent, ruling
out its trampoline and cycle conversion. No modeled interrupt callback could
run without a write. The following diagnostic build now uses a hybrid MMIO
path: it returns DraStic's original read values, mirrors writes through both the
original handler and portable model, and does not bind the ARM7 IRQ adapter
until the first write. This preserves the emulator's internal Wi-Fi shadow
state while retaining complete traces and model accounting; its trace header
says `mode=hybrid16`.

A locally supplied 256 KiB DS firmware dump was checked without copying it
into the repository or release artifacts. Its SHA-256 is
`330c59198faec64b40e96cc875b422d340fc9ab1a9e32e9bd9b7baa1dbe6ba7f`,
exactly matching the `nds_firmware.bin` generated by the steward-fu source and
already included in the Brick package. It therefore does not provide a
different Wi-Fi calibration image for this failure. The supplied 16 KiB ARM7
and 4 KiB ARM9 BIOS dumps differ from DraStic's replacement BIOS files. They
remain a useful later compatibility control, but are intentionally excluded
from the hybrid-MMIO retest so only one variable changes.

The hybrid retest succeeded. Mario Kart entered multiplayer without aborting;
Advance Wars reached its normal unsupported-wireless path. Both were later
exited through DraStic's menu with the port's expected status 137. Each trace reached the 16,384-event
limit, compared with the failing model's 389–390 reads. Of that captured prefix,
13,280 reads were nonzero. DraStic exposed repeated full-window packet-RAM test
patterns (`FFFF`, `5A5A`/`A5A5`, complementary ramps, and alternation) before
the later RF/baseband polling. The zero-filled detached model had hidden that
internally initialized storage from the guest and caused Mario Kart's abort.

No 16-bit store was observed in either successful trace, including during the
packet-RAM pattern changes. Those mutations therefore do not flow through the
intercepted store handler. The durable design must keep DraStic's packet RAM as
authoritative (or synchronize internal mutation through another boundary) and
override only explicitly implemented control registers. A global model-owned
read path is no longer viable.

The model is opt-in with `DRASTIC_WIFI_MODE=model`; packaged launchers select it
only when `drastic/wifi_model.enabled` exists. Without that marker, the validated
hooks still call DraStic's original handlers. Native regressions cover reset,
fixed registers, register mirrors, packet-RAM mirrors, baseband transfers, and
access accounting. Both the A30 and Brick shared objects cross-build with the
same model source.

The next implementation boundary is deliberately narrower than “add UDP”:

1. prove the A30 IRQ24 adapter and CPU wakeup on hardware, then recover and
   validate the equivalent AArch64 CPU boundary;
2. prove the A30 event/cycle hook on hardware and add beacon/countdown phases;
   then
3. add TX/RX queues and the session-scoped transport.

Opening a socket before those first three items would move packets without
giving DS software the interrupt and timing behavior needed to observe them.
