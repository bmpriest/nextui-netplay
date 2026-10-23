# DraStic Wi-Fi hook

## Status (2026-10-23)

The shipped library is now the **radio**: DSperate's port of melonDS's Wi-Fi
block and LAN transport (`src/radio/`), attached to DraStic 2.5.2.0 on AArch64
by `src/radio_hook.c`. It speaks melonDS's LAN protocol, so DraStic plays DS
local wireless against DraStic, DSperate or melonDS. Mario Kart DS races in
both roles. See `docs/drastic.md` for the design, measurements and how Netplay
installs it (an overlay on the user's NDS.pak).

    make drastic-radio   build/../build-brick/libdrastic_wifi.so (pinned tg5040 toolchain)
    make cores-nds       the NDS.pak overlay for tg5040 and tg5050

`DRASTIC_WIFI_LAN=host|join:<IPv4>` turns it on (the overlay's launch.sh sets
it from the Netplay session); without it nothing is hooked.

drastic-wifi is GPL-3.0-or-later (LICENSE), as the code it carries is.

Everything below describes the earlier hand-written model, which still builds
as `libdrastic_wifi_diag.so` for its MMIO tracing. Its hardware findings (the
offsets, the deferred install, the packet RAM location, IRQ injection) are what
the radio hook is built on.

## Earlier status (2026-08-31)

The Brick implementation has advanced beyond the early hook milestone described
below. Two Advanced DraStic instances can discover, authenticate, associate,
and hold an Asphalt Urban GT 2 lobby for more than two minutes. Pressing the
host's required `A` action to start still leaves player 2 on the host but sends
the client to `COMMUNICATION ERROR`. Current work is focused on MP command
completion and IRQ-visible state across that transition.

The deployed diagnostic library is `libdrastic_wifi_diag.so` on both Bricks.
DevBridge assets in `devbridge/` provide the repeatable workflow:

- `asphalt-cold-join.test` launches and joins from a cold state;
- `asphalt-observe.test` and `asphalt-observe-start.test` capture later states;
- `asphalt-safe-quit.test` exits through Advanced DraStic and waits for
  DevBridge to return;
- `asphalt-screen-baselines.json`, `classify-screen.py`, and
  `classify-asphalt-session.sh` distinguish no-room, room-visible, connected,
  dropped-player, and communication-error states.

Do not use DevBridge's generic `QUIT_GAME` or an immediate `kill -9` for routine
tests. The safe Advanced DraStic sequence is `MENU`, wait 500 ms, `UP`, `UP`,
`A`, then allow up to 45 seconds for the wrapper to restore DevBridge. Testing
is currently paused with both devices returned to DevBridge.

This directory contains the first executable multiplayer milestone for DraStic
on the A30 and TrimUI Brick. `libdrastic_wifi.so` replaces the two 16-bit
Wi-Fi MMIO handlers with validated ARM trampolines. Passthrough mode records the
access and calls the original fake handler. Opt-in model mode currently uses a
hybrid boundary: reads and writes remain visible to DraStic while accesses are
also shadowed into the independent model described below.

The runtime hook is disabled unless `DRASTIC_WIFI_TRACE` names an output file:

```sh
export LD_PRELOAD="$PWD/libdrastic_wifi.so"
export DRASTIC_WIFI_TRACE=/tmp/drastic-wifi.log
./drastic game.nds
```

That default remains observation-only. An opt-in hardware-model boundary is
available with:

```sh
export DRASTIC_WIFI_MODE=model
```

The A30 event/cycle hook is independently opt-in with
`DRASTIC_WIFI_CYCLE=1`; the packaged launcher selects it only when both
`wifi_model.enabled` and `wifi_cycle.enabled` exist. This isolation was added
after Mario Kart aborted in the multiplayer menu before any modeled timer or
IRQ register write occurred.

The packaged launchers select it when `drastic/wifi_model.enabled` exists. The
portable model implements an 8 KiB packet RAM and mirror, a Wi-Fi register file,
and the baseband access port, but it does not yet replace DraStic's visible read
results. Hardware testing proved that DraStic initializes its packet RAM through
an internal path which the 16-bit store hook cannot observe; globally replacing
reads with a detached zero-filled copy caused Mario Kart to abort. Selective
register ownership will be enabled only after the relevant state is synchronized.
The model implements `W_IF`, `W_IE`, and `W_IFSet` semantics and exposes a
rising-edge callback. On the A30, the callback uses the
validated 2.5.0.4 memory, system, ARM7 CPU, and I/O layouts to latch IRQ24 and
refresh DraStic's active-interrupt mask. This still needs an on-device wakeup
test. The portable model also accepts deterministic elapsed emulated time and
implements the 8 us hardware tick, 64-bit `W_USCount`/`W_USCompare` registers,
and compare IRQ14. On A30, a third validated hook wraps `execute_events`, reads
the exact `system.global_cycles` delta, and converts the 67,027,964 Hz master
cycle domain to microseconds while retaining the fractional remainder. It runs
after native event dispatch and immediately before DraStic's normal CPU
interrupt checkpoint. On the Brick, the exact-build event hook at PIE offset
`0x216e0` uses the same cycle fields, and the IRQ adapter uses the event-supplied
system pointer, embedded ARM7 offset `0x25cd340`, CPU interrupt/state offsets
`0x2108`/`0x2110`, and ARM7 I/O at memory offset `0x23070`. Hardware tests
validated model shadowing, timing, adapter initialization, and IRQ24 injection
independently. The
model does not synthesize radio frames or open a network transport. Keeping it
opt-in makes the ownership boundary testable on hardware without changing
ordinary single-player launches.

The A30 build supports the exact fixed-address ARMv7 DraStic 2.5.0.4 executable.
The Brick build supports the AArch64 PIE DraStic 2.5.2.0 executable with build
ID `5665dbdad6bec617b3f828afb2f69b0a14460764` (the `59a7` release shipped by
MinUI Nintendo DS Pak). The library verifies every overwritten instruction
before patching. It
does not hook the adjacent 8-bit and 32-bit stubs because an eight-byte absolute
jump would overwrite their neighboring functions.

Brick reads use the specialized `load_wifi_16` equivalent at offset `0x13660`.
Writes instead hook the ARM7 16-bit I/O dispatcher at `0x128d0`, which inlines
its Wi-Fi fallback at `0x131a8`. The standalone raw store at `0x138f0` is not
reached by the guest path observed in Mario Kart.

On the Brick, Advanced DraStic installs its own executable detours during early
`main`. The library therefore validates the target prologues in its constructor
but defers writing the Wi-Fi jumps until three seconds after `main` begins. An
Asphalt GT2 hardware test proved this ordering runs normally and reaches
`status=ready-deferred`; constructor-time installation crashed before the first
hooked call. The A30 installation remains immediate.

The trace is capped at 16,384 records to protect low-powered devices and SD
cards. Trace records still contain a diagnostic host monotonic timestamp;
hardware-model timing remains independent of it.

An A30 model-mode trace should contain both of these lines:

```text
drastic-wifi event-cycle-hook=ready hz=67027964
drastic-wifi arm7-irq-adapter=ready irq=24
```

On a normal exit, the summary's `event_calls`, `arm9_cycles`, and `wifi_us`
values should all be nonzero. Passthrough mode does not install the event hook.

The constructor accepts only a direct `drastic` command or the packaged
`ld-linux-armhf.so.3 ... ./drastic` form. It then removes `LD_PRELOAD` and the
hook variables from DraStic's environment. This prevents UI-spawned helpers
from reopening the trace or applying fixed-address patches to themselves.

The A30 pak also includes `hard-quit.sh`. Run it over SSH if DraStic's own Exit
menu wedges; it validates the launcher's PID, requests the ordinary TERM-based
cleanup, and escalates to SIGKILL after six seconds.
