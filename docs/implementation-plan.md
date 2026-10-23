# Netplay implementation plan

This is the consolidated backlog from both transcript reviews, the live-device
investigation, and the current source. It separates defects from product-policy
decisions: a state-machine observation is not automatically a bug, and items
already repaired are not left in the active queue.

## Completed baseline

- Preserve and validate the original emulator launcher when Netplay's bind
  mount is already active. This fixes the second-arm preflight failure.
- Recognise only Netplay-owned mounts and safely refresh staged launchers
  without deleting unrelated emulator paks.
- Hide frontend save/load/autosave while a netplay session is active; keep guest
  SRAM/RTC volatile and seed it from the host.
- Package the dedicated gambatte/gpSP link cores and the compatibility fallback
  cores separately and in the full distribution.
- Use ROM-content identity, peer-confirmed host checkpoints, host-authoritative
  rejoin, and a five-second shared-screen agreement interval.
- Merge duplicate LAN/ad-hoc discovery sightings while retaining the visible
  ad-hoc SSID and truthful scan count.
- Name original-Wi-Fi restoration explicitly, retry the A30 supplicant within
  the foreground deadline, retain failed watchdog recovery records, restore via
  the captured command on A30, and recognise generated `nextui-*` SSIDs.
- Make mount activation all-or-nothing. A partially covered system set is
  rolled back before trying launch stubs, and foreign mounts remain untouched.

## Current implementation progress

The following concrete portions of the P0/P2 plan are implemented on this
branch. Unlisted bullets below remain planned; this section does not promote
future feature descriptions into current behavior.

- **P0.1 recovery transactions:** early peer hashes are retained; initial wait
  has a deadline; timeline rings reset independently from transaction control;
  protocol sends share one drop/failure path; an unsent resync request cannot
  advance local recovery state.
- **P0.2 runtime exits:** initial sync, absent/disconnected peers, silent input,
  identity/load failures, and repeated desyncs have bounded, distinct failures.
  The shared-screen failure overlay offers wait/retry, process-local solo, and
  exit-game actions while persistence restrictions remain armed.
- **P0.3 transactional arming (partial):** failed arm/core negotiation uses one
  UI rollback path; partial sessions/bindings, AP/join state, announcements,
  listener, and power-save state are unwound. Foreground and watchdog Wi-Fi
  restoration share an atomic ownership lock. Reconstructing radio state after
  process restart remains P1.8.
- **P0.4 identity:** shared-screen ROM hash failure is fatal rather than a mode
  demotion; the protocol handshake includes mode, input delay, core identity,
  and state/save geometry.
- **P0.5 reconnect ordering:** a pending disconnect is consumed before a new
  netpacket generation is started.
- **P2.13 bookkeeping (partial):** local/peer hash rings are session-owned and
  reset with the timeline. Checkpoint/volatile-directory garbage collection
  remains.
- **P2.14 mount transaction:** activation reports failure if any eligible target
  or wrapped extra cannot be covered, allowing the caller to roll back and use
  one consistent fallback route.
- **P1.10 hosting navigation:** armed hosts and guests return to a state-aware
  main menu with Host/Join hidden. A detached broker keeps announcements and
  compatibility serving alive after the setup app exits and reports guests.
- **P1.7 compatibility feedback (partial):** manifest comparison runs even when
  compatibility fallback is disabled. The session records unresolved per-core
  mismatches, and the shim overlays either the selected compatibility build or
  desync-risk outcome for the first two seconds of live core frames. Kernel-
  enforced accepted-socket deadlines remain.
- **P2.16 reset policy:** shared-screen guest reset is rejected; host reset is a
  typed authoritative state transaction with guest ACK and host COMMIT. Link
  and solo reset remain local.
- **P2.17 controls (partial):** shim-owned failure overlays are interactive via
  RetroPad input. Menu-side status/actions still require a MinArch hook.
- **Link pause/transport hardening:** live menu pauses no longer expire after an
  arbitrary 30 seconds. Gambatte uses TCP_NODELAY, complete deadline-bound
  packet I/O, latency summaries, and preserves its Game Link socket across
  unrelated option changes.
- **SNES compatibility alias:** a pak requesting `snes9x2005_libretro.so` is
  routed to packaged Supafaust under the canonical `snes9x_libretro.so` name on
  both architectures. Its encoded 277,033-byte state and hashes were identical
  in an ARMv7/AArch64 Kirby Super Star test; both round trips were exact.
- **Gambatte paired frontend contract (partial):** Netplay pins the maintained
  `netdual7` fork, packages ABI-v1 paired cores for all five platforms, selects
  logical console A/B by role, exchanges SRAM/RTC, establishes a host paired
  checkpoint, and feeds the same delayed A/B input timeline to both replicas.
  Missing ABI support falls back to network serial. Periodic agreement,
  crash/rejoin, Continue Solo, and different-ROM selection remain active work.

## P0 — prevent hangs, divergent protocol state, and stranded devices

1. Make recovery transactions race-safe.
   - Keep an early peer hash until the matching local hash exists; do not pop it
     merely because the host is a few frames ahead.
   - Start the recovery clock when a client enters initial `WAIT_BEGIN`.
   - Split input/hash-ring reset from resync-control reset. Do not clear a fast
     ACK after the host has sent `BEGIN` and state.
   - Treat every failed identity, BEGIN, state, ACK, COMMIT, hash, checkpoint,
     and resync-request send consistently: drop the connection or enter a
     surfaced recovery failure, never silently advance one side's phase.
   - Do not advance the local divergent-state marker when a resync request was
     not actually sent.

2. Put deadlines and useful errors on all absorbing runtime states.
   - Distinguish never connected, initial synchronization timeout, missing
     remote input, disconnected peer, identity mismatch, and failed state load.
   - Cap repeated desync recovery in a time window so an incompatible or
     nondeterministic core cannot rewind every five seconds forever.
   - Keep a new TCP generation as the defined way out of a failed recovery.

3. Make arming transactional.
   - On every `NS_arm` or compatibility-negotiation failure, unwind the AP or
     joined network, announcements, compatibility listener, partial session,
     power-save change, and partial bindings through one cleanup path.
   - Serialize foreground Wi-Fi restore and watchdog restore so they cannot
     kill one another's newly started supplicant.
   - Preserve `wifi_restore` and ad-hoc session fields until an original-network
     address is confirmed. Only successful recovery may clear them.

4. Fail closed when shared-screen identity cannot be established.
   - A ROM hash failure must show a launch error; it must not silently demote a
     shared-screen core to the link/passthrough path.
   - Include protocol mode, negotiated input delay, core identity and state
     geometry in the handshake. A local guess must not let mGBA shared-screen
     wait forever against gpSP link mode.

5. Fix reconnect event ordering for libretro netpacket cores. Consume and stop
   the old disconnect before starting the new connection. A reconnect between
   two `retro_run` calls must leave the core started and connected, not stopped.

## P1 — bind a session to the intended peer and lifecycle

6. Make peer capacity explicit, then extend it deliberately.
   - Until multi-controller transport exists, admit one gameplay peer, actively
     reject extras with `BUSY`, and bind gameplay and compatibility negotiation
     to the selected peer/session. Do not let a queued socket accidentally take
     over after a disconnect.
   - Add a negotiated controller-slot field so a two-device session can assign
     the remote handheld to emulated port 1, 2, 3, etc. This is the small step
     and also provides the addressing needed by the larger one.
   - For simultaneous three/four-player shared-screen play, replace the single
     global socket with a host-side peer table, assign stable client/controller
     IDs, collect each frame's inputs, and relay the complete input set. Define
     per-peer disconnect, neutral-input, hash, and recovery policy. Configure
     multitap/controller devices where a core requires it.

7. Make compatibility negotiation non-blocking and session-scoped.
   - Put accepted sockets in non-blocking mode with real deadlines; the current
     user-space timeout cannot interrupt a blocking `recv`.
   - Tick the listener independently of the current UI screen while hosting.
   - Stop it on disarm, end-session and process teardown.
   - If installed builds differ and there is no common packaged fallback,
     report that explicitly instead of arming a predictably incompatible
     shared-screen session.

8. Reconstruct radio/session facts after process restart. Replace decisions
   based on `hotspot_running`, `joined_hotspot`, cached SSID, and the UI-only
   `hosting_hotspot` with observable hostapd/radio/config/session state. Reopening
   the app must not regenerate an SSID or tear down an AP serving a live peer,
   and ad-hoc input delay must not change after a relaunch.

9. Give discovery records a last-seen timestamp and expiry. Preserve selection
   by stable identity while lists merge or expire, report broadcast bind errors,
   and wait boundedly for the requested supplicant scan generation rather than
   relying only on whatever cache preceded the button press.

10. **Partial:** repair UI lifecycle edges.
    - Armed hosts and guests now use one durable status main screen; Host/Join
      are absent until X ends the session, and the broker reports host guests.
    - Return failed arm checks to the originating Host/Join flow, not always to
      Tools.
    - Use role-aware recovery text: the host is sending/committing state; the
      guest is receiving/rejoining.

## P2 — performance, cleanup, and maintainability

11. Move large authoritative-state transfer off the emulator's synchronous
    1 KiB-send loop, or at minimum use larger bounded frames and expose actual
    byte progress. Keep all socket work deadline-bound.

12. Reduce checkpoint write cost. Avoid synchronously writing and fsyncing a
    multi-megabyte tmpfs file on the emulation thread every agreement interval;
    coalesce or hand it to a worker without weakening the “peer-confirmed only”
    rule.

13. Make hash and recovery bookkeeping session-owned rather than function
    statics. Reset local hash rings/cursors on a new timeline and remove unused
    counters. Garbage-collect obsolete host checkpoint files and guest volatile
    save directories at a lifecycle point that still permits crash rejoin.

14. Finish the mount refresh transaction. When one mounted emulator changes on
    disk, refresh that target without allowing “some targets updated, one target
    silently stock” to count as a successful arm. Keep the existing ownership
    checks and never touch foreign mounts/paks.

15. Remove or clearly mark vestigial state. The UDP announcement `mode` is
    transmitted but unused; `simple_client` has no implementation; Gambatte
    instancing is now an experimental implementation while gpSP/mGBA remain
    visibly planned; core sharing is intentionally visible but frozen.
    Bind/listen/start errors should be surfaced instead of leaving empty screens.

16. **Implemented:** handle reset deliberately. In shared-screen mode, reject a
    guest reset and make a host reset a typed authoritative recovery transaction
    with ACK/COMMIT. In link mode, reset remains local to that device.

17. **Partial:** add an intentional in-game control/status surface.
    - Shim-owned failure overlays poll frontend input and now offer Wait/retry,
      Continue solo, and Exit game.
    - MinArch stops calling `retro_run` while its menu is open, so a panel beside
      that menu and MENU-button combinations require a narrow frontend hook:
      expose session status/actions to the menu renderer and raw frontend input
      handling. Do not start a competing `minui-btntest` process or read input
      devices behind the frontend's back.
    - Keep actions explicit: continue solo, retry/rejoin, end session, and
      dismiss. Do not map them onto controls a core can receive accidentally.

18. **Partial:** discovery and compatibility serving now follow session
    lifetime. A detached broker owns the UDP announcement socket, compatibility
    listener, one-peer admission with explicit `BUSY`, atomic UI status, and
    clean shutdown. A boot-identity check cleans session state that cannot have
    survived a reboot, without treating an empty hot-seat host as stale.
    Remaining work is authenticated pairing, binding admission to gameplay
    sockets, and game-offer/reply ownership.

## Platform validation matrix

19. Exercise shared-screen compatibility cores on both architectures and all
    five platforms, with special focus on cross-architecture pcsx_rearmed,
    SuperFX/SA-1 performance on A30, save-memory geometry, and repeated
    five-second hash agreement. Record cores that are deterministic only within
    an architecture.

20. Test these failure injections on real hardware: host and guest process
    crash, AP disappearance, first supplicant start failure, DHCP failure,
    partial state transfer, dropped ACK/COMMIT, silent compatibility client,
    extra client, mismatched mode/delay/ROM/core, menu pause beyond the timeout,
    and app relaunch while an AP is live.

21. Consider static addressing for ad-hoc peers so hosting does not require
    `udhcpd`. This is a platform-coverage improvement, not a prerequisite for
    the protocol fixes above.

## Accepted behavior decisions

- **Dead peer while armed:** offer an explicit transition to solo play. Never
  freeze indefinitely and never change modes silently. Some cores/games may
  still require the user to leave their multiplayer area.
- **Link-cable reconnect:** allow best-effort reconnect and continued local
  single-player operation; do not force both games to restart. Whether choosing
  solo also ends the durable session remains an implementation/UI decision—the
  safe default is to suspend reconnect attempts for the current game without
  silently disarming future launches.
- **Reset:** shared-screen host reset is an authoritative recovery transaction;
  shared-screen guest reset is rejected. Link-mode reset affects only the local
  device.
- **Hosting B button:** leave the screen without ending the session. Hosting
  should preferably continue advertising; selecting Host again re-enables it
  and displays peers if advertising is not made continuous.
- **Identity:** shared-screen requires identical ROM content. Link mode requires
  a matching link protocol/core but explicitly allows different ROM hashes.
