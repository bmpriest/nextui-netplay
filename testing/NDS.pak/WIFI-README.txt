DraStic Wi-Fi research build
============================

Current status, 2026-08-31
--------------------------

The TrimUI Brick research build now provides experimental local multiplayer.
Two Bricks can discover, authenticate, associate, and hold an Asphalt Urban GT
2 lobby for more than two minutes. It is not yet playable: when the host follows
the on-screen "Press A button to start" instruction, the host keeps player 2
listed but the client reports COMMUNICATION ERROR.

The active Brick test library is libdrastic_wifi_diag.so. Each launch produces
an NDS-wifi-*.txt trace in the NextUI log directory. Keep that trace together
with the ordinary NDS launch log when reporting results.

For automated testing, use the DevBridge macros under drastic-wifi/devbridge.
asphalt-cold-join.test creates a repeatable two-device lobby;
asphalt-observe.test records nondestructive checkpoints; and
asphalt-safe-quit.test exits through Advanced DraStic and waits for DevBridge.
Do not use generic QUIT_GAME or kill -9 during routine testing, because the
wrapper may need up to 45 seconds to restore DevBridge and clean launch state.

A30 historical model controls
-----------------------------

The controls below describe the earlier A30/hybrid-MMIO research path. Active
multiplayer development and hardware validation are currently Brick-first.

To test the first independently owned Wi-Fi state model:

  touch drastic/wifi_model.enabled

The A30 event/cycle trampoline is isolated behind a second marker while its
Mario Kart crash is investigated:

  touch drastic/wifi_cycle.enabled

Remove wifi_cycle.enabled to disable only the event hook. Remove
wifi_model.enabled to return to passthrough. Model mode currently shadows
DraStic's native MMIO state: native reads remain authoritative because device
testing proved that DraStic changes packet RAM through an internal path outside
the 16-bit store hook. Selective model-owned control registers will be enabled
only after those state boundaries are synchronized. The model includes packet
RAM, register storage, the baseband port and Wi-Fi sub-interrupt flags, but the
ARM7 interrupt callback is connected only on the A30 build. That adapter latches IRQ24 and
updates DraStic's active ARM7 interrupts; its first hardware wakeup test is
still pending. On A30, an exact-binary event hook now drives the portable 8 us
Wi-Fi timer from DraStic's 67,027,964 Hz emulated master cycles when the second
marker is present. A working cycle-enabled trace
will say both "event-cycle-hook=ready" and "arm7-irq-adapter=ready"; after a
normal exit, event_calls, arm9_cycles and wifi_us should be nonzero. Older Brick
traces may report both adapters as unavailable. That statement no longer
describes the active Brick diagnostic build, which implements radio frames and
the local NetLink multiplayer transport described above.
Include the launch log and Wi-Fi trace when reporting either a successful menu
test or a regression.

If DraStic's own Exit command hangs, run this over SSH:

  /mnt/SDCARD/Emus/my282/NDS.pak/hard-quit.sh

The command validates the recorded process, asks the launcher to terminate it,
and sends SIGKILL only if the normal stop does not complete within six seconds.

The trace stops after 16,384 register accesses to limit SD-card writes. Please
keep the trace together with the ordinary NDS launch log when reporting a test.
