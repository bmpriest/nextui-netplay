# DS Wi-Fi multiplayer transfers, as observed on hardware

Source: notes from benja, from direct hardware observation. This is more precise
than akkit or GBATEK on several points and contradicts them on others; where the
two disagree, this document wins. Items the author flagged as uncertain are
marked as such rather than smoothed over.

This is the behaviour both DS radios Netplay ships must reproduce: DSperate's
(melonDS's Wi-Fi block, [dsperate.md](dsperate.md)) and the same block hooked
into DraStic ([drastic.md](drastic.md)). It began as the specification for a
hand-written model of the hardware, which reached a lobby and no further; the
port that replaced it already behaves this way, so these notes are now the
reference for debugging it, not a to-do list.

## Register offsets

Taken from melonDS's register map and cross-checked against traces from the
Brick. The ones the Brick work had derived empirically — `W_TXBusy`, `W_TXStat`,
`W_TXReqRead`, `W_TXSlotBeacon`, `W_TXSlotCmd`, `W_USCountCnt`, `W_USCompareCnt`
and `W_USCompare0` — all agree. Note that `0x0134` is `W_BeaconCount2`, not
`W_BeaconCount1`; akkit labels it "W_BEACONCOUNT", which has misled this work
once already.

| Offset | Register |
| --- | --- |
| `0x004` | `W_ModeReset` |
| `0x006` | `W_ModeWEP` |
| `0x008` | `W_TXStatCnt` |
| `0x018` | `W_MACAddr0` |
| `0x020` | `W_BSSID0` |
| `0x028` | `W_AIDLow` |
| `0x02a` | `W_AIDFull` |
| `0x034` | `W_TRXPower` |
| `0x038` | `W_PowerTX` |
| `0x03c` | `W_PowerState` |
| `0x040` | `W_PowerForce` |
| `0x048` | `W_PowerDownCtrl` |
| `0x080` | `W_TXSlotBeacon` |
| `0x088` | `W_ListenCount` |
| `0x08c` | `W_BeaconInterval` |
| `0x08e` | `W_ListenInterval` |
| `0x090` | `W_TXSlotCmd` |
| `0x094` | `W_TXSlotReply1` |
| `0x098` | `W_TXSlotReply2` |
| `0x0b0` | `W_TXReqRead` |
| `0x0b6` | `W_TXBusy` |
| `0x0b8` | `W_TXStat` |
| `0x0c4` | `W_CmdReplyTime` |
| `0x0e8` | `W_USCountCnt` |
| `0x0ea` | `W_USCompareCnt` |
| `0x0f0` | `W_USCompare0` |
| `0x118` | `W_CmdCount` |
| `0x11c` | `W_BeaconCount1` |
| `0x134` | `W_BeaconCount2` |
| `0x19c` | `W_RFPins` |
| `0x214` | `W_RFStatus` |
| `0x268` | `W_RXTXAddr` |

## Beacon interrupt gating

Setting bit 0 of the value written to `W_US_COMPARE` blocks the
`W_BEACONCOUNT` interrupt (IRQ14) until `W_US_COUNT` matches `W_US_COMPARE`. The
bit is write-only and is not part of the compare value, whose low ten bits are
masked off.

The interrupt has three sources — the microsecond compare, the beacon counter,
and a forced write of bit 1 to `W_USCOMPARECNT` — and the block applies only to
the beacon-counter source. Every source is additionally gated on
`W_USCOMPARECNT` bit 0. Taking the interrupt reloads `W_BeaconCount1` from
`W_BeaconInterval` unless it was forced, arms `W_BeaconCount2` at `0xffff`,
clears the transmit request bits with `& 0xfff2`, transmits the beacon if the
beacon slot is enabled, and advances the listen counter.

## W_CMD_COUNT

Defines the time interval during which `W_TXBUF_CMD` transfers are possible.
The CMD slot retries the transfer automatically on error — for example when MP
clients do not respond — but aborts if there is not enough time left in
`W_CMD_COUNT`.

`W_TXBUF_CMD` itself starts its transfer as soon as possible.

## Host side: the CMD transfer process

Sending a `W_TXBUF_CMD` frame starts the following process.

1. The MP host sends the CMD frame, as soon as possible. After the preamble,
   IRQ7 is triggered.
2. Once the transfer is finished: if bit 14 of `W_TXSTATCNT` is set, `W_TXSTAT`
   is set to `0x0800` and IRQ1 is triggered.
3. The hardware waits for the MP clients' replies. The duration is
   `16 + ((10 + W_CMD_REPLYTIME) * popcount(client_mask_from_frame_body))`.
4. The MP host sends the CMD ack. After the preamble, IRQ7 is triggered again —
   this is why a CMD transfer produces two IRQ7s.
5. During the ack transfer, `W_RFSTATUS` is 8 and `W_RXTXADDR` is `0x0FC0`.
6. Once that transfer is finished: if bit 13 of `W_TXSTATCNT` is set, `W_TXSTAT`
   is set to `0x0B01` and IRQ1 is triggered.
7. The TX header of the CMD frame is adjusted: bits in `TXheader[02]` are
   cleared to indicate that the corresponding clients responded successfully.
   Nintendo software checks this.

The CMD ack is sent automatically; it is the `03:09:BF:00:00:03` flow packet.
MP clients receive it as a regular frame.

IRQ12 signals the end of the CMD transfer process: either when the final ack
finishes transferring, if everything succeeded, or otherwise at the end of
`W_CMD_COUNT`.

Retries are not well understood yet. They appear to repeat the entire process.

## Client side: the reply transfer

Software typically sets `W_TXBUF_REPLY1` and that is all; the client has little
control over the MP transfer process. No way has been found to transfer a reply
frame manually.

Reply slots are latches, not interchangeable pointers. `W_RXCnt` bit 7 copies
Reply1 into Reply2 and clears Reply1. Reception of a later command latches the
newly staged Reply1 value and transmits from Reply2. Implementations that always
read Reply1 can appear to work for empty replies while losing populated replies
at the lobby/game transition.

The reply transfer is initiated automatically on receiving an MP CMD frame. This
appears to be driven by the frame control value: CMD frames typically use
`0x0228`, but other values such as `0x0208` also work. Exactly which values
qualify needs more research.

On receiving the CMD frame the hardware determines its position from its
`W_AID_LOW` register and the frame's client bitmask, then waits for its turn to
reply. It always sends a reply:

- if `W_TXBUF_REPLY1` is not configured it sends an empty reply frame, with
  frame control `0x0158`;
- either way `W_TXBUF_REPLY1` is latched into `W_TXBUF_REPLY2` and reset to
  zero;
- if there is a frame to send, the byte at `TXheader[04]` is set to `01` (or
  possibly incremented). Nintendo software checks for this.

No `W_TXBUSY` bit has been found for MP reply transfers.

A populated reply is valid only when its modeled airtime fits the client-time
field advertised by the command. If it does not fit, hardware behavior is to
send the default empty reply. This matters in Asphalt Urban GT 2: the command
advertises `0x0166` (358 us), while the staged 140-byte reply needs roughly
700 us.

While transferring the reply: IRQ7 is triggered and `W_RFSTATUS` is 8.
`W_RXTXADDR` is not modified when sending a default empty reply. At the end, if
bit 12 of `W_TXSTATCNT` is set, `W_TXSTAT` is set to `0x0401` and IRQ1 is
triggered.

## RX header frame-type field

`RXheader[00]` bits 0-3 hold the frame type. Clarified values:

| Value | Meaning |
| --- | --- |
| `C` | MP host CMD frame (typically framectl `0x0228`; also set for `0x0208`) |
| `D` | MP ack frame |
| `E` | MP client reply frame |
| `F` | empty MP client reply frame |

Which frame control values map to `C` needs more research.

## Frame phase: VCOUNT

A client whose `W_AIDLow` is still 0 when a command arrives answers with an
empty reply, the host's ack lists it as failed, and the game sends the command
again. On hardware this rarely happens because the clients' frames are lined up
with the host's: Nintendo's wireless library writes VCOUNT (`0x04000006`)
during VBlank to nudge the display timing towards the host's. Mario Kart DS
does it about once a second on each console. An emulator that drops VCOUNT
writes keeps whatever frame phase the two consoles started with, and in most
sessions pays two or three MP exchanges a frame instead of one. DSperate's
patch 0107 fixed this; DraStic still shows it ([drastic.md](drastic.md)).

## Emulator transport requirements

melonDS's LAN protocol, which both radios use, carries command, reply and ack
as separate message types with the host's microsecond timestamp. Three
properties of it matter, each learned the hard way by a transport that lacked
it:

- **Roles travel out of band.** Address bytes alone are ambiguous: a populated
  reply can resemble an ack template.
- **The client runs on the host's clock.** It adopts the host's timestamp on
  association, holds each host frame until its own clock reaches it, and blocks
  for the next. Two free-running clocks drift apart within seconds of a lobby
  starting a game.
- **Stale frames are dropped, not delayed.** MP frames go unsequenced, and
  anything older than a video frame is discarded. A reliable, ordered transport
  (TCP) delivers every frame late instead.

The host's command is also a real-time rendezvous that emulated time alone
cannot model: it blocks for the clients' replies, up to the MP wait (25 ms in
melonDS, 50 ms in Netplay's ad hoc sessions, 100 ms through an access point).
