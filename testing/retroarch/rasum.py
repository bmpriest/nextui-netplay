"""Summarise a RetroArch netplay capture: per direction, frame rate in 10 s
windows (from INPUT frame numbers), the longest time the frame number stood
still, and counts of CRC / savestate / stall commands.
  rasum.py CAPTURE.pcap [TAIL_SECONDS]   (only the last TAIL_SECONDS of input)"""
import importlib.util, os, struct, sys
from collections import Counter
from bisect import bisect_right
spec = importlib.util.spec_from_file_location("radecode", os.path.join(os.path.dirname(os.path.abspath(__file__)), "radecode.py"))
rd = importlib.util.module_from_spec(spec); spec.loader.exec_module(rd)
out, t0 = rd.streams(sys.argv[1])
for key, chunks in out.items():
    (s, sp), (d, dp) = key
    buf = b"".join(c for _, c, _ in chunks)
    times, pos = [], 0
    for ts, c, _ in chunks:
        times.append((pos, ts)); pos += len(c)
    offs = [p for p, _ in times]
    at = lambda o: times[max(bisect_right(offs, o) - 1, 0)][1] - t0
    off, events, inputs = 24, [], []
    while off + 8 <= len(buf):
        cmd, size = struct.unpack(">II", buf[off:off + 8])
        body = buf[off + 8:off + 8 + size]
        if len(body) < size: break
        name = rd.CMDS.get(cmd, hex(cmd)); events.append((at(off), name))
        if name == "INPUT" and size >= 4: inputs.append((at(off), struct.unpack(">I", body[:4])[0]))
        off += 8 + size
    if len(inputs) < 100: continue
    tail = float(sys.argv[2]) if len(sys.argv) > 2 else 0
    if tail:
        end = inputs[-1][0]
        inputs = [x for x in inputs if x[0] >= end - tail]
        cut = inputs[0][0]
    ta, fa = inputs[0]; tb, fb = inputs[-1]
    wins = []
    w = ta
    while w + 10 <= tb:
        i0 = bisect_right([t for t, _ in inputs], w) - 1; i1 = bisect_right([t for t, _ in inputs], w + 10) - 1
        wins.append((inputs[i1][1] - inputs[max(i0,0)][1]) / 10.0); w += 10
    # longest stand-still of the frame counter
    longest, prev_t, prev_f = 0, ta, fa
    for t, f in inputs:
        if f > prev_f:
            longest = max(longest, t - prev_t); prev_t, prev_f = t, f
    print(f"{s}:{sp} -> {d}: {fb - fa} frames in {tb - ta:.1f}s = {(fb - fa) / (tb - ta):.1f} fps; "
          f"10s windows min {min(wins):.1f} max {max(wins):.1f}; longest stand-still {longest*1000:.0f} ms")
    t_lo = inputs[0][0]
    counts = Counter(n for t, n in events if t >= t_lo - 0.001)
    print("   ", {k: v for k, v in counts.items() if k not in ("INPUT", "PING_REQUEST", "PING_RESPONSE")})
