# RetroArch protocol peer: design

How the shim speaks RetroArch's netplay protocol, so a Netplay device plays
against a handheld running stock RetroArch netplay (spruce, Onion, Allium,
muOS, ROCKNIX, Knulli). The plan and the reasons for doing it are in
[retroarch-plan.md](retroarch-plan.md); this is what was built, the rules it
follows, and what is still open. Written 2026-11-04 from RetroArch's
`network/netplay/netplay_frontend.c` at `v1.22.2` and the 2026-11-02 spruce
captures. Function names below are RetroArch's, so each rule can be found at
its source.

## Shape

| Piece | Where | Does |
|---|---|---|
| Wire protocol | `shim/raproto.c`, `raproto.h` | Header, NICK, INFO, SYNC handshake in both roles; decodes commands into a remote-input ring and an ordered event queue; encodes INPUT, MODE, LOAD_SAVESTATE, CRC, RESET, STALL; PING; PAUSE/RESUME from frontend liveness |
| Frame loop | `shim/shim.c`, "RetroArch peer" section (`ra_tick`, `ra_step`) | Lockstep over RetroArch's frame numbers, state loads at their frame, CRC checks, pacing |
| Selection | session file `protocol=retroarch` | `ra_port=` (default 55435), `nick=`, `input_delay=`, and the usual `role=`/`peer=` |
| Tests | `shim/test/retroarch.sh`, `raproto_test.c`, `testing/retroarch/ra_model.py` | See [Testing](#testing) |

Netplay's own transport (`netlink.c`) is not started in a RetroArch session;
the shim's other modes are untouched.

**The shim stays lockstep.** RetroArch's rollback is private to each
RetroArch: on the wire it needs every player's input for every frame, in
order, and a peer that never predicts sends exactly that. The shim waits for
the RetroArch side's input like it waits for a Netplay peer's.

## Decisions

### Frame numbers are RetroArch's

A client adopts the host's frame counter (SYNC carries it; spruce's was 1050
in the capture). A Netplay host starts at 0 when its first RetroArch client
syncs, and does not run before one connects ("Waiting for a RetroArch
player..."), as a Netplay host waits for its guest.

### Every timed command sits at the receiver's next expected host input

A RetroArch client NAKs, and so disconnects, a MODE, LOAD_SAVESTATE or RESET
whose frame is not its `server_frame_count`, the next host INPUT frame it
expects (`netplay_get_cmd`). A lockstep host has already sent its input
`input_delay` frames ahead, so the next expected frame is in its future and it
has no state for it yet. So a Netplay host that owes a state (initial sync, a
`REQUEST_SAVESTATE`, a reset, a new client) stops deciding new input, runs the
frames it has already sent, and at `netplay_frame == next_send` serializes,
sends LOAD_SAVESTATE for that frame, loads the same bytes itself, and resumes.
That costs `input_delay` frames during which the RetroArch side predicts the
host's input; it then rewinds onto the state, which is what it does for its
own peers.

MODE for a client that asks to PLAY uses the same frame; SYNC does too.

A client gets the mirror image: LOAD_SAVESTATE and RESET for frame X arrive
before the host's INPUT for X, and X cannot run without that input, so an
event for a future frame is held and applied when the timeline reaches it.

### Both sides hold `unserialize(S)`

A Netplay host loads the state it sends ([state-sync.md](state-sync.md)
invariant 2). RetroArch's own host does not, which is why picodrive drifts on
RetroArch-vs-RetroArch; against a Netplay host the RetroArch client and the
host both end on the loaded bytes. When RetroArch hosts, nothing can make it
load its own state, so cores whose load changes live state (picodrive 2.04,
fceumm's MMC1 `lreset`) will mismatch until RetroArch's host replays; prefer
a Netplay host for those.

### Serialize the way RetroArch's netplay does

`core_serialize_special` makes `GET_SAVESTATE_CONTEXT` answer
`ROLLBACK_NETPLAY` and `GET_AUDIO_VIDEO_ENABLE` include `FAST_SAVESTATES`
while the core serializes. A core may write different bytes in that context,
so the shim answers both the same way around every serialize, unserialize and
size query of a RetroArch session (`ra_special_state`). The fake core records
the context in its state and the tests check it.

### State container and compression

Protocol 7 states travel in RetroArch's container: `NETPLAY\1`, a `MEM ` block
with the core's bytes, an `ACHV` block of 8 zero flag bytes, `END `. That is
byte-for-byte what spruce's RetroArch sends (checked against the capture by
`raproto_test`). Protocols 5 and 6 get the bare core state. Both are
zlib-compressed when the peer's header offers it (RetroArch's deflate stream,
which `compress2`/`uncompress` read and write).

### CRCs

The CRC of frame X is zlib CRC-32 over the core's serialized bytes before X
runs (`netplay_delta_frame_crc`, the `MEM` block only). A Netplay host sends
one every 300 frames once a client plays; RetroArch checks it against its own
state for X, replaying first if needed, and asks for a savestate on mismatch.
A Netplay client keeps its own CRC every 60 frames and for any frame the host
announced in advance, compares, and sends `REQUEST_SAVESTATE` on a mismatch.

This is one extra serialize per check on our side. RetroArch serializes every
frame anyway, so serialize counts already differ between the two sides; cores
whose `serialize` changes their state (state-sync.md assumption 3) are not
good RetroArch partners in either direction. snes9x and Supafaust are fine.

### Pacing

RetroArch keeps itself near its peers with three rules, and the shim fits
them:

- **Catch-up** (`netplay_sync_input_post_frame`): a RetroArch that sees its
  peer's input more than 3 frames ahead of its own runs unthrottled until it
  is level. A Netplay peer sends `input_delay` frames ahead, so RetroArch
  settles running about that far ahead of the Netplay device: it then holds
  our input before it needs it (no rollback) and we hold its input before we
  need it (no stall).
- **STALL**: a RetroArch host tells a client more than 3 frames ahead of it
  to wait. A Netplay host does the same, at most every 120 frames; a Netplay
  client honours one by holding frames.
- **Running ahead**: nothing stops a RetroArch that is faster than its peer
  except its 60-frame limit. So a Netplay client whose host's input runs more
  than `input_delay + 4` frames ahead for 30 ticks runs one extra frame per
  tick, video and audio withheld, until it is within `input_delay + 1`.

A Netplay host advertises input latency 0..0: RetroArch then applies its own
player's input immediately, and the Netplay player carries the delay.

### Pause and menus

The worker thread sends PAUSE when `retro_run` has not been called for 250ms
(MinArch's menu, sleep) and RESUME when it comes back, as netlink does with
its own pause. A Netplay host always may; a Netplay client only when the host
sent `SETTING_ALLOW_PAUSING 1`. spruce's RetroArch sends 0, and a RetroArch
host NAKs a client's PAUSE then, so a Netplay client in its menu sends nothing
and RetroArch stalls, then hangs up after 5 s
(`MAX_SERVER_STALL_TIME_USEC`). Feeding neutral input from the worker thread
during the menu would fix that and is the next pacing item.

A peer's PAUSE holds our frames with "Other player paused...".

### Joining and leaving

One RetroArch client: a second connection gets the 4-byte `FULL` answer
RetroArch's own full host sends. A client that leaves stops counting as a
player at the last frame its input reached; the host plays on and syncs the
next client to arrive from its current frame. A Netplay client whose host goes
away gets the usual failure overlay. A `POKE` probe is answered with a header
and closed, as RetroArch does, which gives a cheap way to detect a RetroArch
host (below).

Ports: the RetroArch host is port 1, its client port 2, as RetroArch assigns
them (first free device) and as Netplay already does.

## Not done yet

| Gap | Note |
|---|---|
| Discovery | answer `RANQ` (UDP 55435) from the broker so a Netplay host appears in RetroArch's list; send `RANQ` in Join to list RetroArch hosts |
| Protocol choice in the app | Host offers Netplay.pak or RetroArch; Join detects (below) |
| Nicknames in the app | the shim already reads `nick=` (below) |
| Link cores | `NETPACKET` for gambatte and gpSP; currently refused with a warning |
| Passwords | a host that sets one is refused with a log line; the hash is `sha256("%08X" salt + password)` in hex |
| Analog and other devices | only joypad bits cross; analog words are sent as zero |
| Spectators, more than two players | not planned for the first version |
| Menu on a client of a no-pause host | RetroArch hangs up after 5 s; see Pause above |
| Protocol 8 | RetroArch master only |

## Protocol choice, detection and names

**Choice at host time.** The Host screen gets a protocol choice: Netplay.pak
(today's) or RetroArch. The broker's announcement carries it, and in RetroArch
mode the broker also answers `RANQ`, so stock RetroArch devices list the host
too. The armed session records `protocol=retroarch` for the shim.

**Detection at join time.** The joiner should not have to know:

1. A host found by Netplay's announcement says which protocol it runs.
2. A host found by `RANQ` is a RetroArch host (stock, or a Netplay host in
   RetroArch mode; the `RANS` reply's frontend string tells which).
3. A typed address is probed: Netplay's broker answers on TCP 55439; a
   RetroArch host answers a `POKE` on 55435 with its header and closes,
   without taking a player slot. Whichever answers decides.

The joiner then writes the matching `protocol=` into its own session.

**A name for Netplay.pak's protocol.** "Netplay" is RetroArch's name for its
own protocol too, so the UI and docs need a distinct one. Options:

| Name | For | Against |
|---|---|---|
| **Paklink** | short, says where it comes from, matches `netlink.c`'s role | invented word |
| Lockstep | describes the difference from RetroArch's rollback | RetroArch master's protocol 8 adds a "lockstep" host mode |
| Netplay.pak protocol | unambiguous | long for a menu |

Recommendation: **Paklink** on screen ("Protocol: Paklink / RetroArch") and in
new docs; the code keeps `netlink`. Not applied anywhere yet: it is the
user's call.

**Nicknames.** RetroArch shows a nick for every player (`NICK`, `MODE`, the
lobby). Netplay should have one setting, Configuration > Nickname, defaulting
to the device's model name (a short probe, since an H700 is about twelve
devices), written into the session as `nick=`. The shim already sends it as
the RetroArch nick. The same name belongs in Netplay's own announcements and
Join list, so two Bricks on one network can be told apart.

## spruce launches

feat/spruce appends `launcher/spruce/retroarch-session.cfg` to spruce's
RetroArch for armed launches (save states, rewind, run-ahead and fast/slow
motion off). The RetroArch protocol does not need RetroArch's savestate path
on the Netplay side: when Netplay's spruce build is the RetroArch-protocol
peer it is the shim inside RetroArch that speaks the protocol, and the shim
serializes through the core directly. A spruce device using *stock* RetroArch
netplay is not armed by Netplay at all.

## Testing

`shim/test/retroarch.sh` (host-only, needs python3):

- **`raproto_test`**: implementation magic for 1.22.2 (`0x455`), platform
  magic, input sizes, and the container, which is compared byte for byte with
  the first state spruce sent in the capture when `testing/results` is
  present.
- **`ra_model.py`** plays RetroArch in either role against harness + shim +
  fake core. It enforces RetroArch's NAK and hang-up rules (sizes, order,
  frame of MODE / LOAD_SAVESTATE, inputs in order, no container below
  protocol 7), simulates the fake core over confirmed frames, and exchanges
  CRCs with the shim. The fake core folds both ports' input into its state
  (`FAKE_STATE_INPUTS`), so an agreeing CRC means the same inputs ran on the
  same ports on every frame since the same loaded state.
- Cases: shim joins at protocol 7 and 5, shim hosts at 7 (with a mid-session
  savestate request) and 6, and a forced desync on each side, which must
  produce one mismatch, one state, and agreement afterwards.

The model does not predict, so it does not reproduce RetroArch's rollback,
its real catch-up timing, or its behaviour with a peer that holds input back
during a state drain. Those need a real RetroArch: the device tests below, or
a desktop RetroArch 1.22.2 when one is available.

## Device tests to run

See the feat/retroarch report for exact commands. In order: spruce RetroArch
hosts snes9x or Supafaust and a NextUI device joins; the NextUI device hosts
and spruce joins; then a menu open on each side, a host reset, and a forced
desync with `FAKE`-free real content (desync shows as a `CRC mismatch` line
and one state).
