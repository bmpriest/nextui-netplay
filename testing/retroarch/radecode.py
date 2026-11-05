"""Decode a RetroArch netplay TCP session (port 55435) from a classic pcap:
every command each way, the first LIMIT of the frequent ones (default 3).
  radecode.py CAPTURE.pcap [LIMIT]
The other tools here (rasum, rollback, ra_checks, ../spruce/table.py) load it
for its stream reassembly."""
import struct
import sys
from collections import Counter, defaultdict

PORT = 55435
CMDS = {
    0x0000: "ACK", 0x0001: "NAK", 0x0002: "DISCONNECT", 0x0003: "INPUT",
    0x0004: "NOINPUT", 0x0020: "NICK", 0x0021: "PASSWORD", 0x0022: "INFO",
    0x0023: "SYNC", 0x0024: "SPECTATE", 0x0025: "PLAY", 0x0026: "MODE",
    0x0027: "MODE_REFUSED", 0x0040: "CRC", 0x0041: "REQUEST_SAVESTATE",
    0x0042: "LOAD_SAVESTATE", 0x0043: "PAUSE", 0x0044: "RESUME",
    0x0045: "STALL", 0x0046: "RESET", 0x0047: "CHEATS", 0x0048: "NETPACKET",
    0x0061: "CFG", 0x0062: "CFG_ACK", 0x1000: "PLAYER_CHAT",
    0x1100: "PING_REQUEST", 0x1101: "PING_RESPONSE",
    0x2000: "SETTING_ALLOW_PAUSING", 0x2001: "SETTING_INPUT_LATENCY_FRAMES",
}


def packets(path):
    data = open(path, "rb").read()
    magic = struct.unpack("<I", data[:4])[0]
    endian = "<" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">"
    nano = magic in (0xA1B23C4D, 0x4D3CB2A1)
    off = 24
    while off + 16 <= len(data):
        sec, frac, incl, _ = struct.unpack(endian + "IIII", data[off:off + 16])
        off += 16
        yield sec + frac / (1e9 if nano else 1e6), data[off:off + incl]
        off += incl


def tcp_segments(path):
    for ts, frame in packets(path):
        etype = struct.unpack(">H", frame[12:14])[0]
        p = frame[14:]
        if etype == 0x0800:
            ihl = (p[0] & 15) * 4
            if p[9] != 6:
                continue
            src, dst = ".".join(map(str, p[12:16])), ".".join(map(str, p[16:20]))
            total = struct.unpack(">H", p[2:4])[0]
            t = p[ihl:total]
        else:
            continue
        sport, dport, seq = struct.unpack(">HHI", t[:8])
        flags = t[13]
        doff = (t[12] >> 4) * 4
        if PORT not in (sport, dport):
            continue
        yield ts, (src, sport), (dst, dport), seq, flags, t[doff:]


def streams(path):
    out = defaultdict(list)          # direction -> [(ts, bytes)]
    nxt = {}
    first_ts = None
    for ts, s, d, seq, flags, payload in tcp_segments(path):
        first_ts = first_ts or ts
        key = (s, d)
        if flags & 0x02:             # SYN
            nxt[key] = seq + 1
            out[key].append((ts, b"", "SYN"))
            continue
        if flags & 0x01:
            out[key].append((ts, b"", "FIN"))
        if not payload:
            continue
        exp = nxt.get(key, seq)
        if seq + len(payload) <= exp:
            continue                 # retransmission
        if seq < exp:
            payload = payload[exp - seq:]
        nxt[key] = exp + len(payload)
        out[key].append((ts, payload, None))
    return out, first_ts


def cstr(b):
    return b.split(b"\0", 1)[0].decode("latin-1")


def describe(cmd, body):
    name = CMDS.get(cmd, f"0x{cmd:04x}")
    if name == "NICK":
        return f"nick={cstr(body)!r}"
    if name == "INFO":
        crc = struct.unpack(">I", body[:4])[0]
        return (f"content_crc={crc:08x} core={cstr(body[4:36])!r} "
                f"version={cstr(body[36:68])!r}")
    if name in ("INPUT", "NOINPUT", "CRC", "REQUEST_SAVESTATE", "PAUSE",
                "RESUME", "STALL", "RESET", "MODE", "PING_REQUEST",
                "PING_RESPONSE", "SETTING_ALLOW_PAUSING",
                "SETTING_INPUT_LATENCY_FRAMES", "MODE_REFUSED", "PLAY"):
        n = len(body) // 4
        return "words=" + " ".join(f"{w:08x}" for w in struct.unpack(f">{n}I", body[:4 * n]))
    if name == "SYNC":
        n = min(len(body) // 4, 12)
        return (f"{len(body)} bytes; first words="
                + " ".join(f"{w:08x}" for w in struct.unpack(f">{n}I", body[:4 * n])))
    if name == "LOAD_SAVESTATE":
        frame, size = struct.unpack(">II", body[:8])
        return f"frame={frame} uncompressed={size} payload={len(body)}"
    return f"{len(body)} bytes"


def decode(path, limit):
    out, t0 = streams(path)
    for key, chunks in out.items():
        (s, sp), (d, dp) = key
        buf = b"".join(c for _, c, _ in chunks)
        # Map each byte offset to the time its segment arrived.
        times, pos = [], 0
        for ts, c, _ in chunks:
            times.append((pos, ts))
            pos += len(c)

        def at(o):
            t = t0
            for p, ts in times:
                if p > o:
                    break
                t = ts
            return t - t0

        print(f"\n=== {s}:{sp} -> {d}:{dp}  ({len(buf)} bytes)")
        if len(buf) < 24:
            continue
        hdr = struct.unpack(">6I", buf[:24])
        print(f"  [{at(0):7.3f}] HEADER magic={hdr[0]:08x} ({buf[:4]!r}) platform={hdr[1]:08x} "
              f"compression={hdr[2]:08x} salt/proto={hdr[3]:08x} impl={hdr[4]:08x} w5={hdr[5]:08x}")
        off, counts, shown = 24, Counter(), Counter()
        inputs = []
        while off + 8 <= len(buf):
            cmd, size = struct.unpack(">II", buf[off:off + 8])
            body = buf[off + 8:off + 8 + size]
            if len(body) < size:
                print(f"  truncated {CMDS.get(cmd, hex(cmd))} at {off}")
                break
            name = CMDS.get(cmd, f"0x{cmd:04x}")
            counts[name] += 1
            if name == "INPUT":
                inputs.append((at(off), body))
            if shown[name] < limit or name not in ("INPUT", "NOINPUT", "CRC"):
                print(f"  [{at(off):7.3f}] {name} len={size} {describe(cmd, body)}")
                shown[name] += 1
            off += 8 + size
        if off != len(buf):
            print(f"  {len(buf) - off} trailing bytes")
        print("  counts:", dict(counts))
        if len(inputs) > 1:
            frames = [struct.unpack(">I", b[:4])[0] for _, b in inputs]
            span = inputs[-1][0] - inputs[0][0]
            print(f"  INPUT frames {frames[0]}..{frames[-1]} over {span:.1f}s "
                  f"({(frames[-1] - frames[0]) / span if span else 0:.1f} frames/s), "
                  f"sizes {sorted(Counter(len(b) for _, b in inputs).items())}")
            nonzero = Counter(b[8:] for _, b in inputs)
            print(f"  distinct input payloads: {len(nonzero)}; most common "
                  + ", ".join(f"{p.hex()}x{c}" for p, c in nonzero.most_common(4)))


if __name__ == "__main__":
    decode(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 3)
