"""From a RetroArch netplay capture taken on one device, estimate how often that
device emulated a frame on a guessed remote input, and how often the guess was
wrong (each wrong guess forces a rollback: load an earlier state and replay).

  rollback.py CAPTURE.pcap LOCAL_IP LATENCY_MIN LATENCY_MAX

A device sends its input for frame F while emulating F - latency. If the
remote's newest real input at that moment is for H < F - latency, the frames
H+1 .. F-latency ran on a guess. RetroArch guesses by repeating the remote's
last real input; when the real input for a guessed frame arrives and differs,
it rewinds. Latency is a range when RetroArch adapts it, so both ends are
reported.
"""
import importlib.util
import os
import struct
import sys
from bisect import bisect_right

path, local_ip = sys.argv[1], sys.argv[2]
lat_lo, lat_hi = int(sys.argv[3]), int(sys.argv[4])
spec = importlib.util.spec_from_file_location("radecode", os.path.join(os.path.dirname(os.path.abspath(__file__)), "radecode.py"))
rd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rd)

out, t0 = rd.streams(path)
local, remote = [], []          # (t, frame, payload)
for key, chunks in out.items():
    (s, _), _ = key
    buf = b"".join(c for _, c, _ in chunks)
    times, pos = [], 0
    for ts, c, _ in chunks:
        times.append((pos, ts))
        pos += len(c)
    offs = [p for p, _ in times]

    def at(o):
        i = bisect_right(offs, o) - 1
        return times[max(i, 0)][1] - t0

    off = 24
    while off + 8 <= len(buf):
        cmd, size = struct.unpack(">II", buf[off:off + 8])
        body = buf[off + 8:off + 8 + size]
        if len(body) < size:
            break
        if cmd == 0x0003 and size >= 12:
            frame = struct.unpack(">I", body[:4])[0]
            (local if s == local_ip else remote).append((at(off), frame, body[8:]))
        off += 8 + size

local.sort()
remote.sort()
if not local or not remote:
    print(f"{path}: no input both ways (local {len(local)}, remote {len(remote)})")
    sys.exit(0)

real = {}
for _, f, p in remote:
    real.setdefault(f, p)
rtimes = [t for t, _, _ in remote]
rmax = []
m = -1
for _, f, _ in remote:
    m = max(m, f)
    rmax.append(m)


def analyse(latency):
    guessed = {}                 # frame -> the input it was guessed as
    for t, f, _ in local:
        e = f - latency          # frame being emulated
        i = bisect_right(rtimes, t) - 1
        h = rmax[i] if i >= 0 else -1
        if e > h and h >= 0:
            for g in range(h + 1, e + 1):
                guessed.setdefault(g, real.get(h))
    wrong = sorted(g for g, p in guessed.items() if g in real and real[g] != p)
    # Consecutive wrong frames are one rewind: the replay covers them all.
    rewinds = sum(1 for i, g in enumerate(wrong) if i == 0 or g != wrong[i - 1] + 1)
    return len(guessed), len(wrong), rewinds


span = local[-1][0] - local[0][0]
frames = local[-1][1] - local[0][1]
print(f"{path.split('/')[-2]}/{path.split('/')[-1]}: {frames} frames over {span:.0f}s")
for lat in sorted({lat_lo, lat_hi}):
    g, w, r = analyse(lat)
    print(f"  latency {lat}: {g} frames run on a guess ({100 * g / max(frames, 1):.1f}%), "
          f"{w} guessed wrong, ~{r} rollbacks ({r * 60 / max(span, 1):.1f}/min)")
