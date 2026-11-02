# DevBridge test harness

`tools/netplay-harness.py` is the host-side control plane for repeatable tests
on two handhelds. It consumes the DevBridge discovery relay, uses SSH/SFTP for
device work, gives both roles one session identifier, deploys known artifacts
with rollback copies, and collects correlated logs.

It intentionally does not kill NextUI or start an emulator behind its back.
`launch-plan` prints the exact commands, but automated execution remains gated
until the harness can ask NextUI to yield display, audio and input ownership.
Launching a second SDL application over the live menu would not be a valid
emulator or netplay test.

## Authentication

Use an installed SSH key with `--identity`. For stock firmware password
authentication, keep the password in the process environment rather than a
command-line argument:

```sh
export DEVBRIDGE_PASSWORD=tina
```

The harness makes a mode-0700 temporary askpass helper which reads that
variable; neither the helper nor the command line contains the password.

## Discovery and readiness

With the Windows relay running, discovery receives its unicast copies:

```sh
python3 tools/netplay-harness.py discover --count 2
python3 tools/netplay-harness.py doctor \
  --host 192.168.0.180 --client 192.168.0.144
```

Add `--multicast` only when the harness itself is directly on the LAN.

## Ad-hoc SSH continuity

Every command that contacts the handhelds resolves its control route first.
While both devices are on the house LAN it connects to each directly. When the
host broker reports a guest in `10.0.0.0/24`, the harness verifies and then
uses the host as an SSH jump endpoint:

```text
WSL -> host LAN address -> guest ad-hoc address:22
```

The host stays directly reachable because Netplay leaves its station interface
on the house LAN while a second interface serves `10.0.0.1/24`. The guest
address comes from `.userdata/shared/Netplay/broker.status`, with `/proc/net/arp` as a fallback.
No IP forwarding, subnet route, or relayed guest beacon is required.

Inspect the selected path at any time:

```sh
python3 tools/netplay-harness.py route \
  --host 192.168.0.180 --client 192.168.0.144

# During transition, wait until the guest is reachable through the host:
python3 tools/netplay-harness.py route \
  --host 192.168.0.180 --client 192.168.0.144 \
  --require-jump --wait 30
```

`--guest-ip 10.0.0.20` forces a known ad-hoc address for diagnostics;
`--no-auto-jump` forces the ordinary LAN route. The same resolution is used by
`doctor`, `session`, `deploy`, and `collect`, including their SCP transfers.

## Correlated sessions

Preview both files without changing either device:

```sh
python3 tools/netplay-harness.py session \
  --host 192.168.0.180 --client 192.168.0.144 --dry-run
```

Arm both devices with the same session ID:

```sh
python3 tools/netplay-harness.py session \
  --host 192.168.0.180 --client 192.168.0.144 --mode link
```

The command prints the ID. Existing `session.conf` and shared `Netplay/session` files
are copied to `.userdata/shared/Netplay/netplay-harness/session-backups/` before replacement. Extra experimental
keys can be supplied with repeatable `--set key=value` options.

## Artifact deployment

Known destinations keep development builds from landing in the wrong pak:

```sh
python3 tools/netplay-harness.py deploy \
  --host 192.168.0.180 --client 192.168.0.144 \
  --artifact mgba-core --file /path/to/mgba_libretro.so

python3 tools/netplay-harness.py deploy \
  --host 192.168.0.180 --client 192.168.0.144 \
  --artifact drastic-wifi --file /path/to/libdrastic_wifi.so
```

The old file is backed up below
`.userdata/shared/Netplay/netplay-harness/backups/<timestamp>/`, replacement is an
atomic rename, and the remote SHA-256 must equal the local digest. Available
artifact names are shown by `deploy --help`.

## Launch planning and collection

```sh
python3 tools/netplay-harness.py launch-plan \
  --host 192.168.0.180 --client 192.168.0.144 --engine mgba \
  --host-rom '/mnt/SDCARD/Roms/Game Boy Advance (MGBA)/game.gba' \
  --client-rom '/mnt/SDCARD/Roms/Game Boy Advance (MGBA)/game.gba'

python3 tools/netplay-harness.py collect \
  --host 192.168.0.180 --client 192.168.0.144 \
  --session-id 0123456789abcdef0123456789abcdef
```

Collection writes `testing/results/<session-id>/manifest.json`, separates host
and client files, and includes immutable Netplay session logs plus mGBA,
DraStic Wi-Fi, and broker logs when present.
