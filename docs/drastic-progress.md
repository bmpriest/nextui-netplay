Major Brick milestone complete, 2026-08-29.

Current status (2026-08-31): this milestone has been surpassed. Two Bricks now
reach and hold an Asphalt Urban GT 2 lobby; starting the game still produces a
client communication error. See `drastic-brick-devbridge-progress.md` for the
current implementation and test record.

This records one milestone and is not the current state of the work. For that,
including the Wi-Fi model, the transport, and where the Mario Kart association
currently stands, see `drastic-brick-devbridge-progress.md`.

  We recovered and hardware-validated:

  - Real Wi‑Fi write dispatcher: 0x128d0
  - Wi‑Fi read handler: 0x13660
  - Event executor: 0x216e0
  - Embedded ARM7 CPU: system + 0x25cd340
  The final run completed with 572,496 reads, 11,501 writes, 4,007,718 event callbacks, and IRQ injection enabled.

  - Corrected the misleading zero-write conclusion in the documentation
  - Added an explicit IRQ injection gate
  - Added IRQ-edge accounting for future builds
  - Passed all native tests and git diff --check
  - Restored the Brick’s normal clean launcher

  The next engineering stage is implementing radio TX/RX queues and transport on top of these now-proven Brick boundaries.

The radio TX/RX queues and transport referred to above were built; the work has
since moved on to the association handshake and the multiplayer transfers.
