# Spruce RC coverage and NextUI parity

Current code and packaging, 2026-11-06. A release target means the app allowlist,
launcher runtime and packaged AArch64 binaries cover the device; it does not
mean every model has been tested physically.

| Spruce device | PyUI identity | Build directory | Physical validation |
| --- | --- | --- | --- |
| TrimUI Brick | TRIMUI_BRICK | tg5040 | Main Spruce test rig |
| TrimUI Brick Pro | TRIMUI_BRICK_PRO | tg5040 | Targeted, not tested here |
| TrimUI Smart Pro | TRIMUI_SMART_PRO | tg5040 | Targeted, not tested here |
| TrimUI Smart Pro S | TRIMUI_SMART_PRO_S | tg5050 | Targeted, not tested here |
| Miyoo Flip | MIYOO_FLIP | my355 | Targeted, not tested here |
| Anbernic H700 XX family | ANBERNIC_RGXX family, model/panel-specific identity | h700 | RG35XX H tested |
| Powkiddy RGB30 | RGB30 | my355 | Targeted, not tested here |
| Powkiddy RGB20SX | RGB20SX | my355 | Targeted, not tested here |
| Miniloong Pocket 1 | MINILOONG_POCKET1 | my355 | Targeted, not tested here |

The RK3566 targets use the Flip's AArch64 build and matching ABI, with their
own PyUI device, KMSDRM display and runtime paths. Native prompts/control
use Spruce's installed SDL libraries through temporary SONAME aliases, avoiding
the H700-only fbdev backend on these systems. The Pocket 1's rotation and
scaling belong to its PyUI device class. Use a Spruce build with these classes.
Other 64-bit nightly devices are outside this RC's allowlist.

## Shared features

Shared-screen LinkPlay, RetroArch protocol for supported shared-screen cores,
ordinary Gambatte/gpSP linking, optional Gambatte/mGBA Dual, compatibility-core
negotiation, game invitations, persistent sessions, save protection/backups,
checks and Wi-Fi recovery are implemented on both firmwares. Both dual modes
default off; hybrid LinkPlay traffic defaults on; verbose logs default off.
RetroArch protocol does not transport NETPACKET link-core traffic.

Spruce has a native PyUI menu with contextual glyphs and X to end sessions.
It presents one combined core list; GBA includes gpSP and optional mGBA Dual,
and NDS combines DraStic/DSperate. It reads Spruce's actual emulator selections.
It does not install NextUI's MGBA.pak, DSP.pak or NDS.pak.

## Firmware differences

- NextUI alone has the Varnish START quick menu, status icon, GameSwitcher
  shortcut and Ad Hoc Network Switch. Invitations also use Varnish on NextUI
  and are independent of the START-menu setting.
- Spruce uses its own supplicant for ad hoc joining and restoration, preserving
  saved networks. It owns sleep, power controls and emulator choices.
- Spruce hooks `ra64.universal` and `ra64.h700`, plus `drastic64` and
  `dsperate` under Emu/NDS. 32-bit RetroArch and DraStic-Steward remain outside
  these hooks. DraStic and DSperate use their own radio transport, not hybrid
  LinkPlay's UDP inputs.
- X ends a session and leaves inactive passthrough wrappers. Turn Off Netplay
  also removes the bind mounts. Neither action uninstalls emulator files.

## Validation and remaining limits

The original autonomous suite and transport comparison are historical runs.
The subsequent fixture updates repair cable navigation and DS screen references,
add per-launch Spruce DS log archives, and recognize stock DSperate summaries.
Stock summaries establish session/peer replies; they provide no pacing windows
and must not be reported as measured FPS. Updated fixtures do not retroactively
change old report verdicts. See [the broker plan](../analysis/devbridge-test-broker.md)
and [test instructions](../../testing/spruce/README.md).

Hardware coverage remains concentrated on the Brick and RG35XX H. PS across
architectures, other models' controller/display behavior, and DS games outside
the recorded fixtures remain RC validation limits. Network and emulator
performance should be attributed to the actual game, protocol and run rather
than generalized from device proximity or a single historical result.
