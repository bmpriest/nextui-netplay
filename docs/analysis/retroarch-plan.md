# Plan: RetroArch compatibility

Let a Netplay.pak device play against RetroArch: a PC, a phone, or another
handheld running RetroArch, on the same LAN. Work happens on branch
`feat/retroarch` (worktree `.worktrees/netplay-retroarch`).

Written 2026-11-02 from RetroArch's netplay source at `master` (`54d27c5`,
`network/netplay/`: `README`, `netplay_private.h`, `netplay_protocol.h`,
`netplay_frontend.c`). Everything marked **verify** is read from source, not
yet tested against a running RetroArch.

## What RetroArch netplay is

| | RetroArch | Netplay.pak |
|---|---|---|
| Model | Rollback: each frame it saves state, predicts the remote input, and on a wrong guess rewinds and replays | Lockstep with input delay: a frame waits for the peer's input; no prediction, no rewind |
| Authority | The server's clock arbitrates joins, parts and state loads | The host's state is authoritative |
| Players | Up to 32 clients, plus spectators | Two |
| Transport | TCP, port 55435; LAN discovery on UDP 55435; MITM relays for the internet | TCP 55437; announcements on UDP 55438 |
| Identity | Content CRC32 (of the file, or of the file inside an archive), core name and core version strings, which must match | Content SHA-256, core identity, state and save geometry |
| Desync | `CRC` of a frame; the receiver requests a savestate on mismatch | State hash every 300 frames; authoritative resync |
| Versions | Protocols 5 to 8 accepted (`LOW_`/`HIGH_NETPLAY_PROTOCOL_VERSION`) | Protocol 15, its own |
| Link cores | `NETPLAY_CMD_NETPACKET` carries netpacket traffic | `netlink.c` carries it |

The wire format is simple: every command is a 32-bit ID and a 32-bit payload
length, network byte order, then the payload.

**Handshake** (both sides): a connection header (`RANP` magic, a platform magic,
compression flags, a password salt or the highest supported protocol, and an
implementation magic hashed from RetroArch's version string), then `NICK`. The
server then expects `PASSWORD` if set (SHA-256), sends `INFO` (content CRC,
core name, core version), receives the client's `INFO`, and sends `SYNC` (frame
number, client number, controller devices, share modes, controller mapping,
nick, and the server's SRAM).

**Play**: `INPUT` (frame, client, input words) for every frame from every
player; `NOINPUT` from a server with no controller; `CRC`,
`REQUEST_SAVESTATE` and `LOAD_SAVESTATE` (zlib-compressed when both sides
support it) for desyncs; `PAUSE`, `RESUME`, `STALL` and `RESET`;
`SETTING_INPUT_LATENCY_FRAMES` and ping commands.

Protocol 8 is recent and relevant: its `LOAD_SAVESTATE` may target a frame the
client already has input for, "as a lockstep host sends while it waits on that
client", and its CRC can be taken over system RAM
(`netplay_lockstep_take_crc`). RetroArch itself now has a lockstep host mode.

## Why it can work

RetroArch's rollback is local to each RetroArch. On the wire it only needs
every player's input for every frame, in order. A peer that never runs ahead,
and simply waits for input before advancing, sends exactly that stream; the
RetroArch side predicts, rewinds and absorbs the difference. So Netplay does not
have to adopt rollback to interoperate. Its input delay maps onto RetroArch's
input latency frames, and its authoritative state transfer maps onto
`SYNC` and `LOAD_SAVESTATE`. **Verify** that RetroArch tolerates a peer whose
input lags by the full delay without stalling it into a crawl; protocol 8's
lockstep host suggests it does.

## What makes it hard

1. **Determinism across architectures.** A PC is x86-64; the handhelds are
   AArch64 and ARMv7. Lockstep needs both to compute the same frames
   ([determinism.md](determinism.md)). Encouraging: fceumm is pure C,
   picodrive's portable path matched across ARM architectures, Supafaust's state
   is explicitly encoded. Not expected to work: pcsx_rearmed. Each core has to be
   proven with `statecheck` on an x86-64 build of the same revision.
2. **Matching core versions.** `INFO` compares core name and version strings
   exactly, and those carry git hashes. The player's RetroArch needs the same
   revision Netplay runs: a compatibility core for which Netplay publishes
   desktop builds, or which is pinned to a RetroArch buildbot revision.
3. **Content identity.** RetroArch sends a CRC32 of the content (of the inner
   file for archives). Netplay hashes with SHA-256; it needs a CRC32 computed
   the same way, including RetroArch's archive handling.
4. **The implementation magic** is a hash of RetroArch's version string.
   **Verify** whether a mismatch refuses the connection or only warns, and so
   which RetroArch versions Netplay must claim to be.
5. **The CRC it checks.** Netplay must produce RetroArch's `CRC` values (over
   serialized state, or over system RAM in protocol 8's lockstep path), or it
   will request savestates constantly.
6. **Platform magic.** An A30 is 32-bit (`sizeof(size_t)` 4), a PC 64-bit.
   RetroArch's code only treats an endianness mismatch as fatal; **verify** that
   nothing else keys on it.
7. **Rollback cost is RetroArch's, not ours.** A RetroArch peer on a handheld
   serializes every frame. Heavy cores may not keep up there; on a PC it is
   free.

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

Rejected alternatives: running RetroArch itself on the device (NextUI uses
MinArch; Netplay would become a different product), and a PC-side gateway (a
second program for the player to run).

Out of scope at first: the internet lobby and MITM relays, spectators, more
than two players, passwords beyond what `PASSWORD` needs to join a protected
host.

## Phases

1. **Pin and read.** Choose the RetroArch release to target (not `master`) and
   record its protocol version and implementation magic. Capture a two-RetroArch
   session on a PC with Wireshark as the reference.
2. **Determinism on x86-64.** Build fceumm and picodrive at Netplay's pinned
   revisions for x86-64 and run `statecheck` against the ARM builds. Stop here
   for any core that diverges.
3. **Desktop harness.** RetroArch on a PC against the shim built natively
   (`make native`) with the same core build: handshake, `INFO`, `SYNC`.
4. **Play.** `INPUT` lockstep with Netplay as client, then as host; measure
   whether RetroArch stalls or rewinds excessively.
5. **Resync.** `CRC` and savestates in both directions; a forced desync recovers.
6. **A handheld against a PC** over house WiFi, then over a Netplay ad hoc
   network joined by a laptop.
7. **Discovery.** Answer RetroArch's LAN query (`RANQ`/`RANS` on UDP 55435) so a
   Netplay host appears in RetroArch's list, and list RetroArch hosts in Join.
8. **Link cores.** Gambatte and gpSP over `NETPACKET`.
9. **The app.** Mark RetroArch peers in Join, explain a version or core
   mismatch in words, and publish the desktop core builds a player needs.

## Open questions

- Which RetroArch versions do players actually run, and how far back should
  protocols 5-7 be supported?
- Is a RetroArch peer on the *same* kind of handheld useful enough to test
  first, since it removes the x86 determinism question?
