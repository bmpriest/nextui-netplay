# DraStic Wi-Fi hook

The shipped library is the **radio**: DSperate's port of melonDS's Wi-Fi block
and LAN transport (`src/radio/`), attached to DraStic 2.5.2.0 on AArch64 by
`src/radio_hook.c`. It speaks melonDS's LAN protocol, so DraStic plays DS local
wireless against DraStic, DSperate or melonDS. Two handhelds running DraStic
(Brick and RG35XXH) race Mario Kart DS at 55-58 fps over Netplay's ad hoc
network. See `docs/nintendo-ds/drastic.md` for the design, measurements and how
Netplay installs it (an overlay on the user's NDS.pak).

    make drastic-radio   build/../build-brick/libdrastic_wifi.so (pinned tg5040 toolchain)
    make cores-nds       the NDS.pak overlay for tg5040, tg5050 and h700

`DRASTIC_WIFI_LAN=host|join:<IPv4>` turns it on (the overlay's launch.sh sets
it from the Netplay session); without it nothing is hooked.
`DRASTIC_WIFI_LAN_TIMEOUT_MS` sets the MP wait.

drastic-wifi is GPL-3.0-or-later (LICENSE), as the code it carries is.

## The diagnostic library

The first attempt, a hand-written model of the DS Wi-Fi hardware
(`src/model.c`, `src/adapter.c`, `src/transport.c`, `src/hook.c`), still builds
as `libdrastic_wifi_diag.so` for MMIO tracing, and is the only library for the
A30. It never got past a lobby (see `docs/nintendo-ds/drastic.md` for why), but
its hardware findings are what the radio hook is built on:

- **Builds hooked.** A30: the fixed-address ARMv7 DraStic 2.5.0.4. Brick: the
  AArch64 PIE DraStic 2.5.2.0, build ID
  `5665dbdad6bec617b3f828afb2f69b0a14460764`. Every overwritten instruction is
  verified before patching. The adjacent 8- and 32-bit stubs are not hooked: an
  eight-byte absolute jump would overwrite their neighbours.
- **Hook points (Brick).** Reads: `load_wifi_16` at `0x13660`. Writes: the ARM7
  16-bit I/O dispatcher at `0x128d0`, which inlines its Wi-Fi fallback at
  `0x131a8` (the standalone raw store at `0x138f0` is not reached by Mario
  Kart). Events: `execute_events` at `0x216e0`, whose cycle delta converts the
  67,027,964 Hz master clock to microseconds.
- **Deferred install.** Advanced DraStic installs its own detours early in
  `main`, so the Brick hooks are written three seconds after `main` starts;
  installing from the constructor crashed. The A30 installs immediately.
- **IRQs.** The ARM7 is embedded at system offset `0x25cd340`, with interrupt
  and state fields at `0x2108`/`0x2110` and ARM7 I/O at memory offset
  `0x23070`.
- **Packet RAM** is initialised through an internal path the 16-bit store hook
  cannot see, so a detached copy breaks games; the radio uses DraStic's own at
  `memory + 0x17070`.

Running it:

```sh
export LD_PRELOAD="$PWD/libdrastic_wifi_diag.so"
export DRASTIC_WIFI_TRACE=/tmp/drastic-wifi.log   # required; passthrough tracing
export DRASTIC_WIFI_MODE=model                    # optional: shadow into the model
export DRASTIC_WIFI_CYCLE=1                       # optional, A30: the event hook
./drastic game.nds
```

The trace is capped at 16,384 records. The constructor accepts only a direct
`drastic` command or the packaged `ld-linux-armhf.so.3 ... ./drastic` form, then
removes `LD_PRELOAD` and the hook variables from DraStic's environment so
UI-spawned helpers do not patch themselves. The A30 pak's `hard-quit.sh` ends a
wedged DraStic over SSH (TERM, then KILL after six seconds).

`devbridge/` holds the DevBridge scripts and PC peer used to develop both
libraries; `devbridge/peer/` is the headless DSperate peer the radio was first
tested against.
