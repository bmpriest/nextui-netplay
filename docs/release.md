# Complete RC releases

## Artifacts

| Archive | Extraction location | Installed application |
| --- | --- | --- |
| NextUI-Netplay-Complete.zip | SD `Tools/<PLATFORM>/` | Netplay.pak/ |
| Spruce-Netplay-Complete.zip | SD root | App/Netplay/ |

NextUI includes all five platform builds, Varnish on its supported platforms,
ordinary Gambatte/gpSP, both paired cores where built, MGBA.pak, DSP.pak, the
supported NDS.pak base/radio overlay, and ARMv7/AArch64 compatibility cores.
Spruce includes four AArch64 builds, ordinary/paired cores, the DraStic radio,
PyUI frontend/license, runtime libraries and AArch64 compatibility cores. It
uses Spruce's installed RetroArch and DS emulators rather than installing
NextUI paks. Its target device matrix is in [spruce-parity.md](analysis/spruce-parity.md).
No ROMs are packaged. Neither edition downloads compatibility cores in-app.

The old `Netplay.pak.zip` remains an intermediate base package, and
`compatibility-cores.zip` remains a standalone compatibility pack. They are
not the planned downloadable Core edition.

## Build

Use the pinned sibling build-platforms checkout (or set `BUILDER`) and Docker.
The Makefile fetches/builds pinned source dependencies where required.

```sh
make app app-spruce shim
make cores
make cores-gambatte-dual
make cores-mgba cores-mgba-dual
make compatibility
make cores-dsp cores-nds varnish udhcpd
make test
make dist-complete
```

Build prerequisites are explicit for Complete compatibility cores. Artifacts
are under `dist/`. `SPRUCE_PLATFORMS` defaults to
`tg5040 tg5050 my355 h700`; do not advertise models whose build/runtime is
omitted. Build scripts support mGBA on all five NextUI platform directories;
performance limits, particularly the A30, remain.

Before shipping, verify ZIP integrity, both layouts, executable modes, AArch64
machine types for the Spruce runtime, packaged dependencies, and absence of
ROMs, mutable sessions, credentials and test artifacts. Record SHA-256 sums. Each archive includes SOURCE_COMMIT; the validator
checks it against the current checkout as well as matching packaged native
binaries to the local build outputs.
Use a clean checkout or record the final commit; packaging does not publish a
GitHub release. The final source commit includes the reviewed UI and harness
changes. Historical test reports must retain their original results and
measurement limitations.

## Runtime requirements and scope

Spruce reads `App/Netplay/config.json` to filter the app by model/family. The
launcher maps the actual firmware name to a packaged build and selects the
matching PyUI device/display setup. This allowlist is deliberate: other
64-bit nightly targets are not automatically covered. The native binary's
presence alone cannot establish architecture or library compatibility. The
RK3566 native control backend uses the H700 SDK's portable SDL/GLES ABI and
matching link ABI, avoiding the Flip SDK's vendor Mali hook; its core
artifacts remain the my355 builds. At runtime, RK3566 uses Spruce's SDL
libraries in App/PyUI/dll through temporary SONAME aliases so KMSDRM is
available on dArkMoss.

Hardware exercise has focused on the Spruce Brick/RG35XX H; the remaining
named models are RC targets awaiting their own device validation. 32-bit
RetroArch and DS emulator paths are not routed. Hybrid traffic defaults on,
both dual modes and verbose logs default off; existing preferences persist.
