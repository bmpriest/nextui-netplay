# Superseded Gambatte sources

Nothing here is applied by any build rule. These files date from when the
Gambatte work lived in this repository as patches against upstream
`libretro/gambatte-libretro`. It now lives in the pinned fork
`bmpriest/gambatte-libretro`, which the Makefile clones at `GAMBATTE_REV`, and
every change below is present there — verified by locating each patch's
distinctive symbols in the pinned tree:

| File | Where it went |
| --- | --- |
| `gambatte-platforms.patch` | fork `Makefile.libretro` — tg5040/tg5050/my282 targets |
| `gambatte-network-hardening.patch` | fork `net_serial.{cpp,h}` — `TCP_NODELAY`, `MSG_NOSIGNAL`, complete reads/writes, deadlines, `reportStats` |
| `gambatte-local-instance.patch` | fork `libretro.cpp` — `SERIAL_LOCAL_SERVER`/`SERIAL_LOCAL_CLIENT`, `local_link` rendezvous |
| `gambatte-dual-wrapper.patch` | fork `libretro.cpp`, `Makefile.common`, `Makefile.libretro` — `NETPLAY_DUAL_INSTANCE`, `local_serial_bus`, `GBLC_MIRROR_INPUT` |
| `gambatte-dual-diagnostics.patch` | fork `libretro.cpp` — `dual_diag`, `GBLC perf`, `GBLC serial` |
| `local_serial.{h,cpp}` | fork `libgambatte/libretro/` — the tracked copies here predate `isIdle()`, which `retro_dual_is_checkpoint_safe()` needs |
| `test_local_serial.cpp` | fork `tests/dual_contract.c`, run by `make test-gambatte-dual` |
| `gambatte-serial-audio-overflow.patch` | fork `693068a`, merged as `533aab3` |

Only `gambatte-platforms.patch` still reverse-applies cleanly against the pinned
commit; the rest were subsumed by later fork commits and no longer match their
original context. They are kept because they are the readable form of what
changed and why, and because the fork's history compresses several of them into
single commits.

`gambatte-serial-audio-overflow.patch` is here for a different reason: it landed
upstream rather than being subsumed. It fixed the heap overflow that aborted the
host process the first time two paired consoles exchanged a serial byte
(`realloc(): invalid old size`), was reproduced under ASan, and is carried by
every core built from `GAMBATTE_REV` onwards. It is kept because the reasoning
and the on-device evidence are worth having next to each other, and the fork
commit message alone does not preserve the ASan trace.

## Also landed upstream, no patch kept here

`cf8b662` (merged `c161224`) — **zero `SaveState` before use.** gambatte declared
it on the stack and only default-initialised it, and `sachenOuterMask` /
`sachenLockCount` are written only by the Sachen mapper, so every other
cartridge serialized stack residue. Four bytes of a 119316-byte paired state,
different per call and per device, which made `saveState()` not a function of
emulator state and desynced instanced link at frame 0 on every session. See the
correction note in `docs/gbs/multi-instance.md`: the first diagnosis of this blamed
a deliberate mutation in `CPU::saveState` and was wrong.

Do not add to this directory. Gambatte changes belong in the fork.
