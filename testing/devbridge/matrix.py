#!/usr/bin/env python3
"""Run the two-device gameplay tests as an unattended matrix.

    matrix.py [--rig FILE] [--plan NAME ...] [--only REGEX] [--list]
              [--retries N] [--out DIR] [--resume DIR]

Every cell sets the case up from nothing and puts it back afterwards, with
no one at the devices:

  1. both devices idle: no session, no game, on their house network, DevBridge
     in front (a cell that left one stranded is waited out first)
  2. WiFi power save on or off on both (on an H700 that reloads its driver)
  3. the host arms over WiFi or its own ad hoc network, headless through
     device/ctl.sh; the guest finds it by its announcement and joins
  4. the test runs through DevBridge; a guest that left the LAN for the
     host's ad hoc network is reached through an SSH tunnel via the host
  5. check.sh judges each device's netplay log; the host ends the session and
     the guest has to come back to its house network by itself

Results go to DIR (default testing/results/matrix/<time>): results.jsonl, one
line per attempt; report.md; and per cell the runner's log and DevBridge's
session with every screenshot. --resume DIR skips the cells that already
passed there.

A reference screen a test needs but refs/ lacks for that platform is checked
against another platform's copy, and the device's own screen is captured right
after it matches (with no copy anywhere, the check waits out its timeout and
captures instead). Captures from a cell that passed are copied into refs/ and
listed in the report to be looked at; a failed cell's stay in its session.

The rig file (default rig.json next to this script, kept out of git) names
the devices and how to reach them; see rig.example.json.
"""

import argparse
import datetime as dt
import json
import os
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
HELPERS = "/tmp/netplay-devbridge"
CTL = f"sh {HELPERS}/ctl.sh"


class Fail(Exception):
    """The cell failed; the message says where."""


class Infra(Fail):
    """The rig, not the thing under test, failed: retry, and stop the run if
    a device cannot be brought back."""


def log(msg):
    stamp = dt.datetime.now().strftime("%H:%M:%S")
    line = f"[{stamp}] {msg}"
    print(line, flush=True)
    if Runner.cell_log:
        Runner.cell_log.write(line + "\n")
        Runner.cell_log.flush()


# --- devices -----------------------------------------------------------------

class Device:
    """One handheld, over USB ADB or SSH. A device on a guest's side of an ad
    hoc network is reached through a tunnel (self.tunnel_port) instead."""

    def __init__(self, name, spec, rig):
        self.name = name
        self.serial = spec.get("adb")
        self.address = spec.get("ssh")
        self.password = spec.get("password")
        self.rig = rig
        self.tunnel_port = None
        self.platform = None

    def __str__(self):
        return self.name

    # SSH options shared by commands, tunnels and DevBridge.
    def ssh_base(self):
        opts = ["-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=no",
                "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR",
                "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=3",
                "-i", str(self.rig.identity)]
        if self.password:
            opts += ["-o", "PreferredAuthentications=publickey,password",
                     "-o", "NumberOfPasswordPrompts=1"]
        else:
            opts += ["-o", "BatchMode=yes"]
        return opts

    def ssh_env(self):
        env = os.environ.copy()
        if self.password:
            env.update(SSH_ASKPASS=str(self.rig.askpass), SSH_ASKPASS_REQUIRE="force",
                       DISPLAY=env.get("DISPLAY", ":0"), NETPLAY_RIG_PASSWORD=self.password)
        return env

    def ssh_target(self):
        if self.tunnel_port:
            return ["-p", str(self.tunnel_port), "root@127.0.0.1"]
        return ["root@" + self.address]

    @property
    def via_adb(self):
        return bool(self.serial)

    def sh(self, command, timeout=60, check=False, stdin=None, tries=3):
        """Run a shell command on the device; returns (rc, output). An SSH
        connection that fails outright (255: a Brick with power save on
        loses a third of its packets on some APs) is tried again."""
        for attempt in range(tries):
            rc, out = self._sh(command, timeout, stdin)
            if self.via_adb or rc != 255:
                break
            time.sleep(5)
        if check and rc:
            raise Infra(f"{self}: {command}: rc {rc}: {out.strip()[-300:]}")
        return rc, out

    def _sh(self, command, timeout, stdin):
        if self.via_adb:
            argv = ["adb", "-s", self.serial, "shell", command]
            env, new_session = None, False
        else:
            argv = ["ssh", *self.ssh_base(), *self.ssh_target(), command]
            env, new_session = self.ssh_env(), bool(self.password)
        try:
            r = subprocess.run(argv, input=stdin, capture_output=True, text=True,
                               timeout=timeout, env=env, start_new_session=new_session,
                               stdin=None if stdin is not None else subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            return 124, f"timed out: {command}"
        return r.returncode, r.stdout + r.stderr

    def push(self, local, remote):
        if self.via_adb:
            r = subprocess.run(["adb", "-s", self.serial, "push", str(local), remote],
                               capture_output=True, text=True, timeout=120)
            if r.returncode:
                raise Infra(f"{self}: push {local}: {r.stderr.strip()}")
        else:
            self.sh(f"cat > {shlex.quote(remote)}", stdin=Path(local).read_text(), check=True)

    def reachable(self, timeout=10):
        rc, out = self.sh("echo up", timeout=timeout)
        return rc == 0 and "up" in out

    def lan_ip(self):
        rc, out = self.sh("ip -4 addr show wlan0", timeout=15)
        m = re.search(r"inet (\d+\.\d+\.\d+\.\d+)/", out)
        return m.group(1) if m else None

    def ctl(self, *args, timeout=150, check=True):
        """ctl.sh; returns its key=value lines as a dict plus 'result'."""
        rc, out = self.sh(CTL + " " + " ".join(shlex.quote(a) for a in args), timeout=timeout)
        info = {}
        for line in out.splitlines():
            if "=" in line and " " not in line.split("=", 1)[0]:
                k, v = line.split("=", 1)
                info[k] = v
            if line.startswith(("ok ", "error ")):
                info["result"] = line
        log(f"{self}: ctl {' '.join(args)} -> {info.get('result', 'rc %d' % rc)}")
        if check and rc:
            raise Fail(f"{self}: ctl {' '.join(args)}: {info.get('result') or out.strip()[-200:]}")
        return info

    def detach(self, out, *args):
        """Start ctl.sh detached: for steps that take this device off the
        network the command arrived over."""
        cmd = f"sh {HELPERS}/detach.sh {out} " + CTL + " " + " ".join(shlex.quote(a) for a in args)
        self.sh(cmd, timeout=30, check=True)

    def devbridge_args(self, alias):
        if self.via_adb:
            return ["--adb", f"{self.serial}={alias}"]
        if self.tunnel_port:
            return ["--connect", f"127.0.0.1:{alias}:{self.tunnel_port}"]
        return ["--connect", f"{self.address}:{alias}:22"]


class Tunnel:
    """Local port -> target:22 through `via`, which keeps its LAN address
    while it serves the ad hoc network."""

    def __init__(self, via, via_address, target_ip):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            self.port = s.getsockname()[1]
        argv = ["ssh", *via.ssh_base(), "-N", "-o", "ExitOnForwardFailure=yes",
                "-L", f"127.0.0.1:{self.port}:{target_ip}:22", "root@" + via_address]
        self.proc = subprocess.Popen(argv, env=via.ssh_env(), stdin=subprocess.DEVNULL,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                     start_new_session=True)
        deadline = time.time() + 20
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise Infra(f"tunnel via {via} to {target_ip} exited")
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=1):
                    return
            except OSError:
                time.sleep(0.5)
        self.close()
        raise Infra(f"tunnel via {via} to {target_ip} never opened")

    def alive(self):
        return self.proc.poll() is None

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


class Rig:
    def __init__(self, path, out):
        spec = json.loads(Path(path).read_text())
        self.identity = Path(spec.get("identity", "~/.ssh/id_ed25519")).expanduser()
        self.askpass = out / "askpass.sh"
        self.askpass.write_text('#!/bin/sh\nprintf "%s\\n" "$NETPLAY_RIG_PASSWORD"\n')
        self.askpass.chmod(0o700)
        self.devices = {n: Device(n, d, self) for n, d in spec["devices"].items()}
        self.pairs = spec.get("pairs", {})
        # Next to this checkout, or next to the repository a worktree of it
        # lives in.
        found = [p / "DevBridge.pak" for p in ROOT.parents
                 if (p / "DevBridge.pak/tools/devbridge.py").exists()]
        self.devbridge = Path(spec.get("devbridge") or os.environ.get("DEVBRIDGE")
                              or (found[0] if found else ROOT.parent / "DevBridge.pak")).expanduser()
        if not (self.devbridge / "tools/devbridge.py").exists():
            raise SystemExit(f"DevBridge not found at {self.devbridge} (set devbridge in the rig file)")
        self.password = next((d.password for d in self.devices.values() if d.password), None)


# --- the plan ----------------------------------------------------------------

DS_CHECK = ("dsp", "mariokart")
# A player's own per-game settings that change what the screen shows: a
# DSperate layout picked in-game is remembered here (an H700 had Mario Kart
# on picture-in-picture, which no reference matches). Moved aside for a DS
# cell and put back after it, or at the next run if this one was killed.
DS_ASIDE = ["/mnt/SDCARD/.userdata/{platform}/DSP-dsperate/dsperate/games/mariokart.ini"]
ASIDE_SUFFIX = ".netplay-matrix-aside"
SYSTEM_TESTS = {
    # name: (test file, check.sh mode, ROM pattern)
    "nes":  ("rcr.test", "shared", "River_City_Ransom"),
    "snes": ("contra3.test", "shared", "Contra_III"),
    "gb":   ("tetris.test", "instanced", "Tetris"),
    "gba-gpsp": ("aw2.test", "link", "Advance_Wars_2"),
    "gba-mgba": ("mkart.test", "instanced", "Mario_Kart"),
    "ps":   ("ctr.test", "shared", "CTR"),
}


class Cell:
    def __init__(self, plan, test, host, guest, net, powersave, check, ds=None, min_fps=None):
        self.plan, self.test, self.host, self.guest = plan, test, host, guest
        self.net, self.powersave, self.check, self.ds = net, powersave, check, ds
        self.min_fps = min_fps

    @property
    def id(self):
        emus = f"{self.ds[0]}-{self.ds[1]}" if self.ds else Path(self.test).stem
        return f"{self.plan}.{emus}.{self.host}-{self.guest}.{self.net}.ps-{self.powersave}"


def build_plan(rig, names):
    """Cells for each named plan, in the order named."""
    cells = []
    for name in names:
        cells += build_one(rig, name)
    return cells


def build_one(rig, name):
    cells = []
    bb, bh = rig.pairs.get("BB"), rig.pairs.get("BH")
    if name == "ds":
        # Power save outermost: on an H700 each change is a driver reload.
        for ps in ("off", "on"):
            orients = []
            if bb: orients.append(tuple(bb))
            if bh: orients += [tuple(bh), tuple(reversed(bh))]
            for host, guest in orients:
                for net in ("adhoc", "wifi"):
                    for ds in (("dsp", "dsp"), ("nds", "nds"), ("nds", "dsp"), ("dsp", "nds")):
                        floor = 45 if net == "adhoc" else 30
                        cells.append(Cell("ds", "mkds.test", host, guest, net, ps,
                                          DS_CHECK, ds=ds, min_fps=floor))
    if name == "systems":
        # The established tests: a Brick hosting an RG35XXH, the references
        # they were recorded with; PlayStation Brick to Brick as well.
        for net in ("adhoc", "wifi"):
            for key, (test, mode, rom) in SYSTEM_TESTS.items():
                if bh:
                    cells.append(Cell("sys", test, bh[0], bh[1], net, "off", (mode, rom)))
                if key == "ps" and bb:
                    cells.append(Cell("sys", test, bb[0], bb[1], net, "off", (mode, rom)))
    return cells


# --- test scripts ------------------------------------------------------------

COND = re.compile(r"^\[(host|guest):(\w+)\]\s*")
VERIFY = re.compile(r"^(\w+):(VERIFY_WAIT|VERIFY)\s+(\S+\.png)(?:\s+(\d+))?(.*)$")


def render_test(cell, host, guest, session, out_dir):
    """The cell's DevBridge script, and the references it will record."""
    refs = {"host": HERE / "refs" / host.platform, "guest": HERE / "refs" / guest.platform}
    subst = {
        "{host_refs}": str(refs["host"]), "{guest_refs}": str(refs["guest"]),
        "{helpers}": HELPERS,
    }
    if cell.ds:
        subst.update({
            "{host_ds}": cell.ds[0], "{guest_ds}": cell.ds[1],
            "{host_pak}": cell.ds[0].upper(), "{guest_pak}": cell.ds[1].upper(),
            "{min_fps}": str(cell.min_fps),
        })
    side = {"host": cell.ds[0] if cell.ds else None, "guest": cell.ds[1] if cell.ds else None}
    lines, recorded = [f"SESSION {session} {out_dir}"], []
    for raw in (HERE / cell.test).read_text().splitlines():
        m = COND.match(raw)
        if m:
            if side[m.group(1)] != m.group(2):
                continue
            raw = raw[m.end():]
        for k, v in subst.items():
            raw = raw.replace(k, v)
        v = VERIFY.match(raw)
        if v and not Path(v.group(3)).exists():
            who, kind, ref, timeout = v.group(1), v.group(2), Path(v.group(3)), v.group(4)
            label = f"rec__{ref.parent.name}__{ref.stem}"
            # Another platform's copy: after DevBridge's 64x48 reduction a
            # Brick's and an RG35XXH's Mario Kart screens are 1-7 apart,
            # under the threshold of 10. Check against it, then capture this
            # device's own screen while it is still the one that matched.
            borrowed = sorted(p for p in (HERE / "refs").glob(f"*/{ref.name}") if p != ref)
            if borrowed:
                lines.append(raw.replace(v.group(3), str(borrowed[0])))
                lines.append(f"{who}:SCREEN {label}")
            else:
                # Nothing to check against: wait the timeout out and capture.
                if kind == "VERIFY_WAIT" and timeout:
                    lines.append(f"WAIT {timeout}")
                lines.append(f"{who}:SCREEN {label}")
            recorded.append((label, who, ref))
            continue
        lines.append(raw)
    return "\n".join(lines) + "\n", recorded


# --- the runner ----------------------------------------------------------------

class Runner:
    cell_log = None

    def __init__(self, args):
        self.args = args
        stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.out = Path(args.resume or args.out or ROOT / "testing/results/matrix" / stamp).resolve()
        self.out.mkdir(parents=True, exist_ok=True)
        self.rig = Rig(args.rig, self.out)
        self.tunnels = []

    # devices

    def prepare(self, devices):
        for d in devices:
            if not d.reachable(20):
                raise Infra(f"{d}: not reachable")
            rc, out = d.sh("for p in /mnt/SDCARD/Tools/*/Netplay.pak; do [ -d $p ] && basename $(dirname $p); done",
                           check=True)
            d.platform = out.split()[0]
            d.sh(f"mkdir -p {HELPERS}", check=True)
            self.restore_aside(d)
            for f in sorted((HERE / "device").glob("*.sh")):
                d.push(f, f"{HELPERS}/{f.name}")
            # The host keeps its LAN address while it serves ad hoc, or
            # nothing could reach a tunnelled guest.
            d.ctl("set", "adhoc_switch", "0")
            log(f"{d}: {d.platform}, helpers pushed")

    def devbridge(self, devices, script, log_path, timeout):
        argv = [sys.executable, "-P", str(self.rig.devbridge / "tools/devbridge.py"),
                "--identity", str(self.rig.identity), "--password-fallback",
                "--launch-timeout", "45"]
        for alias, d in devices:
            argv += d.devbridge_args(alias)
        with tempfile.NamedTemporaryFile("w", suffix=".test", delete=False, dir=self.out) as f:
            f.write(script)
        argv += ["run", f.name] if script else ["exec", "STATUS"]
        env = os.environ.copy()
        if self.rig.password:
            env["DEVBRIDGE_PASSWORD"] = self.rig.password
        try:
            with open(log_path, "a") as lf:
                r = subprocess.run(argv, stdout=lf, stderr=subprocess.STDOUT, env=env,
                                   timeout=timeout, start_new_session=True,
                                   stdin=subprocess.DEVNULL)
            return r.returncode
        except subprocess.TimeoutExpired:
            return 124
        finally:
            os.unlink(f.name)

    def foreground(self, d, log_path):
        """DevBridge's own view of what is in front."""
        tmp = self.out / f"status-{d}.log"
        tmp.unlink(missing_ok=True)
        self.devbridge([("x", d)], "", tmp, 90)
        text = tmp.read_text() if tmp.exists() else ""
        with open(log_path, "a") as lf:
            lf.write(text)
        m = re.search(r"foreground=(\S+)", text)
        return m.group(1) if m else None

    def stop_games(self, d):
        """A failed test can leave a game running; DevBridge comes back to
        the front when it exits."""
        names = "minarch.elf drastic dsperate"
        rc, out = d.sh(f"pidof {names}", timeout=15)
        if not out.strip():
            return
        log(f"{d}: stopping a game left running ({out.strip()})")
        d.sh(f"killall {names} 2>/dev/null; sleep 3; killall -9 {names} 2>/dev/null; true", timeout=30)
        time.sleep(5)

    def clear_input_binds(self, d):
        """DevBridge binds the virtual pad over the physical one while DraStic
        runs. One left behind with no game running made the next launch's
        mirror copy the pad into itself, and DraStic's menus scrolled on
        their own (DevBridge 0d13597 fixes the cause)."""
        rc, out = d.sh("grep -c ' /dev/input/' /proc/mounts", timeout=20)
        n = int(out.strip() or 0) if out.strip().isdigit() else 0
        if not n:
            return 0
        log(f"{d}: {n} input bind(s) left with no game running; removing")
        for _ in range(n):
            d.sh("m=$(grep ' /dev/input/' /proc/mounts | head -1 | cut -d' ' -f2); "
                 '[ -z "$m" ] || umount -l "$m"', timeout=20)
        return n

    def wait_home(self, d, timeout=180):
        """Back on the house network, reachable directly."""
        d.tunnel_port = None
        deadline = time.time() + timeout
        while time.time() < deadline:
            if d.reachable(10):
                ip = d.lan_ip()
                if ip and not ip.startswith("10.0.0."):
                    return ip
            time.sleep(5)
        raise Infra(f"{d}: not back on its house network after {timeout}s")

    def idle(self, d, log_path):
        self.wait_home(d)
        st = d.ctl("status", check=False)
        if st.get("armed") == "1":
            d.ctl("end", check=False)
            self.wait_home(d)
        self.stop_games(d)
        self.clear_input_binds(d)
        fg = self.foreground(d, log_path)
        if fg != "devbridge":
            # A game that exited by itself takes DevBridge a moment.
            time.sleep(10)
            fg = self.foreground(d, log_path)
        if fg != "devbridge":
            raise Infra(f"{d}: DevBridge is not in front (foreground={fg}); it needs a person")

    def set_aside(self, d):
        for path in DS_ASIDE:
            f = shlex.quote(path.format(platform=d.platform))
            d.sh(f"[ -f {f} ] && mv {f} {f}{ASIDE_SUFFIX}; true", check=True)

    def restore_aside(self, d):
        for path in DS_ASIDE:
            f = shlex.quote(path.format(platform=d.platform))
            d.sh(f"[ -f {f}{ASIDE_SUFFIX} ] && mv -f {f}{ASIDE_SUFFIX} {f}; true", check=True)

    def set_powersave(self, d, mode):
        if d.platform == "h700":
            st = d.ctl("status")
            want_off = mode == "off"
            is_off = st.get("radio_powersave") == "off"
            if want_off == is_off and (st.get("powersave_off") == "1") == want_off:
                return
            # A driver reload drops WiFi under the command that asked for it.
            log(f"{d}: power save {mode} by driver reload")
            d.detach("/tmp/netplay-ctl-ps.out", "powersave", mode)
            time.sleep(15)
            self.wait_home(d, 240)
            rc, out = d.sh("cat /tmp/netplay-ctl-ps.out", timeout=15)
            log(f"{d}: {out.strip().splitlines()[-2:] if out.strip() else 'no output'}")
        else:
            d.ctl("powersave", mode)

    def arm(self, cell, host, guest):
        host.ctl("host", cell.net, timeout=180)
        if cell.net == "wifi" or guest.via_adb:
            guest.ctl("join", "discover", "30", timeout=240)
            return None
        # SSH guest onto the host's ad hoc network: the join takes the
        # guest's network away from this connection.
        guest.detach("/tmp/netplay-ctl-join.out", "join", "discover", "30")
        lease, deadline = None, time.time() + 150
        while time.time() < deadline and not lease:
            time.sleep(5)
            st = host.ctl("status", check=False)
            lease = next((v for k, v in st.items() if re.fullmatch(r"guest\d+", k) and v), None)
        if not lease:
            raise Fail(f"{guest}: never showed up on {host}'s network")
        via_address = host.lan_ip() if host.via_adb else host.address
        tunnel = Tunnel(host, via_address, lease)
        self.tunnels.append(tunnel)
        guest.tunnel_port = tunnel.port
        deadline = time.time() + 120
        while time.time() < deadline:
            rc, out = guest.sh("cat /tmp/netplay-ctl-join.out 2>/dev/null", timeout=20)
            if "exit=" in out:
                log(f"{guest}: {[l for l in out.splitlines() if l.startswith(('ok', 'error'))]}")
                if "exit=0" not in out:
                    raise Fail(f"{guest}: join failed: {out.strip()[-200:]}")
                return tunnel
            time.sleep(4)
        raise Fail(f"{guest}: join did not finish")

    def teardown(self, cell, host, guest):
        for d in (host, guest):
            try:
                self.stop_games(d)
            except Fail as e:
                log(f"teardown: {e}")
        try:
            host.ctl("end", check=False)
        except Fail as e:
            log(f"teardown: {e}")
        for t in self.tunnels:
            t.close()
        self.tunnels = []
        guest.tunnel_port = None
        # The host's session-end message, or the guest's watchdog once the
        # AP is gone, has to bring it home by itself.
        # .181, a Brick on this rig, has gone minutes with the house AP not
        # answering its authentication (no Netplay involved; its twin never
        # does), so a slow return is measured, and only no return fails.
        t0 = time.time()
        self.wait_home(guest, 420)
        for d in (host, guest):
            self.restore_aside(d)
        back = time.time() - t0
        st = guest.ctl("status", check=False)
        if st.get("armed") == "1":
            log(f"{guest}: still armed after the host ended; ending it")
            guest.ctl("end", check=False)
            raise Fail(f"{guest}: kept its session after the host ended it")
        return back

    def judge(self, d, cell):
        mode, rom = cell.check
        floor = cell.min_fps or 0
        rc, out = d.sh(f"sh {HELPERS}/check.sh {mode} {rom} {floor}", timeout=60)
        m = {}
        if (x := re.search(r"([\d.]+) fps average", out)): m["fps"] = float(x.group(1))
        if (x := re.search(r"MP timeouts: (\d+)", out)): m["timeouts"] = int(x.group(1))
        if (x := re.search(r"stalls: (\d+) of (\d+) frames \(([\d.]+)%\), longest (\d+) frames, slowest window ([\d.]+)", out)):
            m.update(stall_frames=int(x.group(1)), frames=int(x.group(2)), stall_pct=float(x.group(3)),
                     longest_stall=int(x.group(4)), slowest_fps=float(x.group(5)))
        if (x := re.search(r"link packets: (\d+), dropped: (\d+)", out)):
            m.update(link_packets=int(x.group(1)), link_dropped=int(x.group(2)))
        m["pass"] = "PASS" in out and rc == 0
        m["check"] = out.strip()[-600:]
        return m

    def run_cell(self, cell, attempt):
        host, guest = self.rig.devices[cell.host], self.rig.devices[cell.guest]
        cdir = self.out / f"{cell.id}.{attempt}"
        cdir.mkdir(exist_ok=True)
        Runner.cell_log = open(cdir / "runner.log", "a")
        res = {"cell": cell.id, "attempt": attempt, "plan": cell.plan, "test": cell.test,
               "host": cell.host, "guest": cell.guest, "host_platform": host.platform,
               "guest_platform": guest.platform, "net": cell.net, "powersave": cell.powersave,
               "ds": cell.ds, "start": dt.datetime.now().isoformat(timespec="seconds")}
        t0 = time.time()
        try:
            log(f"=== {cell.id} (attempt {attempt})")
            for d in (host, guest):
                self.idle(d, cdir / "runner.log")
            for d in (host, guest):
                self.set_powersave(d, cell.powersave)
                if cell.ds:
                    self.set_aside(d)
            t_arm = time.time()
            self.arm(cell, host, guest)
            res["arm_s"] = round(time.time() - t_arm, 1)
            hs, gs = host.ctl("status"), guest.ctl("status")
            res["radio_powersave"] = {"host": hs.get("radio_powersave"), "guest": gs.get("radio_powersave")}
            if gs.get("armed") != "1" or gs.get("role") != "client":
                raise Fail(f"{guest}: not joined: {gs}")
            # check.sh trusts only logs newer than this; a host's confirmed
            # checkpoint outlives a clean exit (see run.sh).
            host.sh(f"touch {HELPERS}/started; rm -f /tmp/netplay-host-*.state", check=True)
            guest.sh(f"touch {HELPERS}/started", check=True)

            session = "session"
            script, recorded = render_test(cell, host, guest, session, cdir)
            (cdir / "script.test").write_text(script)
            if recorded:
                log(f"recording {len(recorded)} missing reference(s): {', '.join(r[2].name for r in recorded)}")
            t_test = time.time()
            rc = self.devbridge([("host", host), ("guest", guest)], script, cdir / "devbridge.log", 1500)
            res["test_s"] = round(time.time() - t_test, 1)
            res["devbridge_rc"] = rc
            res["devbridge_error"] = self.devbridge_error(cdir / "devbridge.log")
            # quit.sh, guest then host: how each emulator was made to exit.
            text = (cdir / "devbridge.log").read_text(errors="replace")
            quits = re.findall(r"^quit: (menu|term|kill)$", text, re.M)
            if quits:
                res["quit"] = quits
            res["checks"] = {"host": self.judge(host, cell), "guest": self.judge(guest, cell)}
            res["pass"] = rc == 0 and all(c["pass"] for c in res["checks"].values())
            if res["pass"] and recorded:
                res["new_refs"] = self.keep_recorded(recorded, cdir / session)
            log(f"test rc {rc}; host {self.brief(res['checks']['host'])}; guest {self.brief(res['checks']['guest'])}")
        except Infra as e:
            res["pass"], res["infra"], res["error"] = False, True, str(e)
            log(f"INFRA: {e}")
        except Fail as e:
            res["pass"], res["error"] = False, str(e)
            log(f"FAIL: {e}")
        finally:
            try:
                res["guest_home_s"] = round(self.teardown(cell, host, guest), 1)
            except Fail as e:
                res["pass"] = False
                res.setdefault("error", str(e))
                res["teardown_error"] = str(e)
                res["infra"] = res.get("infra") or isinstance(e, Infra)
                log(f"teardown: {e}")
            res["total_s"] = round(time.time() - t0, 1)
            with open(self.out / "results.jsonl", "a") as f:
                f.write(json.dumps(res) + "\n")
            log(f"=== {cell.id}: {'PASS' if res['pass'] else 'FAIL'} in {res['total_s']:.0f}s")
            Runner.cell_log.close()
            Runner.cell_log = None
        return res

    @staticmethod
    def brief(c):
        bits = [f"{c['fps']:.1f} fps" if "fps" in c else "no fps"]
        if "timeouts" in c: bits.append(f"{c['timeouts']} MP timeouts")
        if "stall_pct" in c: bits.append(f"stalls {c['stall_pct']}%")
        return ", ".join(bits) + (" PASS" if c["pass"] else " FAIL")

    @staticmethod
    def devbridge_error(path):
        if not path.exists():
            return None
        errs = [l for l in path.read_text(errors="replace").splitlines()
                if re.match(r".*\.test:\d+: ", l) or l.startswith("error:")]
        return errs[-1] if errs else None

    def keep_recorded(self, recorded, session_dir):
        kept = []
        for label, who, ref in recorded:
            shots = sorted(session_dir.glob(f"{label}-*.png"))
            if not shots:
                continue
            ref.parent.mkdir(parents=True, exist_ok=True)
            if not ref.exists():
                shutil.copy(shots[0], ref)
                kept.append(str(ref.relative_to(HERE)))
                log(f"new reference {ref.relative_to(HERE)}")
        return kept

    # the whole run

    def run(self, cells):
        done = set()
        rpath = self.out / "results.jsonl"
        if self.args.resume and rpath.exists():
            for line in rpath.read_text().splitlines():
                r = json.loads(line)
                if r.get("pass"):
                    done.add(r["cell"])
        used = {n for c in cells for n in (c.host, c.guest)}
        self.prepare([self.rig.devices[n] for n in sorted(used)])
        stranded = 0
        for i, cell in enumerate(cells, 1):
            if cell.id in done:
                continue
            log(f"--- cell {i}/{len(cells)}")
            for attempt in range(1, self.args.retries + 2):
                r = self.run_cell(cell, attempt)
                if r["pass"]:
                    stranded = 0
                    break
                if r.get("teardown_error") and r.get("infra"):
                    stranded += 1
            self.report()
            if stranded >= 2:
                log("a device could not be brought back twice running; stopping")
                break
        self.report()

    def report(self):
        rpath = self.out / "results.jsonl"
        if not rpath.exists():
            return
        rows = [json.loads(l) for l in rpath.read_text().splitlines()]
        write_report(rows, self.out / "report.md")


def fmt_fps(c):
    return f"{c['fps']:.1f}" if c and "fps" in c else "-"


def write_report(rows, path):
    last = {}
    attempts = {}
    for r in rows:
        last[r["cell"]] = r
        attempts.setdefault(r["cell"], []).append(r["pass"])
    cells = list(last.values())
    passed = sum(1 for r in cells if r["pass"])
    flaky = [c for c, a in attempts.items() if a[-1] and not all(a)]
    out = [f"# Netplay test matrix", "",
           f"{passed} of {len(cells)} cells passed"
           + (f"; {len(flaky)} only on a retry" if flaky else "") + ".", ""]
    for plan, title in (("ds", "Nintendo DS (Mario Kart DS)"), ("sys", "Other systems")):
        sel = [r for r in cells if r["plan"] == plan]
        if not sel:
            continue
        out += [f"## {title}", "",
                "| Host -> guest | Emulators | Network | Power save | Result | Host fps | Guest fps | MP timeouts (h/g) | Stalls (h/g) | Arm s | Guest home s | Note |",
                "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
        for r in sel:
            ch = r.get("checks", {})
            h, g = ch.get("host", {}), ch.get("guest", {})
            emus = "/".join(x.upper() for x in r["ds"]) if r.get("ds") else Path(r["test"]).stem
            res = "PASS" if r["pass"] else ("INFRA" if r.get("infra") else "FAIL")
            if r["pass"] and len(attempts[r["cell"]]) > 1:
                res += " (retry)"
            to = f"{h.get('timeouts', '-')}/{g.get('timeouts', '-')}" if "timeouts" in h or "timeouts" in g else "-"
            st = f"{h.get('stall_pct', '-')}/{g.get('stall_pct', '-')}%" if "stall_pct" in h or "stall_pct" in g else "-"
            note = r.get("error") or r.get("devbridge_error") or ""
            forced = [q for q in r.get("quit", []) if q != "menu"]
            if forced:
                note = (note + " " if note else "") + f"quit by {'/'.join(r['quit'])} (guest/host)"
            if r.get("new_refs"):
                note = (note + " " if note else "") + f"new refs: {', '.join(r['new_refs'])}"
            rp = r.get("radio_powersave") or {}
            ps = r["powersave"] + (f" ({rp.get('host')}/{rp.get('guest')})" if rp else "")
            out.append(f"| {r['host']} ({r['host_platform']}) -> {r['guest']} ({r['guest_platform']}) | {emus} | {r['net']} | {ps} "
                       f"| {res} | {fmt_fps(h)} | {fmt_fps(g)} | {to} | {st} | {r.get('arm_s', '-')} | {r.get('guest_home_s', '-')} "
                       f"| {note.replace('|', '/')[:160]} |")
        out.append("")
    path.write_text("\n".join(out) + "\n")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--rig", default=str(HERE / "rig.json"))
    p.add_argument("--plan", nargs="+", default=["systems", "ds"], choices=["ds", "systems"],
                   help="plans to run, in this order")
    p.add_argument("--only", help="run only cells whose id matches this regex")
    p.add_argument("--list", action="store_true", help="print the cells and exit")
    p.add_argument("--retries", type=int, default=1)
    p.add_argument("--out")
    p.add_argument("--resume")
    args = p.parse_args()

    spec_rig = json.loads(Path(args.rig).read_text())
    pairs = spec_rig.get("pairs", {})

    class _R:  # the plan needs only the pairs
        pass
    r = _R(); r.pairs = pairs
    cells = build_plan(r, args.plan)
    if args.only:
        cells = [c for c in cells if re.search(args.only, c.id)]
    if args.list:
        for c in cells:
            print(c.id)
        print(f"{len(cells)} cells", file=sys.stderr)
        return 0
    runner = Runner(args)
    print(f"results: {runner.out}", flush=True)
    try:
        runner.run(cells)
    finally:
        for t in runner.tunnels:
            t.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
