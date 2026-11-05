"""The protocol comparison table: one set of measures for every run, over the
same play window, the WINDOW seconds (default 93: 90 s of play and the
screenshot) ending at the after-play screenshot. Device clocks agree with the
PC to about a second.

  table.py RUN_DIR...        (collect.sh fills each RUN_DIR; run.env says what it was)

RetroArch protocol (pure-ra, ra-netplay), from a capture: frame numbers from
INPUT commands; checks = CRC sent by the host; failed = REQUEST_SAVESTATE from
the client; resends = LOAD_SAVESTATE; frames on a guess and rollbacks
estimated as in testing/retroarch/rollback.py (pure RetroArch only: Netplay
never guesses). A ra-netplay run with no usable capture falls back to the
Netplay log's "ra: frame" lines.

Linkplay, from the RG35XXH's Netplay log (or the Brick's): frames run, from the
pacing lines; checks = in sync + DESYNC; resends = committed resyncs; longest
freeze = the longest stall run, or a resync from DESYNC to commit, whichever is
longer.
"""
import importlib.util
import os
import re
import struct
import sys
from bisect import bisect_right
from datetime import datetime, timedelta

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("radecode", os.path.join(HERE, "..", "retroarch", "radecode.py"))
rd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rd)
FRAME_MS = 1000 / 60.0
NAMES = {"pure-ra": "Pure RetroArch", "ra-netplay": "RetroArch over Netplay", "linkplay": "Linkplay"}


def read_run(d):
    env = {}
    for line in open(os.path.join(d, "run.env")):
        if "=" in line:
            k, v = line.rstrip("\n").split("=", 1)
            env[k] = v
    env["dir"] = d
    env["end"] = float(env["end"])
    env["window"] = float(env.get("window", 93))
    return env


def pick_pcap(d):
    for n in ("brick.pcap", "h700.pcap"):
        p = os.path.join(d, n)
        if os.path.exists(p) and os.path.getsize(p) > 1000:
            return p


def pick_log(d):
    for n in ("h700.log", "brick.log"):
        p = os.path.join(d, n)
        if os.path.exists(p):
            return p


def rate_stats(points, t0, t1):
    """points: [(t, frame)] sorted. Average fps, slowest 10 s, longest stand-still (ms)."""
    pts = [p for p in points if t0 <= p[0] <= t1]
    if len(pts) < 2:
        return None
    fps = (pts[-1][1] - pts[0][1]) / (pts[-1][0] - pts[0][0])
    ts = [p[0] for p in pts]
    slow = None
    for i, (t, f) in enumerate(pts):
        j = bisect_right(ts, t + 10.0)
        if j >= len(pts):
            break
        j = max(j, i + 1)
        r = (pts[j][1] - f) / (pts[j][0] - t)
        slow = r if slow is None else min(slow, r)
    longest, pt, pf = 0.0, pts[0][0], pts[0][1]
    for t, f in pts:
        if f > pf:
            longest = max(longest, t - pt)
            pt, pf = t, f
    return fps, slow if slow is not None else fps, longest * 1000


def latency_range(s):
    lo, _, hi = s.partition("-")
    return int(lo), int(hi or lo)


def capture(run):
    t1 = run["end"]
    t0 = t1 - run["window"]
    hip, cip = run["host_ip"], run["guest_ip"]
    pcap = pick_pcap(run["dir"])
    if not pcap:
        return None
    out, _ = rd.streams(pcap)
    frames = {}   # src ip -> [(abs t, frame, payload)]
    cmds = []     # (abs t, src ip, name)
    for key, chunks in out.items():
        (s, _), _ = key
        buf = b"".join(c for _, c, _ in chunks)
        times, pos = [], 0
        for ts, c, _ in chunks:
            times.append((pos, ts))
            pos += len(c)
        offs = [p for p, _ in times]
        off = 24
        while off + 8 <= len(buf):
            cmd, size = struct.unpack(">II", buf[off:off + 8])
            body = buf[off + 8:off + 8 + size]
            if len(body) < size:
                break
            name = rd.CMDS.get(cmd, hex(cmd))
            t = times[max(bisect_right(offs, off) - 1, 0)][1]
            if name == "INPUT" and size >= 12:
                frames.setdefault(s, []).append((t, struct.unpack(">I", body[:4])[0], body[8:]))
            elif t0 <= t <= t1:
                cmds.append((t, s, name))
            off += 8 + size
    rh = rate_stats(sorted((t, f) for t, f, _ in frames.get(hip, [])), t0, t1)
    rc = rate_stats(sorted((t, f) for t, f, _ in frames.get(cip, [])), t0, t1)
    if not rh or not rc:
        return None
    fps, slow, freeze = min(rh[0], rc[0]), min(rh[1], rc[1]), max(rh[2], rc[2])
    checks = sum(1 for _, s, n in cmds if n == "CRC" and s == hip)
    failed = sum(1 for _, s, n in cmds if n == "REQUEST_SAVESTATE" and s == cip)
    resends = sum(1 for _, s, n in cmds if n == "LOAD_SAVESTATE" and s == hip)
    guess = None
    if run["protocol"] == "pure-ra":
        lat = latency_range(run.get("latency", "0"))
        guess = (guess_est(frames, hip, cip, lat, t0, t1), guess_est(frames, cip, hip, lat, t0, t1))
    return fps, slow, freeze, checks, failed, resends, guess


def guess_est(frames, local_ip, remote_ip, lat, t0, t1):
    local = sorted(frames.get(local_ip, []))
    remote = sorted(frames.get(remote_ip, []))
    real = {}
    for _, f, p in remote:
        real.setdefault(f, p)
    rtimes = [t for t, _, _ in remote]
    rmax, m = [], -1
    for _, f, _ in remote:
        m = max(m, f)
        rmax.append(m)
    res = []
    for latency in (lat[1], lat[0]):          # fewest guesses first
        guessed, n = {}, 0
        for t, f, _ in local:
            if not (t0 <= t <= t1):
                continue
            n += 1
            e = f - latency
            i = bisect_right(rtimes, t) - 1
            h = rmax[i] if i >= 0 else -1
            if e > h >= 0:
                for g in range(h + 1, e + 1):
                    guessed.setdefault(g, real.get(h))
        wrong = sorted(g for g, p in guessed.items() if g in real and real[g] != p)
        rb = sum(1 for i, g in enumerate(wrong) if i == 0 or g != wrong[i - 1] + 1)
        res.append((100.0 * len(guessed) / max(n, 1), rb * 60.0 / (t1 - t0)))
    return res


def log_time(hms, end):
    """A log's [HH:MM:SS.mmm] as an epoch, on the day nearest the run's end."""
    e = datetime.fromtimestamp(end)
    h, m, s = hms.split(":")
    t = e.replace(hour=int(h), minute=int(m), second=int(float(s)), microsecond=int((float(s) % 1) * 1e6))
    if (t - e).total_seconds() > 12 * 3600:
        t -= timedelta(days=1)
    elif (e - t).total_seconds() > 12 * 3600:
        t += timedelta(days=1)
    return t.timestamp()


def linkplay(run):
    t1 = run["end"]
    t0 = t1 - run["window"]
    pts, checks, failed, resends, freeze_ms, ran = [], 0, 0, 0, 0.0, 0
    desync_t = None
    for line in open(pick_log(run["dir"]), errors="replace"):
        m = re.match(r"\[(\d\d:\d\d:\d\d\.\d+)\] \[netplay-shim\] (.*)", line)
        if not m:
            continue
        t = log_time(m.group(1), t1)
        msg = m.group(2)
        a = re.match(r"(?:instanced )?pacing: (\d+) (?:paired )?frames in", msg)
        if a:
            ran += int(a.group(1))
            pts.append((t, ran))
        if not (t0 <= t <= t1):
            continue
        if msg.startswith("in sync at frame") or msg.startswith("paired consoles agree"):
            checks += 1
        elif msg.startswith("DESYNC"):
            checks += 1
            failed += 1
            desync_t = t
        elif re.match(r"authoritative resync \d+ committed", msg):
            resends += 1
            if desync_t:
                freeze_ms = max(freeze_ms, (t - desync_t) * 1000)
                desync_t = None
        p = re.match(r".*pacing: .* longest (\d+),", msg)
        if p:
            freeze_ms = max(freeze_ms, int(p.group(1)) * FRAME_MS)
    fps, slow, _ = rate_stats(sorted(pts), t0, t1)
    return fps, slow, freeze_ms, checks, failed, resends, None


def ralog(run):
    """Netplay's RetroArch-protocol log: 'ra: frame N ... (max M) ... crc X bad of Y, states L loaded'."""
    t1 = run["end"]
    t0 = t1 - run["window"]
    pts, freeze_ms, first, last = [], 0.0, None, None
    for line in open(pick_log(run["dir"]), errors="replace"):
        m = re.match(r"\[(\d\d:\d\d:\d\d\.\d+)\] \[netplay-shim\] ra: frame (\d+), .*\(max (\d+)\).*crc (\d+) bad of (\d+), states (\d+) loaded", line)
        if not m:
            continue
        t = log_time(m.group(1), t1)
        pts.append((t, int(m.group(2))))
        if t0 <= t <= t1:
            freeze_ms = max(freeze_ms, int(m.group(3)) * FRAME_MS)
            vals = (int(m.group(4)), int(m.group(5)), int(m.group(6)))
            first = first or vals
            last = vals
    fps, slow, _ = rate_stats(sorted(pts), t0, t1)
    return fps, slow, freeze_ms, last[1] - first[1], last[0] - first[0], last[2] - first[2], None


def rng(a, b, unit=""):
    return f"{a:.0f}{unit}" if round(a) == round(b) else f"{a:.0f}-{b:.0f}{unit}"


print("| Protocol | Latency / delay | Game | Avg fps | Slowest 10 s | Longest freeze | Checks failed | State resends | Frames on a guess (host / guest) | Rollbacks per min (host / guest) |")
print("|" + "---|" * 10)
for d in sys.argv[1:]:
    run = read_run(d)
    if run["protocol"] == "linkplay":
        r = linkplay(run)
    else:
        r = capture(run) or ralog(run)
    fps, slow, freeze, checks, failed, resends, guess = r
    if guess:
        hg, cg = guess
        gs = f"{rng(hg[0][0], hg[1][0], '%')} / {rng(cg[0][0], cg[1][0], '%')}"
        rs = f"{rng(hg[0][1], hg[1][1])} / {rng(cg[0][1], cg[1][1])}"
    else:
        gs, rs = "0 (no guessing)", "0 (no rollback)"
    print(f"| {NAMES.get(run['protocol'], run['protocol'])} | {run.get('label', '')} | {run.get('game', os.path.basename(d))} "
          f"| {fps:.1f} | {slow:.1f} | {freeze:.0f} ms | {failed} of {checks} | {resends} | {gs} | {rs} |")
