"""Split a RetroArch netplay capture (Netplay host on 55435) into what replay.c
needs: state<frame>.bin per LOAD_SAVESTATE (the core bytes from the MEM block),
inputs.txt ("frame host client" joypad words) and crcs.txt ("frame crc").

    replay_extract.py RADECODE_PY CAPTURE.pcap OUTDIR

Reassembles TCP by sequence offset; radecode.py streams() misreads a capture
with reordered segments."""
import sys, struct, zlib, importlib.util, json
spec = importlib.util.spec_from_file_location("rd", sys.argv[1]); rd = importlib.util.module_from_spec(spec); spec.loader.exec_module(rd)
od = sys.argv[3]
segs = {}
for ts, s_, d_, seq, flags, payload in rd.tcp_segments(sys.argv[2]):
    k = (s_, d_)
    if flags & 2: segs.setdefault(k, {"isn": seq + 1, "data": {}}); continue
    if k not in segs or not payload: continue
    segs[k]["data"].setdefault((seq - segs[k]["isn"]) & 0xffffffff, payload)
out = {}
for k, v in segs.items():
    b = bytearray()
    for off in sorted(v["data"]):
        p = v["data"][off]
        if off + len(p) <= len(b): continue
        b += p[len(b) - off:]
    out[k] = [(0, bytes(b), None)]
def cmds(buf):
    off = 24
    while off + 8 <= len(buf):
        c, n = struct.unpack(">II", buf[off:off+8]); yield c, buf[off+8:off+8+n]; off += 8 + n
host = {}; client = {}; crcs = []; loads = []; ev = []
for (s, d), chunks in out.items():
    buf = b"".join(c for _, c, _ in chunks)
    if s[1] == 55435:
        for c, b in cmds(buf):
            if c == 3: f, cl, w = struct.unpack(">III", b[:12]); host[f] = w
            elif c == 0x40: crcs.append(struct.unpack(">II", b))
            elif c == 0x42:
                f, raw = struct.unpack(">II", b[:8]); st = zlib.decompress(b[8:])
                core = st[16:16 + struct.unpack("<I", st[12:16])[0]]
                open("%s/state%d.bin" % (od, f), "wb").write(core); loads.append(f)
            elif c == 0x26: ev.append(("MODE", struct.unpack(">III", b[:12])))
            elif c == 0x2001: ev.append(("LAT", struct.unpack(">ii", b)))
            elif c == 0x23: ev.append(("SYNC", struct.unpack(">I", b[:4])))
            elif c == 0x45: ev.append(("STALL", struct.unpack(">I", b)))
    elif d[1] == 55435:
        for c, b in cmds(buf):
            if c == 3: f, cl, w = struct.unpack(">III", b[:12]); client[f] = w
            elif c == 0x41: ev.append(("REQ", None))
print("host inputs", min(host), max(host), len(host), "client", min(client) if client else None, max(client) if client else None, len(client))
print("loads", loads); print("crcs", crcs[:6]); print("events", [e for e in ev if e[0] not in ("STALL","REQ")], "stalls", sum(1 for e in ev if e[0]=="STALL"))
with open(od + "/inputs.txt", "w") as f:
    for fr in range(min(host), max(host) + 1):
        f.write("%d %d %d\n" % (fr, host.get(fr, 0), client.get(fr, 0)))
with open(od + "/crcs.txt", "w") as f:
    for fr, c in crcs: f.write("%d %08x\n" % (fr, c))
print("missing host frames", [x for x in range(min(host), max(host)) if x not in host][:5])
