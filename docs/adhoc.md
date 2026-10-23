# Ad hoc networking

How Netplay.pak puts two handhelds on their own network, what the radios in
these devices actually allow, and the failure modes that cost the most time to
find. Everything here was measured on a Miyoo A30 (`my282`) and a Trimui Brick
(`tg5040`); nothing is inferred from documentation.

## Why bother

Shared-screen netplay is lockstep: a frame advances only when both sides have
each other's input for it. That makes it a latency problem, not a bandwidth one,
and the difference between routing through an access point and going direct is
not marginal.

Round-trip time between the two handhelds, 40 pings each:

| path | min | avg | max |
|---|---|---|---|
| through the access point (2 hops) | 6.2–6.5 ms | 26–43 ms | **118–300 ms** |
| ad hoc, device to device (1 hop) | 1.7 ms | **4.3 ms** | **21.5 ms** |

What that does to a session, same game, same devices:

| | via access point | ad hoc |
|---|---|---|
| frame rate | 18.0 fps | 58.4–60.2 fps |
| stalled frames | 79–85 % | 0–10 % |
| longest stall | 138 frames (2.3 s) | 14–23 frames |
| TCP connect | 12.9 s | 78 ms |

The arithmetic ties out. `input_delay` frames of buffer is `delay × 16.7 ms`;
once RTT exceeds that window the pipeline drains every frame and you degrade to
one round trip per frame. At 43 ms that predicts ~23 fps and 18.0 was observed.

**Confirmed in play.** Over ad hoc at `input_delay=3`, real sessions run at
59.0–60.0 fps with 0–1 % stalled frames, on both NES and Mega Drive, with
several intervals reporting literally zero stalls:

```
pacing: 600 frames in 10000ms (60.0 fps), stalled 0 frames (0%), longest 0, delay 3
```

The visible stuttering that made earlier sessions unplayable is gone. Note that
this is a *transport* result only - it says nothing about whether the two
emulators agree, which is a separate problem entirely and is covered in
[state-sync.md](state-sync.md).

This is why `NS_INPUT_DELAY_ADHOC` is 3 and `NS_INPUT_DELAY_WIFI` is 10. A
single constant tuned for one transport is unusable on the other — 3 frames over
the access point measured 33 fps and 74 % stalls, *worse* than the 6 it
replaced.

Wi-Fi power saving is not a lever here. Alternating A/B, three paired rounds:
`off` averaged 26.3 ms and `on` 33.9 ms, but the tails invert and neither brings
the worst case under the input-delay window. It is left off because it wins on
average, not because it fixes anything.

## What the hardware actually is

|  | A30 (`my282`) | Brick (`tg5040`) |
|---|---|---|
| wireless interfaces | `wlan0` only | `wlan0` + `wlan1` |
| AP-capable second interface | **no** | yes (`wlan1`) |
| `hostapd` | yes | yes |
| `hostapd_cli` | **no** | **no** |
| `udhcpd` (DHCP server) | **no** | yes |
| `udhcpc` (DHCP client) | yes | yes |
| `wpa_supplicant` / `wpa_cli` / `iw` | yes | yes |
| `/etc/wifi/wifi_init.sh` | **absent** | present |
| `nohup`, `setsid` | **no** | **no** |
| `base64` | **no** | yes |
| `scp` / `sftp-server` | **no** | **no** |
| `nc` | connect-only (no `-l`) | connect-only (no `-l`) |

Both run BusyBox 1.27.2 and hostapd 2.6.

**Hosting is a capability, not a device.** It currently requires two things: an
AP-capable interface, and `udhcpd` to hand out addresses. The A30 has neither,
so on that pair the Brick hosts and the A30 joins - but that is a measurement,
not a rule. `NS_canHostAdhoc()` probes for the interface and `NS_hotspotStart`
checks for both, refusing with a specific reason rather than half-working, and
the UI omits the option entirely where it cannot succeed.

This matters because the pak targets my282, tg5040, tg5050, my355 and h700, and
only two of those had been measured - and note that the DHCP requirement is
removable: the client already knows the host is at `10.0.0.1`, so a static
`10.0.0.2` would drop `udhcpd` from the design and widen hosting to any device
that can raise an AP. Not yet done.

### The client stack differs on every platform

Building the client side from those two samples is what broke the H700. Every
association check went through `iw` and every DHCP request through `udhcpc`, and
the H700's Ubuntu rootfs has neither. Nothing failed loudly: `iw dev wlan0 link`
on a device with no `iw` prints nothing and exits non-zero, which is exactly
what "not associated" looks like. The join loop ran its full 5 x 11s and gave
up; the restore path then "restored" WiFi into a state with no DHCP client at
all, kept its recovery breadcrumb, and re-ran the same teardown on every
subsequent launch - so WiFi needed a reboot after each session.

`launcher/wifi-platform.sh` now owns this, as a table rather than a probe. The
app reaches the same implementations through it rather than carrying a second
copy that can drift.

| | tg5040 / tg5050 | my282 | my355 | h700 |
|---|---|---|---|---|
| association | `iw` | `iw` | auto | **`wpa_cli status`** |
| DHCP client | `udhcpc` | `udhcpc -s <script>` | **`dhcpcd`** | **`dhclient`** |
| `wifi_init.sh` | `/etc/wifi/` | `$SYSTEM_PATH/etc/wifi/` | `$SYSTEM_PATH/etc/wifi/` | `$SYSTEM_PATH/etc/wifi/` |
| supplicant ctrl dir | `/etc/wifi/sockets` | `/tmp/nextui-wifi` | `/var/run/wpa_supplicant` | `/tmp/wifi/sockets` |
| ctrl dir spelling | `-O<dir>` joined | `-c <conf>` | `-O <dir>` | **`-C <dir>`** |
| DHCP driven by | the pak | the pak | the pak | **`wpa_cli -a` hook** |

Three consequences worth stating on their own:

- **Hardcoding `/etc/wifi/wifi_init.sh` made that recovery route dead code on
  three of the five platforms.** It is the only one of the four that keeps its
  script outside `SYSTEM_PATH`.
- **`udhcpc` exists on the Flip but must not be used there.** my355 runs
  `dhcpcd`, and its own bring-up carries a comment about the two fighting over
  `wlan0`'s address. Selection is by platform, not by what is first on `PATH`.
- **H700 drives DHCP entirely from a `wpa_cli -a` action script.** Replaying the
  supplicant without that hook leaves an interface that associates and never
  gets an address again - for every future reassociation, not only ours. The
  hook is captured alongside the supplicant command line and replayed with it.

Anything unverified stays `auto` in that table, which degrades to the old
probe-and-hope behaviour. That is the right default for an unmeasured platform,
and it is visible there as a gap rather than hidden as an assumption.
`testing/test-wifi-platform.sh` exercises each branch against stub rootfs trees
containing only the tools that platform actually has.

### The driver constraint that shapes everything

The Brick's radio advertises:

```
valid interface combinations:
  * #{ managed } <= 2, #{ AP } <= 1, total <= 3, #channels <= 1
```

Two things follow, and both matter:

1. **AP and station can run at once.** The host serves the ad hoc network on
   `wlan1` while staying associated to the house network on `wlan0`. Discovery
   keeps working, the host stays reachable, and only one device has to move.
2. **`#channels <= 1`.** Every interface on that radio shares one channel. The
   AP *must* use whatever channel `wlan0` is already on. A hardcoded channel is
   rejected outright the moment it disagrees.

`station_channel()` reads it at runtime from `iw dev wlan0 link` (`freq` →
channel). There is no picking a quiet channel while `wlan0` is associated.

**When not associated, there is.** `quiet_channel()` surveys the band and takes
the least congested, weighting each candidate by the traffic on it and the two
channels either side, since a 20MHz AP splatters that far. A survey found
channel 1 completely clear while channel 7 - where the house network sits - had
5 APs directly on it and 10 overlapping. So an independently hosted network can
start somewhere quiet; a concurrent one cannot.

The A30's driver does not advertise interface combinations at all.

## How the host brings the AP up

`NS_hotspotStart`, in order, and every step earns its place:

1. **If we are already serving this SSID, stop.** Return success and change
   nothing. Tearing down a working AP to rebuild it is what stranded a client
   that had already joined.
2. **Delete stale monitor interfaces** (`mon.*`) — see the failure catalogue.
3. **Pick the AP interface** by name *and* type: not `wlan0`, not named `mon.*`,
   not of type `monitor`.
4. **Refuse early** if there is no AP interface or no `udhcpd`.
5. **Reset the interface**: `ip link set <if> down`, `ip addr flush`,
   `iw dev <if> set type managed`. Without this a second session fails.
6. **Write the config** with `channel=` from `station_channel()`.
7. **Start hostapd in the background and wait for `AP-ENABLED`** in its log —
   never `-B`, whose exit status only tells you it forked.
8. **Add `10.0.0.1/24` after** the AP is up; hostapd owns the interface once it
   starts.
9. **Start `udhcpd`** (serving `10.0.0.20`–`10.0.0.40`) and verify it is
   running.

On failure it logs hostapd's own output and the app falls back to hosting on the
existing network rather than refusing to host at all.

The SSID is `nextui-XXXX`, where the four characters are generated once per
session and shown on the armed main screen; the passphrase is fixed
(`playwithme`).

That shape is deliberate. Credentials used to be generated *and advertised over
the existing network*, which coupled ad hoc to discovery having worked first -
the chicken-and-egg the feature exists to avoid. A known prefix plus a visible
code decouples them completely: the client scans for `nextui-*`, lists what it
finds, and the user matches the code shown on the host. Nothing has to be
exchanged beforehand, and several pairs can operate in one room. The host's LAN
announcements still carry the SSID and passphrase, every 500 ms, so a client on
the same network sees the host there too; the Join list merges the two.

## How the client joins

`NS_hotspotJoin` moves `wlan0` onto the ad hoc network. Five attempts, three
seconds apart, each one bounded — eight seconds to associate, then DHCP — so
five failures cannot take minutes with nothing on screen. It fires a progress
callback about once a second so the UI can animate rather than look hung.

Two details that are not obvious:

- **`iw dev wlan0 link` reports the SSID as soon as it associates**, which is
  *before* the 4-way handshake completes. A DHCP request sent in that gap is
  silently dropped. There is a deliberate settle before `udhcpc`.
- **Associated is not reachable.** A station can hold the radio link and have no
  address. The host's client list reports the address where it knows one and
  says `(no address yet)` otherwise, rather than implying a working peer.

### The join does not survive leaving the app

This is the part that is easy to get wrong. The app joins when you pick a peer,
but the game launches later from the NextUI menu, and **the platform
re-establishes its own Wi-Fi in between**. By the time a core starts, the device
is back on the house network with the session still pointing at `10.0.0.1`.

So the join is recorded, not assumed. `NS_arm` writes `adhoc_ssid` / `adhoc_psk`
into the session file, and `launcher/adhoc-join.sh` — called from
`launch-stub.sh` on every game launch — re-joins if we are not already on that
SSID. It is a no-op for non-ad-hoc sessions and a no-op when already associated.

This is the same shape as Wi-Fi power saving, which also comes back on
reassociation and is also re-applied per launch. Anything the platform owns has
to be re-asserted at launch time, not just at arm time.

## Teardown and recovery

Host and client undo different things, and doing the wrong one takes a device
off the network for no reason:

- **Host**: kill `hostapd`/`udhcpd`, delete monitor interfaces, flush and down
  the AP interface, set it back to `type managed`. The client stack is never
  touched — the host never left the house network.
- **Client**: restore the platform's own supplicant.

`NS_hotspotStop` does nothing at all unless a hotspot is actually running
(checked via the in-process flag *or* a live `hostapd`/`udhcpd`, so one leaked by
an earlier run is still cleanable).

`udhcpd` ignores `SIGTERM` on these images. Teardown sends `TERM` then `KILL`.

**`hostapd` must be given time to die.** `killall` immediately followed by
`killall -9` froze the Brick hard enough to need several power cycles. A station
was associated at the time, and killing hostapd before it can deauthenticate
leaves the driver holding an AP that no longer has a userspace owner. Teardown
now sends `TERM`, polls `pidof` ten times at 500 ms, and escalates to `KILL`
only if that fails — which in practice it does not. The general shape: on a
single shared radio, *how* a process holding the interface exits matters, not
just that it exited.

### Getting back

The client's route home is the platform's own `wpa_supplicant` command line,
captured from `/proc/*/cmdline` **before** killing it, and matched on `argv[0]`
only. Guessing this is not viable: the original code assumed
`/etc/wifi/wifi_init.sh`, which exists on tg5040 and does not exist on my282, so
tearing the stack down there left the device with nothing that knew how to
undo it.

The captured command is persisted to `.userdata/shared/Netplay/wifi_restore`. Holding it only in
memory meant it vanished the moment the app exited — which is exactly when a
device is stranded and needs it.

Four ways back:

1. **Automatic, at app launch.** `NS_wifiRecoverIfStranded()` restores if the
   radio is associated to nothing, or is on a generated `nextui-*` network with no session
   armed. A healthy connection to some other network clears the stale record.
2. **On join failure**, which restores before returning.
3. **In the detached watchdog**, after three consecutive failures to reach the
   ad-hoc host. A failed restore retains the breadcrumb and is retried; it must
   not erase the only route home.
4. **Manually**, via *Tools → Restore WiFi*, for when neither automatic route
   has completed.

Restoring waits for association before requesting a lease rather than firing
DHCP into a link that is not up yet.

#### The restore deadline

Restoring is one loop against one deadline — 25 s, redrawn once a second —
polling for the thing actually wanted, an address on `wlan0`. Not a budget per
step. An earlier attempt split it into 8 s for association and 8 s for DHCP,
which is wrong in both directions: if association used its whole allowance the
DHCP phase then ran against a link that did not exist, and a device that
associated at 9 s was declared failed with 8 unused seconds sitting in the next
phase.

**25 s because that is what the hardware needs.** After a supplicant restart the
my282 can take past 20 s to associate — the watchdog logged `not associated to
anything` at its 20 s check and was associated by the next one. The original
code accommodated this entirely by accident: it blocked on `udhcpc -t 8`, which
sat for ~24 s and incidentally gave association that long to finish. Shortening
the DHCP call removed the accident and exposed the real requirement. Anything
tuned against this path should assume tens of seconds, not single digits.

DHCP runs backgrounded and is polled, re-asked every 8 s while unanswered. On
timeout `udhcpc` is deliberately **left running**: it is the one process that
might still finish the job, and the caller reports failure honestly rather than
killing it and claiming success.

On platforms without `wifi_init.sh`, the saved supplicant is restarted once at
10 s if it is still unassociated. This covers a measured A30 failure where the
first daemon disappeared during the radio transition but the watchdog's later
replay of the identical command succeeded. The ending-session UI names the
**original Wi-Fi** throughout; if the foreground deadline expires it says that
recovery is continuing rather than looking like a reconnect to ad hoc.

**The breadcrumb is removed only on success.** `NS_wifiRecoverIfStranded` and
`NS_wifiRestore` both used to remove the shared `wifi_restore` record unconditionally after
restoring. A restore that timed out therefore deleted the sole record telling the
watchdog and the next app launch that a restore was still owed — permanent
stranding, arriving exactly when recovery matters most. Whatever else changes
here, that record must outlive every failed attempt.

## Nothing invoked from a screen may be unbounded

This produced four separate "the device is frozen" reports before it was
recognised as one defect rather than four, so it is stated here as a rule:

> Any call made from the UI thread, or while holding a lock, must carry a
> deadline. Fast is not the same as bounded, and only bounded matters.

The instances, all real:

| Call | Presented as | Worst case |
|---|---|---|
| `write_all` spinning on `EAGAIN` under the netlink mutex | emulator hangs mid-game | unbounded |
| blocking `connect()` to a peer whose network is gone | black screen on leaving a game | ~2 min (kernel SYN timeout) |
| `iw dev wlan0 scan` via `popen` on the Join screen | app frozen, no buttons respond | unbounded |
| blocking `udhcpc -t 8` while ending a session | app frozen until it finished | ~24 s |

The third is the clearest illustration of why "fast" is the wrong test. That
scan was *measured* at ~2 s and treated as safe; the freeze happened when one
invocation simply never returned. A fresh scan run over ssh at the same moment
completed in 2 s, so the radio was fine — the single call was not. Bounding it
is the fix; measuring it faster never would have been.

Tooling notes that follow from this:

- `timeout(1)` does not exist on these images. Deadlines are enforced in C
  (`popen_bounded`, non-blocking pipe plus a clock) or by backgrounding the
  command and polling for its observable effect.
- **Prefer polling for the effect over waiting on the process.** Whether
  `udhcpc` exited is far less interesting than whether `wlan0` has an address,
  and the second can be checked on our own schedule.
- Busybox retry flags multiply into wall-clock time. `udhcpc -t N` sends N
  discover packets separated by `-T` seconds, defaulting to 3 — so `-t 8` is
  ~24 s, not "8 quick tries". Check `--help` on the device rather than assuming
  the GNU meaning.

## Never let cached UI state describe live hardware

The app polls Wi-Fi every 5 s into `wifi_ssid`/`wifi_up` and draws text from
that, while the signal icon comes from NextUI's live `PLAT_wifiConnection`. After
any operation that moves the radio, the cache is stale by definition, and the two
disagree on screen: a status line reporting the network you just left beside an
icon correctly showing nothing. It reads as "it claims to be connected but
isn't". Every path that changes the radio now forces a refresh before drawing a
conclusion about it.

## Failure catalogue

Every one of these was hit for real. Symptoms first, because that is how they
present.

| Symptom | Cause | Fix |
|---|---|---|
| AP never comes up; `wlan0` stays `type managed`; no client can associate | hostapd pointed at `wlan0` while `wlan0` is an associated station | Run the AP on `wlan1` |
| `Could not read interface mon.wlan1 flags: No such device`; a working AP is destroyed by a second Host | interface picker took the first device that was not `wlan0`, and `iw dev` lists hostapd's leftover monitor vif **first** | Match on name *and* type; skip `mon.*` and `type monitor`; make `NS_hotspotStart` idempotent |
| Client joins successfully, then the device is on no network at all and cannot recover | a second Host attempt killed the AP the client was associated to | Never tear down an AP that is already serving the right SSID |
| Signal icon and SSID blank while ad hoc is up and passing traffic | the join replaced the platform supplicant with one on a different control socket, so the frontend's `wpa_cli -p <dir>` had nothing to talk to | Discover the platform's `ctrl_interface` (`-O`, else the `-c` config) and reuse it |
| Host shows no connected clients while a client is demonstrably associated | the UI depended on process-local AP state and only redrew on a keypress | Have the broker publish a reconstructed guest table and poll its atomic status once a second |
| `Could not set channel for kernel driver` / `Interface initialization failed` | Config channel disagrees with `wlan0`'s, violating `#channels <= 1` | Read the channel at runtime |
| Same error, but only on the **second** session | Killing hostapd leaves the interface `type AP` with SSID and channel still claimed | Reset before start: delete `mon.*`, link down, `set type managed` |
| `Could not read interface mon.wlan1 flags: No such device` | `iw dev` lists hostapd's leftover monitor vif **first**, so "first interface that is not wlan0" picked `mon.wlan1` | Match on name *and* type; skip `mon.*` and `type monitor` |
| hostapd "starts" but nothing works | `hostapd -B`'s exit status only means it forked | Run in background, wait for `AP-ENABLED` |
| Client associates, then reports no address | `udhcpd` missing (A30) and its return value unchecked | Check for the binary up front and refuse with a reason |
| Client associates, DHCP times out intermittently | Request sent between association and the 4-way handshake | Settle before `udhcpc` |
| Client joins, then is back on the house network by the time the game runs | The platform re-establishes its Wi-Fi when the app exits | Re-join from the launch stub |
| Device left with no network and no way back | Restore assumed `wifi_init.sh`, absent on my282 | Capture and replay the real supplicant command |
| Restore replays garbage | The `/proc` scan matched `wpa_supplicant` anywhere in the command line — including the shell running the scan | Match `argv[0]` only |
| Device stranded after the app exits | The captured command lived only in memory | Persist to `.userdata/shared/Netplay/wifi_restore` |
| Client stranded on a network that no longer exists | Host tore down a working AP to rebuild it, and the rebuild failed | Make `NS_hotspotStart` idempotent |
| Turn off takes the device off Wi-Fi entirely | `NS_hotspotStop` ran unconditionally, tearing down the client stack even when no hotspot existed | No-op unless something is actually running |
| House network degrades, SSH times out mid-command | An AP left running shares one radio and one channel with the client interface | Tear the AP down when hosting without ad hoc, and on End session / Turn off |
| Brick freezes hard on End session; needs several power cycles | `killall` then `killall -9` on hostapd with a station still associated, leaving the driver an ownerless AP | `TERM`, poll `pidof` 10x500ms, escalate only if needed |
| App frozen on the Join screen, no buttons respond | `iw dev wlan0 scan` via `popen` on the UI thread never returned | Read `wpa_cli scan_results` (cached, 0s) and bound every shell-out |
| Ending a session freezes the UI for ~30s | blocking `udhcpc -t 8` = 8 packets x 3s, on the UI thread with no repaint | Background DHCP, poll for the address, animate to a deadline |
| Session ends claiming connected, but no signal icon; reopening the pak fixes it | status text drawn from the 5s cache, icon drawn live; the restore had not finished | Force a poll after teardown; report "still reconnecting" honestly |
| A restore that timed out left the device stranded with nothing to recover from | `remove(wifi_restore)` ran unconditionally, deleting the record the watchdog needs | Remove the breadcrumb only on confirmed success |

## Deliberately not done

**Channel selection.** The Brick can scan (`iw dev wlan0 scan` returns real
results; a survey found channel 1 completely clear and channel 7 — where the
house network lives — the most congested, 5 APs direct and 10 overlapping). It
does not help: `#channels <= 1` forces the AP onto the station's channel while
`wlan0` is associated, and ad hoc measured 4.3 ms average *on the congested
channel*. There is no headroom left worth chasing. This only becomes relevant if
the host drops the house network entirely.

**Channel switch announcement.** 802.11 CSA is the right mechanism and hostapd
2.6 supports `chan_switch`; `ctrl_interface=/var/run/hostapd` is already in the
config for it. Three problems: `hostapd_cli` is not on either device, driver CSA
support is unadvertised and unverified, and — decisively — the single-channel
constraint means the AP cannot leave `wlan0`'s channel while `wlan0` is
associated. CSA only becomes meaningful after dropping the house network, which
is the thing concurrency exists to avoid.

## Working on this

The ad hoc path used to report only through on-screen status, so a failure left
nothing to read and every diagnosis started from scratch. It now logs to the
pak's log via `ns_log()`:

```
[netplay-app] starting AP on wlan1 channel 7 (station is on 7)
[netplay-app] channel survey: picked 1 (overlap score 0)
[netplay-app] AP up on wlan1 as nextui-K7QM, serving 10.0.0.1
[netplay-app] scan found 2 ad hoc network(s)
```

and on failure hostapd's own lines are echoed in alongside. The client side logs
`[netplay-adhoc]` from the launch stub. **Add logging before adding behaviour
here** — the interesting failures are all invisible otherwise.

Getting files onto these devices is its own problem: no `scp`, no
`sftp-server`, no `base64` on the A30, `nc` is connect-only, and command-line
stuffing caps out around 8 KB. The route that works is a single
password-authenticated ssh `ControlMaster` per device, then `cat > file` through
it — binary-safe and needs nothing on the far end.

Note the A30 has neither `nohup` nor `setsid`; to run something that must
survive the ssh session dropping (anything that touches `wlan0`), use
`( cmd </dev/null >/dev/null 2>&1 & )`.

## The menu

```
Netplay          Host                        Tools                         Settings
  Host      ->     Create ad hoc network       Settings              ->     Use compatibility cores: Yes
  Join             Host over WiFi              Debug                 ->     Use instanced cores: N  >
  Tools                                       Restore WiFi                  Add Netplay to GameSwitcher: No
                                              Turn off Netplay               Disable Wi-Fi Power Saving: No
                                              (Remove Bindings)

                                                                         Debug
                                                                           Force compatibility cores: No
                                                                           Verbose debugging logs: Yes
                                                                           Run checks
```

Once armed, Host and Join are replaced by durable session status:

```
Host                                      Guest
  Hosting: nextui-XXXX                      Joined: nextui-XXXX
  Connected guests:                        Connected to host: 10.0.0.1
    10.0.0.20  aa:bb:cc:dd:ee:ff            Tools
  Tools
```

The detached broker keeps hosting discoverable after Netplay.pak exits. X ends
the session and stops the broker; until then Host and Join stay hidden.

`X` ends a session from any screen; the hint only appears when one is armed.

Submenus are not decoration. The flat menu reached seven entries and pushed the
WiFi status line off the bottom with no way to scroll to it - and that line is
exactly how you tell an ad hoc session from one that quietly fell back. Short,
focused screens keep it visible.

*Create ad hoc network* is absent on devices without an AP-capable interface,
with the reason shown in its place.

Settings are toggles drawn in place (`Name: value`) rather than destinations - a
submenu per boolean would be three presses to flip one flag. *Instanced cores*
is the exception: its third state opens a picker over the cores that can be
instanced (gambatte and mgba), marking any not installed rather than offering
something that cannot run. Debug-only toggles and checks live in the separate
*Debug* submenu. Settings are stored in `.userdata/shared/Netplay/settings`, written with
write-then-rename so a power cut cannot leave a half-written file that silently
reads back as defaults.

Every screen must handle its own back button - there is no global one. The
checks screen shipped without an input case at all, so it drew "B BACK" while
nothing read B and the only way out was a power cycle. When adding a screen,
check that the render switch and the input switch have the same number of
cases.

## Status

Working end to end, and validated in play. The Brick hosts on `wlan1` while
staying on the house network, the A30 joins, and the launch stub re-joins per
game because the platform reclaims the radio when the app exits.

Two things it is worth knowing are true but easy to misread as failures:

- **The A30 disappearing from the house network is the success case.** It is on
  the ad hoc network and therefore unreachable from anywhere else - including
  ssh. Check for it in the host's `iw dev wlan1 station dump`, not by pinging it.
- **A missing signal icon does not mean the radio is down.** It has meant two
  different things now: the frontend could not see the supplicant, and later,
  the restore had not finished while cached text claimed otherwise. Both fixed;
  the general lesson holds either way — check `iw`, not the UI.

**Ad hoc no longer requires a pre-existing shared network.** Finding a host is
two independent mechanisms, shown as one list:

- announcements heard over UDP broadcast, if both devices are already on a
  network together;
- an SSID scan for the `nextui-` prefix, which finds a host that has already
  moved to its own network and cannot be heard any other way.

Neither alone is sufficient and the second removes the bootstrap problem, so the
"agree over WiFi, then create ad hoc" sequence is no longer needed. The client
keeps its place on the existing network until it commits to joining, and ~20
SSIDs are visible from the A30 while associated.

If both mechanisms see the same host, the rows are merged without losing the
fact that the SSID scan succeeded. The resulting row is labelled with the
`nextui-XXXX` SSID, and the result count is the number actually found by the
scan—not the number remaining after duplicate removal.

The scan reads `wpa_cli -p <ctrl> -i wlan0 scan_results` - the supplicant's
cached results, which return in **0 s** - and only falls back to `iw dev wlan0
scan` where no control socket can be found. This is the same mechanism the
frontend itself uses, so the cache stays warm. An `iw` scan was measured at ~2 s,
but that number is worth distrusting: treating it as safe is exactly what froze
the Join screen, because one such scan simply never returned. Both paths are
bounded regardless.

Still unproven: hosting from a device with no AP-capable interface. The UI omits
the option there rather than offering something that must fail, and
`NS_canHostAdhoc()` decides that by probing rather than by platform name.
