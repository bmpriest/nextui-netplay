# Plan: RetroArch netplay compatibility

Let a Netplay.pak device play against handhelds running the other common Linux
firmwares: spruce, Onion, Allium, muOS, ROCKNIX, Knulli. Those use RetroArch for
emulation, and their netplay is RetroArch's own protocol, with setup and
teardown scripted around it by each firmware. A Windows PC running RetroArch
would be a bonus, not a goal. Work happens on branch `feat/retroarch`
(worktree `.worktrees/netplay-retroarch`).

Written 2026-11-02 from RetroArch's netplay source (tag `v1.22.2` and `master`
at `54d27c5`) and the spruceOS tree at `0c410ba` (2026-10-25). Items marked
**verify** are read from source and not yet tested against a running
RetroArch.

## First target: spruce on an RG35XXH

spruce ships one RetroArch, **1.22.2**, for every device it supports (`ra64.h700`,
`ra64.universal`, `ra32.a30`, `ra32.mini`), with its cores in
`RetroArch/.retroarch/cores64/` and `cores/`. The local checkout has no netplay
scripts of its own: a player uses RetroArch's netplay menu. Starting there makes
the first test as easy as it can be:

- **Same device, same architecture.** An RG35XXH on spruce against an RG35XXH
  (or any AArch64 device) on NextUI removes the cross-architecture determinism
  question entirely.
- **One RetroArch version.** spruce pins 1.22.2 everywhere, so there is one
  protocol to match.
- **DevBridge on spruce** (in progress) makes the spruce side scriptable like
  every other device in the rig.

Other firmwares come after, one at a time, each by inventory first: which
RetroArch version, which core revisions, and what its netplay setup script
does around RetroArch (ad hoc network, discovery, config).

## What RetroArch netplay is

| | RetroArch 1.22.2 | Netplay.pak |
|---|---|---|
| Model | Rollback: each frame it saves state, predicts the remote input, and on a wrong guess rewinds and replays | Lockstep with input delay: a frame waits for the peer's input |
| Authority | The server's clock arbitrates joins, parts and state loads | The host's state is authoritative |
| Players | Up to 32 clients, plus spectators | Two |
| Transport | TCP 55435; LAN discovery UDP 55435 (`RANQ` query, `RANS` reply); MITM relays for the internet | TCP 55437; announcements UDP 55438 |
| Identity | Content CRC32, core name and core version strings, which must match | Content SHA-256, core identity, state and save geometry |
| Desync | `CRC` of a frame; the receiver requests a savestate | State hash every 300 frames; authoritative resync |
| Versions | Protocols **5 to 7** (1.22.2); `master` has moved on to 8 | Protocol 15, its own |
| Link cores | `NETPLAY_CMD_NETPACKET` | `netlink.c` |

Every command is a 32-bit ID and a 32-bit payload length in network byte order,
then the payload.

**Handshake** (both sides): a connection header (`RANP` magic, a platform
magic, compression flags, a password salt or the highest protocol, and an
implementation magic hashed from RetroArch's version string), then `NICK`. The
server takes `PASSWORD` if set (SHA-256), sends `INFO` (content CRC, core name,
core version), receives the client's `INFO`, and sends `SYNC` (frame, client
number, controller devices, share modes, controller mapping, nick, SRAM).

**Play**: `INPUT` (frame, client, input words) for every frame from every
player; `NOINPUT` from a server with no controller; `CRC`,
`REQUEST_SAVESTATE` and `LOAD_SAVESTATE` (zlib when both support it); `PAUSE`,
`RESUME`, `STALL`, `RESET`; input latency frames and ping.

`master` adds protocol 8 with a lockstep host mode and a CRC over system RAM.
No firmware ships it yet; target 1.22.x's protocol 7 and treat 8 as later.

## Why a lockstep peer can work

RetroArch's rollback is local to each RetroArch. On the wire it only needs
every player's input for every frame, in order. A peer that never runs ahead
sends exactly that; the RetroArch side predicts, rewinds and absorbs the
difference. So Netplay does not need rollback to interoperate: its input delay
maps onto RetroArch's input latency frames, and its authoritative state maps
onto `SYNC` and `LOAD_SAVESTATE`. **Verify** that RetroArch 1.22.2 tolerates a
peer whose input always arrives at the full delay without stalling itself into
a crawl, and what it does when its rollback window fills.

## The real obstacle: matching cores

`INFO` compares core name and version strings exactly, and both sides must run
the same core revision for lockstep to hold. spruce's cores do not match
Netplay's anywhere:

| Core | spruce 1.22.2 (`cores64/`) | Netplay |
|---|---|---|
| fceumm | `(SVN) 3544ff5` | compatibility `3544ff5` (pinned to spruce) |
| picodrive | `2.04-7102a71` | NextUI `b0be121`; compatibility `7102a71` (pinned to spruce) |
| snes9x | `1.63 49f48456` | NextUI's floats; fallback Supafaust |
| mednafen_supafaust | `1.29.0 e25f667` | compatibility `e25f667` (pinned to spruce) |
| gambatte | `v0.5.0 a85fe7c` | the bmpriest fork `b3803b3` |
| gpsp | `v1.1.0-5819380` | `v1.1.0-netplay-5819380` (same base, RFU patches) |

Each firmware will have its own set. The way through is the compatibility-core
mechanism Netplay already has, keyed by peer: **peer-matched cores**, built at
the revisions a firmware ships, reporting the same version strings, loaded for a
session against that firmware. Netplay-Core downloads them like the other
compatibility cores, per firmware and RetroArch version.

The first experiment is cheaper still: on AArch64, spruce's own `cores64`
binaries may run under NextUI's MinArch and the shim as they are (both are
libretro cores; **verify** their library dependencies). That proves the protocol
work before any core is rebuilt.

## Other differences to handle

1. **Content identity.** RetroArch's content CRC is the CRC32 of the whole
   content file, or of the member inside an archive (taken from the archive's
   header). For a disc that is the CRC of whatever file was opened (the `.cue`,
   `.chd` or `.m3u`), not of the disc data. Netplay must compute the same thing,
   from the same file, in addition to its own SHA-256 (see below).
2. **The implementation magic** hashes RetroArch's version string. **Verify**
   whether a mismatch refuses or only warns; it decides whether Netplay must
   claim to be 1.22.2.
3. **The `CRC` command.** Netplay must produce RetroArch's values for a frame
   (1.22.2: over the serialized state; **verify**), or each check triggers a
   savestate request.
4. **Platform magic.** The A30 and Miyoo Mini are 32-bit; RetroArch's code only
   treats an endianness mismatch as fatal. **Verify** nothing else keys on it.
5. **Each firmware's setup.** Some firmwares put their own ad hoc network or
   discovery around RetroArch. Interoperating may mean speaking that too, or at
   least joining the network it makes. Inventory per firmware.
6. **Rollback cost lands on the other device.** A RetroArch peer on a handheld
   serializes every frame and replays on mispredictions. Light cores are fine
   on A53s; heavy ones (PlayStation, Supafaust) may not keep up there.

## Approach

**An alternate wire protocol inside the shim.** The shim already owns input
lockstep, state transfer, resync and the netpacket bridge; RetroArch's protocol
is another encoding of the same events. A `ra_proto.c` beside `netlink.c`,
selected when the peer is RetroArch, translates:

| Netplay event | RetroArch command |
|---|---|
| connect, identity | header, `NICK`, `INFO` |
| initial state | `SYNC` with SRAM, then `LOAD_SAVESTATE` |
| per-frame input | `INPUT` |
| periodic hash | `CRC` |
| resync | `REQUEST_SAVESTATE` / `LOAD_SAVESTATE` |
| peer in menu | `PAUSE` / `RESUME` |
| host reset | `RESET` |
| link packets | `NETPACKET` |

Out of scope at first: MITM relays and the internet lobby, spectators, more
than two players.

## Phases

1. **DevBridge on spruce** (in progress): the RG35XXH on spruce scriptable from
   the rig.
2. **Reference capture.** Two spruce devices (or spruce and a PC on 1.22.2)
   playing fceumm; record the session with `tcpdump` as the byte-level
   reference, including spruce's setup around it.
3. **spruce's core under NextUI.** Run spruce's `fceumm_libretro.so` through
   the shim on an AArch64 NextUI device; confirm it loads and that `statecheck`
   matches the same core on spruce.
4. **Handshake.** The shim, built natively first (`make native`), completes
   header, `NICK`, `INFO` and `SYNC` against spruce's RetroArch.
5. **Play.** `INPUT` lockstep with Netplay as client, then as host; measure
   whether RetroArch stalls or rewinds excessively.
6. **Resync.** `CRC` and savestates both ways; a forced desync recovers.
7. **Discovery and setup.** Answer `RANQ` so a Netplay host appears in
   RetroArch's list; list RetroArch hosts in Join; follow spruce's network setup.
8. **Link cores.** Gambatte and gpSP over `NETPACKET`.
9. **Peer-matched cores** for spruce's set, then the next firmware.
10. **The app.** Mark RetroArch peers in Join, name the firmware and core a
    mismatch needs, and download the matching core.

## Questions this answers

**Could Netplay do rollback itself?** Not needed for RetroArch compatibility
(above), and not a MinArch rebuild either: the shim owns `retro_run`, input and
the video and audio callbacks, so it can save state each frame, run ahead on
predicted input, and on a wrong guess load an older state and replay several
frames inside one frontend frame with video and audio suppressed. Two costs
decide whether it is worth it. **CPU**: a serialize every frame plus replays, on
A53 cores that already run PlayStation and the paired cores near their limit.
**State round trips**: rollback needs `unserialize(serialize(x))` to be exactly
`x`, and [state-sync.md](state-sync.md) found that is false for picodrive (649
bytes of 678,519 differ) and that `serialize` itself mutates state. Lockstep
survives that because both devices do the same thing in the same order;
rollback would only replay on the device that guessed wrong, and the two would
diverge. It could be offered per core, for cores whose round trip is exact
(Supafaust's is), as a later feature.

**CRC32 beside SHA-256, or CRC32 instead?** Beside. The shim already computes
the CRC32 of the same bytes its SHA-256 covers (for finding the peer's
cartridge, where a zip's header supplies it for free), and hashing a ROM once at
launch costs milliseconds either way. SHA-256 stays Netplay's identity because
it is also what pins downloads and the DraStic build, where a CRC32 can be
forged. RetroArch's CRC32 is a different value anyway (whole file or archive
member, and the `.cue` rather than the disc), so it is computed for RetroArch
peers only, exactly as RetroArch computes it.
