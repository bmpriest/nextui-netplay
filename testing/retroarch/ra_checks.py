"""ra_checks.py CAPTURE.pcap: RetroArch state checks (CRC) and whether the client
asked for a state within 2 s of each (a mismatch), and every savestate sent:
time, frame, uncompressed size, size on the wire. Use a capture holding the
host's side of the connection (port 55435)."""
import sys, os, importlib.util, struct
spec = importlib.util.spec_from_file_location("rd", os.path.join(os.path.dirname(os.path.abspath(__file__)), "radecode.py"))
rd = importlib.util.module_from_spec(spec); spec.loader.exec_module(rd)
pcap = sys.argv[1]
out, t0 = rd.streams(pcap)
ev = []; first = None; proto = None; loads = []
for (s, d), chunks in out.items():
    from_host = s[1] == 55435
    times = []; pos = 0
    for ts, c, _ in chunks: times.append((pos, ts)); pos += len(c)
    buf = b"".join(c for _, c, _ in chunks); off = 24
    def at(o):
        t = t0
        for p, ts in times:
            if p > o: break
            t = ts
        return t - t0
    if from_host and len(buf) >= 20: proto = struct.unpack(">I", buf[16:20])[0]
    while off + 8 <= len(buf):
        cmd, size = struct.unpack(">II", buf[off:off + 8]); body = buf[off + 8:off + 8 + size]
        if not from_host and cmd == 3 and first is None and body[8:] != b"\0" * 4: first = struct.unpack(">I", body[:4])[0]
        if from_host and cmd == 0x40: ev.append((at(off), "CRC", struct.unpack(">I", body[:4])[0]))
        if not from_host and cmd == 0x41: ev.append((at(off), "REQ", None))
        if from_host and cmd == 0x42:
            fr, raw = struct.unpack(">II", body[:8]); loads.append((round(at(off), 1), fr, raw, size))
        off += 8 + size
ev.sort(); res = []; mism = 0
for i, (t, k, f) in enumerate(ev):
    if k == "CRC":
        bad = any(e[1] == "REQ" and 0 <= e[0] - t < 2.0 for e in ev[i + 1:])
        mism += bad; res.append(f"{f}:{'MISMATCH' if bad else 'match'}")
print(f"protocol {proto}; client's first press at frame {first}")
print(f"checks: {len(res)}, mismatched {mism}:  " + "  ".join(res))
print("savestates sent (time s, frame, uncompressed, on wire):")
for l in loads: print("  ", l)
