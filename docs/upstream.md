# Upstream fixes

Changes that belong in someone else's project. Each one Netplay works around
today; fixed upstream, the workaround (or a whole class of desync) goes away,
for Netplay and for anyone else running the same code. Grouped by project,
most valuable first within each.

## Emulator cores

Rollback netplay (RetroArch's) and resynchronization (Netplay's) both rest on
one property: `unserialize(serialize(x))` must give back exactly `x`, and
`serialize` must not change anything. Where a core breaks that, two devices
part ways - RetroArch on every wrong guess, Netplay whenever an option or a
load differs between the sides. Supafaust and snes9x hold it (byte-identical
round trips, 2026-11-02); fceumm and picodrive do not.

### fceumm (libretro/libretro-fceumm)

**1. MMC1 forgets its last-reset timestamp on load.** `boards/mmc1.c` saves
`lreset` (chunk `LRST`), but `MMC1_Restore` sets it to 0, because what it is
compared against - `timestampbase + timestamp` - is not in the state. A device
that has loaded a state therefore differs from one that has not, in those 8
bytes, for the rest of the session. Every MMC1 game is affected; it was found
with Bubble Bobble between two spruce RetroArchs (2026-11-02). River City
Ransom is MMC3 (mapper 4) and does not have it. RetroArch's host never loads
the state it sends, so its client's CRCs mismatch until the host's first
rollback replay.

Fix: save the elapsed cycles since the reset instead of the absolute value
(`timestampbase + timestamp - lreset`, clamped at the 2-cycle window that is
all it is used for) and rebuild `lreset` from it on load; or save
`timestampbase` alongside. Either makes the restore exact.

**2. High-quality sound keeps unsaved samples in its filter path.** With
`fceumm_sndquality` High or Very High, `sound.c` renders into `WaveHi` and
filters it (`NeoFilterSound`); the samples left over at the end of a frame are
moved to the front of `WaveHi` and filtered next frame. The state saves the
filter accumulators (`FAC1`/`FAC2`, `sexyfilter_acc1/2`) and, for Low quality,
the first 32 entries of `Wave` (`WAVE`), but nothing of `WaveHi`. After a load
the leftover samples are whatever the loading device had, so the next frames'
filter output - and with it `FAC1`/`FAC2`, which are in every later state -
differ from the saving device's. Measured 2026-11-04: River City Ransom,
NextUI host at High, spruce guest; after an option change on the guest, the
two devices' states differed only in `FAC1`/`FAC2`, at every checkpoint and
through every resync. cc8d792's offline replay found the same: at High,
unserialize keeps pre-load filter state.

Fix, in order of preference: save the pending `WaveHi` samples (the `left`
count and that many values), as `WAVE` does for Low; or treat the output
filter as output, not emulation - leave `FAC1`/`FAC2` out of the state and
reset them on load. The first keeps sound continuous across a load; the
second is smaller and is all netplay needs.

**3. (Not a bug, worth documenting.)** Sound options are part of the state:
quality and volume change what `FAC1`/`FAC2` hold. Two devices with different
`fceumm_sndquality` or `fceumm_sndvolume` can never agree on a CRC. Netplay
runs the host's values on both (docs/analysis/core-options.md); RetroArch has
no way to, so a note in the core's netplay documentation would save others the
search.

### picodrive (libretro/picodrive)

**1. A state does not round-trip.** `unserialize(serialize(x))` differs from
`x`: 1 byte of 678,519 at offset 140429 in one measurement, 649 bytes in
another (Streets of Rage 2, every build: stock armv7 and aarch64, FAME/CZ80,
`NO_ARM_ASM` at -O0, -O2 and -Ofast). Loading a state also changes live state
beyond what was saved: tens of bytes around offset 66000, still different 600
frames later (2026-11-02). RetroArch-to-RetroArch picodrive sessions between
two spruce devices mismatched at every CRC check in all three runs, with about
50 rollbacks a minute on the device at input latency 0.

**2. `serialize` changes the emulator.** One extra `serialize` call costs 7-8
bytes of divergence from offset 66013 within 900 frames
(`shim/test/statecheck.c --serprobe`). RetroArch's netplay serializes every
frame, so two RetroArchs only agree while they serialize at identical points.

Not yet located: the offsets need mapping to picodrive's state chunks (the
`statecheck` tool prints both; `pico/state.c` writes the chunks). That mapping
is the next step before a patch. Lockstep Netplay survives both today only
because both devices serialize and load at the same frames.

**3. Already patched here:** `cores/patches/picodrive-old-arm-hwcap.patch`
(build on older ARM kernels).

### gpSP

Two RFU fixes, the original author's and still unmerged
(`cores/patches/gpsp-001-rfu-disconnect.patch`, `gpsp-002-rfu-queue-size.patch`),
and `gpsp-003-netplay-version.patch`.

### Gambatte

The serial receive timeout. And in NextUI's cores makefile, `HAVE_NETWORK=1`
for Gambatte: one line, and no separate Gambatte link core.

## RetroArch (libretro/RetroArch)

**RESUME never unpauses (1.22.2).** `remote_unpaused` in
`network/netplay/netplay_frontend.c` tests

    if (sc->flags & ((NETPLAY_CONN_FLAG_ACTIVE | NETPLAY_CONN_FLAG_PAUSED)
          == (NETPLAY_CONN_FLAG_ACTIVE | NETPLAY_CONN_FLAG_PAUSED)))

`==` binds before `&`, so this is `flags & 1`, the ACTIVE bit: true for the
connection that just resumed, and RetroArch stays paused. Fix:

    if ((sc->flags & (NETPLAY_CONN_FLAG_ACTIVE | NETPLAY_CONN_FLAG_PAUSED))
          == (NETPLAY_CONN_FLAG_ACTIVE | NETPLAY_CONN_FLAG_PAUSED))

Netplay never sends PAUSE to a RetroArch peer because of it (43ad5dd). Check
master before filing: its netplay has been reworked (protocol 8, lockstep host
mode).

## MinArch (NextUI)

`RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE`: about ten lines, and any
netpacket core could link without the shim.

## spruce

**1. `enable_wifi` imports a foreign supplicant's networks.** On every game
exit `spruce/scripts` adopts any `wpa_supplicant` running on `wlan0` and
copies its networks into `Saves/spruce/wpa_supplicant.conf`. Netplay's first
ad hoc join, which replaced the supplicant as it does on NextUI, left
`nextui-XXXX` in the user's saved networks at priority 99. Netplay now adds the
ad hoc network to spruce's own supplicant at runtime instead; spruce should
still only import networks the user added.

**2. A netplay-aware in-game menu.** spruce's in-game menu (spruceUI/RA,
`common_src/spruce_igm_items.c`) reads its rows from one fixed path,
`RetroArch/igm_items.json`. An environment variable naming a different item
list would let a session show Resume, RetroArch Menu and Exit only, without
overwriting the user's file. Also: the menu writes fast-forward, rewind and
shader changes straight into the main config on close, bypassing an
`--appendconfig` session overlay.

**3. Input latency 0 on the RG35XXH.** spruce's H700 platform config sets
`netplay_input_latency_frames_min = 0`, range 0 (the Brick's is 1, range 2).
At 0 RetroArch runs nearly every frame on a guessed input - 95-99% in our
captures - and rolls back whenever the other player presses anything. 1 to 3
costs one to three frames of input lag and cuts guessing to a few percent.

## DSperate and DSP.pak

The emulator fixes (VCOUNT writes, late transport `begin()`, LAN join retry)
to DSperate, and the launcher and pad changes to josegonzalez's pak. DSP.pak
could then be an overlay like DraStic's instead of a full fork.

## Varnish

The ICON and MENU commands, input grab and H700 port, from the bmpriest fork to
Helaas's repository ([analysis/varnish.md](analysis/varnish.md)).

## Device firmware (report only)

**TrimUI Brick, xradio WiFi driver.** The driver forces its fast power-save
mode whenever it applies its ARP filter - on every association and address
change - even after `iw set power_save off`, which still reports off. Under
power save the Brick misses beacons and loses the access point for minutes.
`/sys/kernel/debug/ieee80211/phy0/xradio/ps_disable` forces it off until
reboot. Vendor code; worth a report to TrimUI, not a patch.
