# DevBridge device test broker plan

## Current harness status (2026-11-06, RC)

After the runs below, the fixture review updated Gambatte cable navigation,
Mario Kart input timing and H700 DS screen references, and added reviewed
reference provenance. Spruce DS launches now archive emulator output when
verbose logs are enabled. The checker and suite judge support stock DSperate's
joined-session and MP-wait summary as well as patched pacing logs. Stock
summaries do not establish FPS or continuous timestamped gameplay coverage;
those performance fields remain NO_DATA. The fixtures were separately reviewed and adjusted. Existing generated
reports remain evidence of their original
runs, not a fresh RC-wide result. Re-run with the current fixtures to produce
new verdicts; do not carry the historical navigation failures forward as proven
current product failures.

## Historical autonomous suite status (2026-11-06, before fixture review)

The 26-cell spruce suite has completed with 13 PASS, 3 PRODUCT_FAIL,
4 INFRA_FAIL, and 6 declared UNSUPPORTED; no cells remain NOT_RUN. See the
[generated report](../../testing/results/broker-autonomous-20261106/report.md)
for per-attempt evidence and retries. Existing CTR results and references were
reused; LinkPlay CTR passes. RetroArch CTR reaches a race but its guest reports
CRC failures. Gambatte cable cases fail the link-traffic/multiplayer gate.
RetroArch link-core cases are declared unsupported before launch because the
adapter does not transport NETPACKET traffic.

All four DS pairings remain INFRA_FAIL. The fixture snapshots both devices'
layouts, applies top-above-bottom views, and restores the originals. Bounded
screen checks and conditional branches replace blind repeated menu actions.
The latest DSperate pilot still cannot move the H700 main-menu selection to
Multiplayer. Raw SDL receives injected physical-pad directions, and process
inspection confirms DevBridge's UI closes during emulation; neither finding
establishes successful DS navigation. DS references remain review-gated.

Teardown now checks emulator executable paths, and tests reject solo continuation
and numeric RetroArch CRC failures. ADB commands propagate remote failures even
on legacy adbd. Seven runner guard tests and ten protocol tests pass. The final
baseline is the recorded ad hoc network, LinkPlay host/client configuration,
and DevBridge in front. No instrumentation overhead profile is validated and
network RTT remains NO_DATA; these are functional results, not protocol
performance comparisons. Typed device status and native DS telemetry remain
migration work.

## Implementation progress (2026-11-05)

The initial PC broker is implemented in `DevBridge.pak/tools/broker.py`, with
device leases, journaled phases, per-attempt artifacts, collection and teardown
after failure, resume of passed cells, and a report that includes unexecuted
cells. `ASSERT_STATE` and `WAIT_STATE` are implemented in the existing test
runner. Offline telemetry calculations retain RTT distributions and reject
disconnects inside a measurement interval. Twenty-four broker, state,
telemetry, and existing protocol tests pass. The broker control-channel smoke
suite passed on the actual spruce Brick and RG35XXH; both reported DevBridge
in front and input ready. Artifacts are under
`testing/results/broker-control-smoke-20261105`.

The spruce scripts now propagate launch/play failures, collection judges both
archived device logs, and missing performance data produces NO_DATA. The
recorded Mario Deluxe LinkPlay run passes the new interim connection gate;
the RetroArch-over-Netplay solo continuation fails on both device logs.

This does not complete the migration. Network and emulator fixtures remain in
the existing scripts/matrix; typed agent launch IDs and event cursors, DS
readiness/reference repair, conditional routing, full suite manifests, and
the instrumentation overhead comparison still need implementation and physical
validation. No instrumentation profile has been approved for performance
claims, and no new emulation performance result is asserted by the smoke run.

## Goal and success criteria

Run a named test or suite on registered devices without manual intervention. A run must
set up the devices, tolerate bounded startup and menu timing differences, collect
evidence from both sides, restore their prior state, and produce an honest verdict.
An interrupted run must be resumable. A disconnected multiplayer game must never
pass because its test kept playing solo.

The first target is the spruce Brick and RG35XXH over the NextUI Brick's ad hoc
network. The initial suite compares RetroArch over Netplay with LinkPlay for FC,
SFC (Snes9x and Supafaust), MD, GB (Gambatte link cable and paired instances),
GBC, GBA (gpSP and mGBA), NDS (all DSperate/DraStic pairings), and PSX. Pure
RetroArch is outside this suite. Keep the existing NextUI matrix working during
the migration.

Success means: one broker command runs the suite from an idle rig, every cell
ends as PASS, PRODUCT_FAIL, INFRA_FAIL, UNSUPPORTED, or NOT_RUN, every verdict
links to its evidence, and all devices return to their recorded starting
configuration even after a failed cell. A suite summary must count the planned
cells, not just those that happened to run.

## Current assets and gaps

`DevBridge.pak/tools/devbridge.py` already registers SSH/ADB connections for a
run, executes the test language, records `events.jsonl`, and provides `VERIFY`,
`VERIFY_WAIT`, and `VERIFY_UNTIL`. Keep these as the screen navigation primitives.
`testing/devbridge/matrix.py` already has rig definitions, device recovery,
tunnels, retries, per-attempt directories, and a resumable JSONL report. Move
these capabilities into the broker rather than replacing them. The spruce
comparison separately uses `testing/spruce/{adhoc,arm,launch,play,collect}.sh`,
`table.py`, and shell helpers. This split produced two independent notions of
success and let a solo game count as a multiplayer pass.

`OPEN_GAME` currently acknowledges a queued request, not a running game.
`STATUS` reports foreground and input availability but no game or peer identity.
`matrix.py` sometimes borrows another platform's screen reference or captures
an unchecked missing reference; those captures are useful diagnostics, but
must not make a visual assertion pass. Fixed waits in DS tests press through
menus before slow devices reach them.

## Ownership and interfaces

## Measurement integrity and latency

The broker and tests must preserve normal emulation speed, audio, frame pacing,
and image quality. This is a release gate for the testing system. Run orchestration,
analysis, matching, and report generation on the PC. Device collectors must be
bounded, buffered, and passive where possible. Do not continuously copy framebuffer
images, poll via new SSH processes at high frequency, or enable verbose per-frame
logging during the measured gameplay interval. Capture navigation screenshots
before that interval and diagnostic screenshots after it. Collectors must report
their sample rate, CPU time, memory, bytes written, and dropped records.

Validate overhead on representative light and heavy cores with repeated matched
runs, alternating minimal instrumentation and the proposed telemetry profile.
Compare frame pacing, FPS, audio underruns where available, and emulator CPU time.
Use the same inputs and settings, quantify natural run-to-run variation, and reject
a telemetry profile that introduces a measurable regression beyond that variation.
Publish the comparison and the instrumentation profile with the suite. No arbitrary
CPU percentage alone establishes that instrumentation is harmless. If a collector
cannot meet this gate, reduce its frequency or remove it from performance runs.
Do not change CPU governor, audio, rendering, or synchronization settings merely
to make the broker work; any test-controlled setting must be explicit in the
manifest and consistent across the protocols being compared.

Report average network round-trip latency (RTT), median RTT, p95 RTT, sample count,
and loss over the named measurement interval, with direction, source, and sampling
method. Prefer RTT already measured by the session protocol. If unavailable, use
a validated low-rate probe profile (initial candidate: one small probe per second)
and label it as probe RTT; do not substitute SSH command duration. Validate the
probe's overhead alongside the other collectors. Do not mix probe RTT with session
RTT in a single comparison column, and report unavailable measurements explicitly.
One-way latency needs synchronized clocks and is not estimated by halving RTT.

Keep network RTT separate from configured input delay, time blocked waiting for
peer input, and end-to-end button-to-display latency. These describe different
effects. Existing stall and freeze figures remain alongside RTT. Button-to-display
latency requires an independently validated capture method and is outside the
initial suite. Latency statistics must retain their sample distribution so that
an average cannot hide occasional long waits.

### Device protocol: observe and act on one device

Keep the installed protocol small and line-oriented. Add a version/capability
reply so a broker can reject an old agent before starting. All mutations return
a request ID and a terminal result. Add the following structured observations:

- `STATUS`: foreground, game PID and process start time, emulator, ROM path and
  hash, selected core, active session ID, netplay protocol and role, peer
  address, connection state, and last connection change time. Unknown fields
  are explicit `unknown`, not omitted or guessed.
- `REQUEST_STATUS ID` and `WAIT_REQUEST ID TIMEOUT`: queued, launching,
  running, failed, or exited, with exit code and launch-log path. `OPEN_GAME`
  keeps its current quick acknowledgement for compatibility, while the broker
  waits for the request's terminal state and checks the observed game identity.
- `WAIT_STATE predicate TIMEOUT`: wait for a stable device state such as
  `foreground=game`, `session=armed`, or `peer=connected`. State is sampled
  repeatedly and must remain true for a short configurable settling period.
- `SUBSCRIBE` or `EVENTS since=cursor`: monotonically numbered process,
  foreground, network, session, and peer events. Reconnect returns events
  after the last cursor. The broker timestamps receipt as well as device time.
- `CAPTURE`: screenshot and optional bounded game-log, process, and network
  snapshots, labeled with run, cell, step, device, and monotonic timestamp.

Expose Netplay's session status from the application or launcher through a
stable local status file/API. DevBridge should read that contract; it should
not infer a live multiplayer connection merely from a process name or an
`armed` flag. At minimum Netplay reports session ID, protocol, role, core,
peer connected/disconnected, connected-at time, and frame/checkpoint counters.
DS emulators need their own status adapter where that contract does not apply.

Protocol responses need a consistent envelope: schema version, request ID,
device ID, state, error code, message, and data. Shell commands and `TRM` remain
available for development, but planned tests should use typed commands. A
`TRM` command that exits nonzero is always a failed step. No output filter or
pipeline may turn that into success.

### Broker: own devices, setup, execution, telemetry, and verdicts

Add a broker module/CLI to DevBridge, starting by extracting the proven
`matrix.py` device transport, tunnel watcher, preparation, teardown, retry,
and report code. Its inventory registers device ID, model, firmware, build,
transport endpoints, credentials by environment/secret reference, capabilities,
ROM inventory, core inventory, and health. Registration validates agent
version, input injection, screenshots, storage, time, and available routes.
Credentials never appear in artifacts.

The broker owns a device lease: one run may control a device at a time. It
records a snapshot of settings it changes (core choice, save/autoload behavior,
radio power save, network, Netplay session, display layout) and restores it in
`finally`. Before each cell it confirms the expected baseline. If recovery
fails, mark infrastructure failure and quarantine that device from further
cells rather than cascading misleading game failures. A restart reads the
run journal and reconciles devices before resuming.

Network topology is a broker fixture, not a game-test script: house Wi-Fi,
NextUI Brick AP, ad hoc leases, and SSH tunnel creation/reconnection have one
implementation. Core/emulator selection and Netplay arming are fixtures with
observable postconditions. Keep platform-specific mechanics in adapters
(`spruce`, `nextui`, `dsperate`, `drastic`) behind the same broker operations.

The broker also owns telemetry windows. It starts captures on both devices
before launch, records a named `play_start` and `play_end`, and collects logs,
packet captures, frame counters, peer events, process exits, and screenshots
even after a failure. Telemetry collectors declare coverage and clock source;
missing data is `NO_DATA`, never zero or a pass. The report generator consumes
this normalized data, not raw shell output or the most recent screenshot in
an unrelated directory.

### Test definitions: declare game behavior and assertions

Define a manifest for each cell: game/ROM hash, host and guest, transport,
Netplay protocol, core/emulator pair, required capabilities, expected mode,
telemetry profile, timeout budget, and assertions. The same gameplay script
can be parameterized across protocols and cores; unsupported combinations
are declared with a reason before launch. A suite manifest expands to a
deterministic list of cell IDs so a report includes missing cells.

Keep `VERIFY`, `VERIFY_WAIT`, and `VERIFY_UNTIL` for screen routing. Add
`IF/THEN/ELSE` only for observed, named conditions such as a verified screen
or a typed device state. A condition records what it observed and which
branch ran. Give branches an overall deadline and maximum iteration count;
avoid open-ended retries. Add `ASSERT` for semantic states and paired
conditions (`both peer-connected`, `same ROM hash`, `correct host/client
roles`). A timeout automatically captures both screens and status.

Tests distinguish navigation checkpoints from measurement. Mario Deluxe,
for example, verifies its title, third menu item, VS stage picker, and both
race screens before starting the timed window. A screen reference is scoped
by game, emulator, firmware/model, and layout; missing references produce
`NEEDS_REFERENCE`, not PASS. For visually flexible menus, support region
matching, multiple approved references, and a stable-frame requirement.
Do not auto-promote a captured screen to an approved reference. Review and
record references separately.

## Cell lifecycle and verdict contract

Each cell follows a journaled state machine:

1. `PREFLIGHT`: lease devices; verify agent versions, capabilities, ROM hashes,
   core versions, battery/storage, routes, and required references.
2. `BASELINE`: stop stale games, recover the intended network, snapshot and
   apply settings, verify DevBridge input and foreground on both devices.
3. `ARM`: start host and join guest; verify both report the same session and
   expected roles/protocol. Establish and monitor any tunnel.
4. `LAUNCH`: issue identified launch requests; wait for both expected games
   and cores. A queued request alone is insufficient.
5. `NAVIGATE`: run verified, bounded menu steps. Record screenshots and
   branch decisions. Confirm the intended multiplayer mode on both screens.
6. `MEASURE`: begin the named gameplay interval; require the peer to stay
   connected throughout, and collect frame/checkpoint and packet data.
7. `JUDGE`: evaluate visual, semantic, connection, synchronization, and
   performance assertions independently. Preserve the raw observations.
8. `TEARDOWN`: stop collectors and games, end the session, restore settings
   and house connectivity, verify the baseline, release leases.

Every stage has a deadline and a result. A product failure is a game that
launched under a healthy rig but violated a game/netplay assertion. An infra
failure is unreachable transport, broken capture, stale agent, or failed
setup/restore. `UNSUPPORTED` requires a declared incompatibility, such as a
core that cannot serialize under the RetroArch protocol; it is not inferred
from a failed test after the fact. `NOT_RUN` covers planned cells skipped due
to a quarantined device or interrupted suite. Retries preserve every attempt
and never overwrite a failure; the report marks a pass that required retry.

Artifacts use `run-id/cell-id/attempt-n/`: manifest and resolved device
inventory, state journal, event JSONL, exact rendered test, step results,
screenshots, host/guest logs, packet captures, normalized telemetry JSON,
and verdict JSON. The suite report links each cell to that directory. An
atomic result write makes resume safe after a broker crash.

## Migration sequence

### Phase 1: make current results trustworthy

- Fix the spruce scripts to propagate failures through pipes and stop when
  either launch is rejected. Record explicit `NOT_RUN` for later steps.
- Add post-launch game identity and peer-connected checks using available
  status/log sources; fail the RetroArch Mario Deluxe solo continuation.
- Refuse performance rows without a valid measurement window or frame
  samples; report the reason instead of crashing in `table.py`.
- Remove borrowed NextUI DS references from spruce pass criteria. Keep
  captures for reference review.
- Create a fixed suite manifest and a report of all planned cells, including
  those not yet run. This is the immediate audit baseline.

Gate: rerun Mario Deluxe LinkPlay and RetroArch-over-Netplay. LinkPlay must
pass in VS mode; the RetroArch case must fail or be declared unsupported at
the save-state boundary, never pass after disconnect.

### Phase 2: promote the existing matrix runner into DevBridge

- Move `matrix.py` rig, transport, tunnels, lease/recovery, cell journal,
  resume, and report logic into DevBridge's broker package with CLI commands
  `devices`, `doctor`, `suite list`, `suite run`, `suite resume`, and `report`.
- Migrate spruce `adhoc`, `arm`, `launch`, `play`, and `collect` operations to
  broker fixtures/adapters. Keep shell helpers only for unavoidable on-device
  work and call them through typed broker operations with checked results.
- Run the old NextUI matrix and the new broker against one identical cell;
  compare screenshots, logs, and verdict before retiring the old path.

Gate: a two-cell suite (FC and GBC, both protocols) runs from idle to idle
without manual steps, survives one injected tunnel drop, and resumes after
an interrupted attempt without duplicate or lost results.

### Phase 3: device protocol and Netplay telemetry

- Add request IDs, completion states, typed `WAIT_STATE`, structured status,
  event cursors, and capture bundles to `DEVBRIDGE/2`; preserve `/1` command
  behavior until devices are upgraded.
- Add Netplay's local session-status contract and DS emulator adapters. Test
  disconnected peer, crashed game, delayed launch, and stale PID cases with
  small simulated agents before device deployment.
- Normalize FPS, slowest-window rate, longest freeze, checks, state resends,
  disconnects, and capture coverage into one telemetry schema.
- Add network RTT distributions and collector overhead metadata; pass the
  measurement-integrity gate on light and heavy cores before using this profile
  for protocol performance claims.

Gate: the broker classifies a deliberate 20-second disconnect as product
failure with the exact interval, even if the game continues solo; it reports
missing frame data as `NO_DATA` without generating an FPS figure.

### Phase 4: robust game tests and full matrix

- Replace fixed menu sleeps with existing VERIFY variants, bounded branch
  logic where necessary, and platform-specific approved references.
- Record proper spruce DS references and add readiness checkpoints before
  DS menu input. Parameterize DSperate/DraStic host and guest choices.
- Add the requested core and protocol matrix, each cell with declared
  eligibility. Include Gambatte cable and paired modes, gpSP and mGBA,
  Snes9x and Supafaust, all requested DS pairings, and PSX.
- Run the full suite with no manual intervention and publish the broker's
  generated comparison with passes, failures, unsupported cells, retries,
  and missing cells visible.

Gate: every planned cell has a verdict and artifact link; a reviewer can
replay the exact device sequence and explain every failed assertion from
the recorded evidence. The devices finish on their original networks and
settings.

## Implementation discipline

Develop the broker and protocol in DevBridge.pak, with the suite manifests
and game-specific tests next to the Netplay tests until ownership is settled.
Use a simulated device/protocol fixture for state-machine and failure-path
tests, then a small physical smoke suite. Avoid a full hours-long device run
for each broker edit. Keep the existing `matrix.py` and spruce scripts usable
until the new broker passes their comparison gates.
