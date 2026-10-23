#!/usr/bin/env python3
"""DevBridge-powered two-device development harness for Netplay.pak."""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid

BEACON_GROUP = "239.255.42.99"
BEACON_PORT = 42099
DEFAULT_PLATFORM = "tg5040"
NETPLAY_PORT = 55437
ADHOC_NETWORK = ipaddress.ip_network("10.0.0.0/24")

ARTIFACTS = {
    "mgba-core": ("Emus/{platform}/MGBA.pak/mgba_libretro.so", "0644"),
    "mgba-dual": ("Emus/{platform}/MGBA.pak/mgba_dual_libretro.so", "0644"),
    "mgba-launcher": ("Emus/{platform}/MGBA.pak/launch.sh", "0755"),
    "mgba-shim": ("Emus/{platform}/MGBA.pak/netplay_shim.{platform}.so", "0644"),
    "drastic": ("Emus/{platform}/NDS.pak/drastic/drastic", "0755"),
    "drastic-wifi": ("Emus/{platform}/NDS.pak/drastic/libs/libdrastic_wifi.so", "0644"),
    "drastic-adapter": ("Emus/{platform}/NDS.pak/drastic/libs/libadvdrastic.so", "0644"),
    "netplay-shim": ("Tools/{platform}/Netplay.pak/bin/{platform}/netplay_shim.so", "0755"),
    "netplay-app": ("Tools/{platform}/Netplay.pak/bin/{platform}/netplay.elf", "0755"),
    "netplay-launcher": ("Tools/{platform}/Netplay.pak/launcher/minarch.elf", "0755"),
}

ENGINE_LAUNCHERS = {
    "mgba": "Emus/{platform}/MGBA.pak/launch.sh",
    "drastic": "Emus/{platform}/NDS.pak/launch.sh",
}


class HarnessError(RuntimeError):
    pass


def parse_beacon(data: bytes) -> dict[str, str] | None:
    try:
        text = data.decode("utf-8").strip()
    except UnicodeDecodeError:
        return None
    if not text.startswith("devbridge "):
        return None
    fields = {}
    for item in text.split()[1:]:
        key, separator, value = item.partition("=")
        if separator and key:
            fields[key] = value
    return fields if fields.get("ip") else None


def discovery_socket(multicast: bool, interface: str) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", BEACON_PORT))
    if multicast:
        membership = struct.pack(
            "=4s4s", socket.inet_aton(BEACON_GROUP), socket.inet_aton(interface)
        )
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
    return sock


def discover_devices(count: int, timeout: float, multicast: bool, interface: str):
    deadline = time.monotonic() + timeout
    devices = {}
    with discovery_socket(multicast, interface) as sock:
        while len(devices) < count:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            sock.settimeout(remaining)
            try:
                data, source = sock.recvfrom(4096)
            except socket.timeout:
                break
            fields = parse_beacon(data)
            if fields:
                fields.setdefault("source", source[0])
                devices[fields["ip"]] = fields
    return sorted(devices.values(), key=lambda item: item["ip"])


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_from_output(text: str) -> str | None:
    """Extract a digest even when a device's login shell prints a banner."""
    for token in text.split():
        candidate = token.lower()
        if len(candidate) == 64 and all(char in "0123456789abcdef" for char in candidate):
            return candidate
    return None


def validate_address(value: str) -> str:
    try:
        socket.inet_aton(value)
    except OSError as error:
        raise argparse.ArgumentTypeError(f"invalid IPv4 address: {value}") from error
    return value


def validate_guest_address(value: str) -> str:
    value = validate_address(value)
    if not is_adhoc_address(value) or value == "10.0.0.1":
        raise argparse.ArgumentTypeError("ad-hoc guest address must be in 10.0.0.2-10.0.0.254")
    return value


def is_adhoc_address(value: str) -> bool:
    try:
        return ipaddress.ip_address(value) in ADHOC_NETWORK
    except ValueError:
        return False


def guest_address_from_status(text: str) -> str | None:
    """Extract the first usable ad-hoc guest address from broker/ARP output."""
    for line in text.splitlines():
        candidate = line.partition("=")[2] if line.startswith("guest_") else line
        candidate = candidate.strip().split()[0] if candidate.strip() else ""
        if is_adhoc_address(candidate) and candidate != "10.0.0.1":
            return candidate
    return None


def validate_setting(value: str) -> str:
    key, separator, setting = value.partition("=")
    if not separator or not key or any(char.isspace() for char in key + setting):
        raise argparse.ArgumentTypeError("settings must be nonempty key=value tokens")
    if "\n" in value or "\r" in value:
        raise argparse.ArgumentTypeError("settings cannot contain newlines")
    return value


def session_text(role, session_id, host_address, mode, settings):
    lines = [
        f"session_id={session_id}",
        "input_delay=3",
        "input_delay_auto=1",
        "share_cores=0",
        "compatibility_cores=1",
        "verbose_logs=1",
        f"role={role}",
        f"port={NETPLAY_PORT}",
    ]
    if role == "client":
        lines.append(f"peer={host_address}")
    if mode != "auto":
        lines.append(f"mode={mode}")
    lines.extend(settings)
    return "\n".join(lines) + "\n"


def shell_environment(platform: str) -> str:
    values = {
        "SDCARD_PATH": "/mnt/SDCARD",
        "PLATFORM": platform,
        "SYSTEM_PATH": f"/mnt/SDCARD/.system/{platform}",
        "USERDATA_PATH": f"/mnt/SDCARD/.userdata/{platform}",
        "SHARED_USERDATA_PATH": "/mnt/SDCARD/.userdata/shared",
        "BIOS_PATH": "/mnt/SDCARD/Bios",
        "SAVES_PATH": "/mnt/SDCARD/Saves",
        "CHEATS_PATH": "/mnt/SDCARD/Cheats",
        "LOGS_PATH": f"/mnt/SDCARD/.userdata/{platform}/logs",
    }
    return " ".join(f"{key}={shlex.quote(value)}" for key, value in values.items())


def launch_command(engine: str, platform: str, rom: str) -> str:
    launcher = "/mnt/SDCARD/" + ENGINE_LAUNCHERS[engine].format(platform=platform)
    return f"{shell_environment(platform)} {shlex.quote(launcher)} {shlex.quote(rom)}"


class Remote:
    def __init__(self, user, port, identity, password_env, connect_timeout):
        self.user = user
        self.port = port
        self.identity = identity
        self.password_env = password_env
        self.connect_timeout = connect_timeout
        self.operation_timeout = max(30, connect_timeout * 6)
        self._temp = tempfile.TemporaryDirectory(prefix="netplay-harness-")
        self._askpass = None
        if os.environ.get(password_env):
            helper = Path(self._temp.name) / "askpass.sh"
            helper.write_text(
                "#!/bin/sh\n" + f"printf '%s\\n' \"${{{password_env}}}\"\n",
                encoding="utf-8",
            )
            helper.chmod(0o700)
            self._askpass = helper
        config = Path(self._temp.name) / "ssh_config"
        lines = [
            "Host *",
            f"    ConnectTimeout {self.connect_timeout}",
            "    StrictHostKeyChecking no",
            "    UserKnownHostsFile /dev/null",
            "    LogLevel ERROR",
            "    ControlMaster auto",
            f'    ControlPath "{self._temp.name}/ssh-%C"',
            "    ControlPersist 15",
        ]
        if self.identity:
            identity_path = str(self.identity).replace("\\", "\\\\").replace('"', '\\"')
            lines += [f'    IdentityFile "{identity_path}"', "    BatchMode yes"]
        elif self._askpass:
            lines += ["    PreferredAuthentications password", "    PubkeyAuthentication no"]
        config.write_text("\n".join(lines) + "\n", encoding="utf-8")
        config.chmod(0o600)
        self._config = config

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        self._temp.cleanup()

    def _environment(self):
        environment = os.environ.copy()
        if self._askpass:
            environment.update(
                DISPLAY=environment.get("DISPLAY", ":0"),
                SSH_ASKPASS=str(self._askpass),
                SSH_ASKPASS_REQUIRE="force",
            )
        return environment

    def _options(self, jump=None):
        # Host-* config applies to both the destination and the implicit ssh
        # process created by -J. Command-line destination options do not.
        options = ["-F", str(self._config)]
        if jump:
            jump_target = f"{self.user}@{jump}"
            if self.port != 22:
                jump_target += f":{self.port}"
            options += ["-J", jump_target]
        return options

    def run(self, address, command, check=True, jump=None):
        argv = ["ssh", *self._options(jump), "-p", str(self.port),
                f"{self.user}@{address}", f"sh -lc {shlex.quote(command)}"]
        try:
            result = subprocess.run(
                argv, text=True, capture_output=True, check=False,
                env=self._environment(), stdin=subprocess.DEVNULL,
                start_new_session=bool(self._askpass), timeout=self.operation_timeout,
            )
        except subprocess.TimeoutExpired as error:
            route = f" via {jump}" if jump else ""
            raise HarnessError(
                f"{address}{route}: remote command timed out after "
                f"{self.operation_timeout}s"
            ) from error
        if check and result.returncode:
            detail = result.stderr.strip() or result.stdout.strip()
            route = f" via {jump}" if jump else ""
            raise HarnessError(f"{address}{route}: remote command failed: {detail}")
        return result

    def upload(self, address, source, destination, jump=None):
        argv = ["scp", *self._options(jump), "-P", str(self.port), str(source),
                f"{self.user}@{address}:{destination}"]
        try:
            result = subprocess.run(
                argv, text=True, capture_output=True, check=False,
                env=self._environment(), stdin=subprocess.DEVNULL,
                start_new_session=bool(self._askpass), timeout=self.operation_timeout,
            )
        except subprocess.TimeoutExpired as error:
            route = f" via {jump}" if jump else ""
            raise HarnessError(
                f"{address}{route}: upload timed out after {self.operation_timeout}s"
            ) from error
        if result.returncode:
            route = f" via {jump}" if jump else ""
            raise HarnessError(f"{address}{route}: upload failed: {result.stderr.strip()}")

    def download_tree(self, address, source, destination, jump=None):
        destination.mkdir(parents=True, exist_ok=True)
        argv = ["scp", *self._options(jump), "-P", str(self.port), "-r",
                f"{self.user}@{address}:{source}", str(destination)]
        try:
            result = subprocess.run(
                argv, text=True, capture_output=True, check=False,
                env=self._environment(), stdin=subprocess.DEVNULL,
                start_new_session=bool(self._askpass), timeout=self.operation_timeout,
            )
        except subprocess.TimeoutExpired:
            return False
        return result.returncode == 0


def remote_from_args(args):
    return Remote(args.user, args.ssh_port, args.identity,
                  args.password_env, args.connect_timeout)


def require_programs(*programs):
    missing = [program for program in programs if not shutil.which(program)]
    if missing:
        raise HarnessError("missing host programs: " + ", ".join(missing))


def device_addresses(args):
    return [("host", args.host), ("client", args.client)]


def platform_for(args, role):
    """A pair may mix devices, so each role names its own platform directory."""
    specific = getattr(args, f"{role}_platform", None)
    return specific or args.platform


NETPLAY_STATE = "/mnt/SDCARD/.userdata/shared/Netplay"
HARNESS_STATE = f"{NETPLAY_STATE}/netplay-harness"


def host_reported_guest(remote, args):
    command = (
        "if pidof hostapd >/dev/null 2>&1 && "
        "ip -4 addr 2>/dev/null | grep -q '10[.]0[.]0[.]1/24'; then "
        f"cat {shlex.quote(NETPLAY_STATE + '/broker.status')} 2>/dev/null; "
        "awk '$1 ~ /^10[.]0[.]0[.]/ && $1 != \"10.0.0.1\" {print $1; exit}' "
        "/proc/net/arp 2>/dev/null; fi"
    )
    result = remote.run(args.host, command, check=False)
    if result.returncode:
        return None
    return guest_address_from_status(result.stdout)


def resolve_client(remote, args):
    """Return (address, jump) for the guest's currently reachable network."""
    candidate = args.guest_ip
    if not candidate and not args.no_auto_jump:
        candidate = host_reported_guest(remote, args)
    if candidate:
        result = remote.run(candidate, "true", check=False, jump=args.host)
        if result.returncode == 0:
            return candidate, args.host
        if args.guest_ip:
            raise HarnessError(
                f"guest {candidate} is not reachable through host {args.host}"
            )

    result = remote.run(args.client, "true", check=False)
    if result.returncode == 0:
        return args.client, None
    if candidate:
        raise HarnessError(
            f"guest is unreachable at LAN address {args.client} and at "
            f"{candidate} through host {args.host}"
        )
    raise HarnessError(
        f"guest is unreachable at LAN address {args.client}; host broker has "
        "not reported an ad-hoc guest address"
    )


def resolved_devices(remote, args):
    # The host deliberately retains its station/LAN interface while its second
    # interface serves the AP. It is always our stable control endpoint.
    if remote.run(args.host, "true", check=False).returncode:
        raise HarnessError(f"host is unreachable at {args.host}")
    client_address, client_jump = resolve_client(remote, args)
    return [
        ("host", args.host, None),
        ("client", client_address, client_jump),
    ]


def route_label(address, jump):
    return f"jump {jump} -> {address}" if jump else f"direct {address}"


def command_discover(args):
    devices = discover_devices(args.count, args.timeout, args.multicast, args.interface)
    if args.json:
        for device in devices:
            print(json.dumps(device, sort_keys=True))
    else:
        print(f"{'IP':15} {'PORT':5} {'SFTP':5} {'BATTERY':8} HOST")
        for device in devices:
            print(f"{device['ip']:15} {device.get('port', '22'):5} "
                  f"{device.get('sftp', '?'):5} {device.get('battery', '?'):8} "
                  f"{device.get('host', '?')}")
    if len(devices) < args.count:
        print(f"expected {args.count} device(s), received {len(devices)} before timeout",
              file=sys.stderr)
        return 1
    return 0


def probe_script(platform):
    netplay = f"/mnt/SDCARD/Tools/{platform}/Netplay.pak"
    mgba = f"/mnt/SDCARD/Emus/{platform}/MGBA.pak"
    nds = f"/mnt/SDCARD/Emus/{platform}/NDS.pak"
    session = f"{NETPLAY_STATE}/session"
    mgba_log = f"/mnt/SDCARD/.userdata/{platform}/logs/MGBA.txt"
    return "; ".join([
        "printf 'firmware='; cat /etc/version 2>/dev/null || printf unknown; echo",
        "printf 'arch='; uname -m",
        f"test -f {shlex.quote(netplay + '/pak.json')} && echo netplay=yes || echo netplay=no",
        f"test -x {shlex.quote(mgba + '/launch.sh')} && echo mgba=yes || echo mgba=no",
        f"test -f {shlex.quote(mgba + '/mgba_libretro.so')} && echo mgba_core=yes || echo mgba_core=no",
        f"test -f {shlex.quote(mgba + '/mgba_dual_libretro.so')} && echo mgba_dual=yes || echo mgba_dual=no",
        f"test -f {shlex.quote(mgba + f'/netplay_shim.{platform}.so')} && echo mgba_shim=yes || echo mgba_shim=no",
        f"test -x {shlex.quote(netplay + f'/bin/{platform}/netplay.elf')} && echo netplay_app=yes || echo netplay_app=no",
        f"test -f {shlex.quote(netplay + f'/bin/{platform}/netplay_shim.so')} && echo netplay_shim=yes || echo netplay_shim=no",
        f"test -x {shlex.quote(nds + '/launch.sh')} && echo drastic=yes || echo drastic=no",
        f"test -x {shlex.quote(nds + '/drastic/drastic')} && echo drastic_binary=yes || echo drastic_binary=no",
        "netstat -ltn 2>/dev/null | grep -q ':22 ' && echo ssh=yes || echo ssh=no",
        "ps | grep -E '[m]inui-list|[m]inarch|[d]rastic|[n]etplay.elf' >/dev/null && echo ui=busy || echo ui=idle",
        f"for key in session_id role mode instanced_mgba instanced_gambatte; do "
        f"value=$(sed -n \"s/^$key=//p\" {shlex.quote(session)} 2>/dev/null | tail -1); "
        "echo active_$key=${value:-missing}; done",
        f"printf 'mgba_selection='; grep -E 'launcher: (paired mGBA selected|shim=)' "
        f"{shlex.quote(mgba_log)} 2>/dev/null | tail -2 | tr '\\n' '|'; echo",
        "printf 'loaded_mgba='; for pid in $(pidof minarch.elf 2>/dev/null); do "
        "awk '/mgba(_dual)?_libretro[.]so/ {print $6; exit}' /proc/$pid/maps 2>/dev/null; "
        "done | tail -1",
    ])


def command_doctor(args):
    require_programs("ssh")
    failed = False
    with remote_from_args(args) as remote:
        for role, address, jump in resolved_devices(remote, args):
            platform = platform_for(args, role)
            result = remote.run(address, probe_script(platform), check=False, jump=jump)
            print(f"[{role}: {route_label(address, jump)} {platform}]")
            keys = ("firmware=", "arch=", "netplay=", "mgba=", "mgba_core=",
                    "mgba_dual=", "mgba_shim=", "netplay_app=", "netplay_shim=",
                    "drastic=", "drastic_binary=", "ssh=", "ui=", "active_",
                    "mgba_selection=", "loaded_mgba=")
            lines = [line for line in result.stdout.splitlines() if line.startswith(keys)]
            print("\n".join(lines))
            if result.returncode:
                failed = True
                print(result.stderr.rstrip(), file=sys.stderr)
    return int(failed)


def command_route(args):
    require_programs("ssh")
    deadline = time.monotonic() + args.wait
    last_error = None
    with remote_from_args(args) as remote:
        while True:
            try:
                devices = resolved_devices(remote, args)
                if args.require_jump and not devices[1][2]:
                    raise HarnessError(
                        "guest is reachable directly but has not moved to the ad-hoc network"
                    )
                break
            except HarnessError as error:
                last_error = error
                if time.monotonic() >= deadline:
                    raise last_error
                time.sleep(min(1, max(0, deadline - time.monotonic())))
        payload = {
            role: {"address": address, "jump": jump, "transport": "jump" if jump else "direct"}
            for role, address, jump in devices
        }
        if args.json:
            print(json.dumps(payload, sort_keys=True))
        else:
            for role, address, jump in devices:
                print(f"{role}: {route_label(address, jump)}")
    return 0


def atomic_remote_file(remote, address, content, destination, jump=None):
    name = f"netplay-harness-{uuid.uuid4().hex}"
    local = Path(remote._temp.name) / name
    local.write_text(content, encoding="utf-8")
    uploaded = f"/tmp/{name}"
    remote.upload(address, local, uploaded, jump=jump)
    parent = str(Path(destination).parent)
    remote.run(address, f"mkdir -p {shlex.quote(parent)}; "
               f"mv {shlex.quote(uploaded)} {shlex.quote(destination)}", jump=jump)


def command_session(args):
    require_programs("ssh", "scp")
    session_id = args.session_id or uuid.uuid4().hex
    if len(session_id) != 32 or any(c not in "0123456789abcdefABCDEF" for c in session_id):
        raise HarnessError("session id must be exactly 32 hexadecimal characters")
    rendered = {role: session_text(role, session_id, args.host, args.mode, args.setting)
                for role, _ in device_addresses(args)}
    if args.dry_run:
        for role, address in device_addresses(args):
            print(f"[{role} {address}]\n{rendered[role]}", end="")
        return 0
    with remote_from_args(args) as remote:
        for role, address, jump in resolved_devices(remote, args):
            pak = f"/mnt/SDCARD/Tools/{platform_for(args, role)}/Netplay.pak"
            if remote.run(address, f"test -f {shlex.quote(pak + '/pak.json')}",
                          check=False, jump=jump).returncode:
                raise HarnessError(f"{address}: Netplay.pak is not installed")
            state = NETPLAY_STATE
            backup = f"{HARNESS_STATE}/session-backups"
            legacy_backup = f"{pak}/state/harness-backup"
            remote.run(address, f"mkdir -p {shlex.quote(str(Path(backup).parent))}; "
                       f"test ! -d {shlex.quote(legacy_backup)} || "
                       f"test -e {shlex.quote(backup)} || mv {shlex.quote(legacy_backup)} {shlex.quote(backup)}; "
                       f"mkdir -p {shlex.quote(backup)}; "
                       f"for f in {shlex.quote(state + '/session.conf')} {shlex.quote(state + '/session')}; do "
                       f"test ! -f \"$f\" || cp -p \"$f\" {shlex.quote(backup)}/; done",
                       jump=jump)
            atomic_remote_file(remote, address, rendered[role], f"{state}/session.conf", jump)
            atomic_remote_file(remote, address, rendered[role], f"{state}/session", jump)
            print(f"{role} ({route_label(address, jump)}): armed session {session_id}")
    print(session_id)
    return 0


def artifact_destination(name, platform):
    relative, mode = ARTIFACTS[name]
    return "/mnt/SDCARD/" + relative.format(platform=platform), mode


def command_deploy(args):
    require_programs("ssh", "scp")
    source = args.file.resolve()
    if not source.is_file():
        raise HarnessError(f"artifact does not exist: {source}")
    digest = sha256(source)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    with remote_from_args(args) as remote:
        selected = resolved_devices(remote, args)
        if args.target != "both":
            selected = [item for item in selected if item[0] == args.target]
        for role, address, jump in selected:
            platform = platform_for(args, role)
            destination, mode = artifact_destination(args.artifact, platform)
            uploaded = f"/tmp/netplay-artifact-{uuid.uuid4().hex}"
            remote.upload(address, source, uploaded, jump=jump)
            parent = str(Path(destination).parent)
            backup = f"{HARNESS_STATE}/backups/{stamp}/{args.artifact}"
            legacy = f"/mnt/SDCARD/.userdata/{platform}/netplay-harness"
            command = (
                f"set -e; mkdir -p {shlex.quote(HARNESS_STATE)}; "
                f"if test -d {shlex.quote(legacy)}; then "
                f"cp -Rp {shlex.quote(legacy)}/. {shlex.quote(HARNESS_STATE)}/ && "
                f"rm -rf {shlex.quote(legacy)}; fi; "
                f"mkdir -p {shlex.quote(parent)} {shlex.quote(str(Path(backup).parent))}; "
                f"test ! -e {shlex.quote(destination)} || cp -p {shlex.quote(destination)} {shlex.quote(backup)}; "
                f"chmod {mode} {shlex.quote(uploaded)}; mv {shlex.quote(uploaded)} {shlex.quote(destination)}; "
                f"sync; sha256sum {shlex.quote(destination)}"
            )
            result = remote.run(address, command, jump=jump)
            remote_digest = sha256_from_output(result.stdout)
            if remote_digest != digest:
                detail = result.stdout.strip() or result.stderr.strip() or "no sha256sum output"
                raise HarnessError(
                    f"{address}: checksum mismatch after deployment "
                    f"(local {digest}, remote {remote_digest or 'missing'}; {detail})"
                )
            print(f"{role} ({route_label(address, jump)}): "
                  f"{args.artifact} -> {destination} ({digest[:12]})")
    return 0


def command_launch_plan(args):
    for role, address in device_addresses(args):
        rom = args.host_rom if role == "host" else args.client_rom
        print(f"[{role} {address}]\n{launch_command(args.engine, platform_for(args, role), rom)}")
    print("Execution is gated until a NextUI display/input handoff adapter is installed.",
          file=sys.stderr)
    return 0


def command_collect(args):
    require_programs("ssh", "scp")
    with remote_from_args(args) as remote:
        devices = resolved_devices(remote, args)
        session_id = args.session_id
        if args.latest:
            role, address, jump = devices[0]
            root = f"/mnt/SDCARD/.userdata/{platform_for(args, role)}/logs/netplay-games"
            result = remote.run(
                address,
                f"ls -td {shlex.quote(root)}/*/* 2>/dev/null | head -1",
                check=False,
                jump=jump,
            )
            latest = result.stdout.strip().splitlines()
            session_id = Path(latest[-1]).name if latest else None
            if not session_id:
                raise HarnessError(f"{address}: no retained Netplay game sessions found")
            if len(session_id) != 32 or any(
                char not in "0123456789abcdefABCDEF" for char in session_id
            ):
                raise HarnessError(f"{address}: invalid latest session directory: {session_id}")

        output = args.output.resolve() / session_id
        output.mkdir(parents=True, exist_ok=True)
        manifest = {"session_id": session_id,
                    "collected_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "devices": {}}
        for role, address, jump in devices:
            target = output / role
            target.mkdir(parents=True, exist_ok=True)
            logs = f"/mnt/SDCARD/.userdata/{platform_for(args, role)}/logs"
            sources = [
                f"{logs}/netplay-games/*/{session_id}",
                f"{logs}/MGBA.txt",
                f"{logs}/NDS.txt",
                f"{logs}/NDS-wifi-*.txt",
                f"{NETPLAY_STATE}/broker.log",
            ]
            copied = []
            for source in sources:
                print(f"{role}: collecting {source}", flush=True)
                if remote.download_tree(address, source, target, jump=jump):
                    copied.append(source)
            manifest["devices"][role] = {
                "address": address,
                "jump": jump,
                "transport": "jump" if jump else "direct",
                "sources": copied,
            }
            print(f"{role} ({route_label(address, jump)}): collected {len(copied)} log source(s)")
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(output)
    return 0


def add_connection_arguments(parser):
    parser.add_argument("--host", required=True, type=validate_address)
    parser.add_argument("--client", required=True, type=validate_address)
    parser.add_argument("--platform", default=DEFAULT_PLATFORM)
    parser.add_argument("--host-platform",
                        help="platform directory on the host (default: --platform)")
    parser.add_argument("--client-platform",
                        help="platform directory on the client (default: --platform)")
    parser.add_argument("--user", default="root")
    parser.add_argument("--ssh-port", type=int, default=22)
    parser.add_argument("--identity", type=Path)
    parser.add_argument("--password-env", default="DEVBRIDGE_PASSWORD")
    parser.add_argument("--connect-timeout", type=int, default=8)
    parser.add_argument("--guest-ip", type=validate_guest_address,
                        help="force this ad-hoc guest address through the host")
    parser.add_argument("--no-auto-jump", action="store_true",
                        help="do not inspect the host broker for an ad-hoc guest")


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="command", required=True)
    discover = subs.add_parser("discover", help="find DevBridge devices")
    discover.add_argument("--count", type=int, default=2)
    discover.add_argument("--timeout", type=float, default=20)
    discover.add_argument("--multicast", action="store_true")
    discover.add_argument("--interface", default="0.0.0.0")
    discover.add_argument("--json", action="store_true")
    discover.set_defaults(handler=command_discover)
    doctor = subs.add_parser("doctor", help="check both devices and emulator installations")
    add_connection_arguments(doctor)
    doctor.set_defaults(handler=command_doctor)
    route = subs.add_parser("route", help="show the currently usable control path")
    add_connection_arguments(route)
    route.add_argument("--wait", type=float, default=0,
                       help="wait this many seconds for a usable route")
    route.add_argument("--require-jump", action="store_true",
                       help="fail until the guest is reachable through the host")
    route.add_argument("--json", action="store_true")
    route.set_defaults(handler=command_route)
    session = subs.add_parser("session", help="atomically arm one correlated session")
    add_connection_arguments(session)
    session.add_argument("--session-id")
    session.add_argument("--mode", choices=("auto", "netplay", "link"), default="auto")
    session.add_argument("--set", dest="setting", action="append", type=validate_setting, default=[])
    session.add_argument("--dry-run", action="store_true")
    session.set_defaults(handler=command_session)
    deploy = subs.add_parser("deploy", help="backup and replace a known artifact")
    add_connection_arguments(deploy)
    deploy.add_argument("--artifact", required=True, choices=sorted(ARTIFACTS))
    deploy.add_argument("--file", required=True, type=Path)
    deploy.add_argument("--target", choices=("host", "client", "both"), default="both")
    deploy.set_defaults(handler=command_deploy)
    launch = subs.add_parser("launch-plan", help="print launch commands without taking over NextUI")
    add_connection_arguments(launch)
    launch.add_argument("--engine", required=True, choices=sorted(ENGINE_LAUNCHERS))
    launch.add_argument("--host-rom", required=True)
    launch.add_argument("--client-rom", required=True)
    launch.set_defaults(handler=command_launch_plan)
    collect = subs.add_parser("collect", help="download correlated logs")
    add_connection_arguments(collect)
    session_source = collect.add_mutually_exclusive_group(required=True)
    session_source.add_argument("--session-id")
    session_source.add_argument("--latest", action="store_true",
                                help="collect the newest retained game session")
    collect.add_argument("--output", type=Path, default=Path("testing/results"))
    collect.set_defaults(handler=command_collect)
    return parser


def main(argv=None):
    try:
        args = build_parser().parse_args(argv)
        return args.handler(args)
    except (HarnessError, OSError) as error:
        print(f"netplay-harness: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
