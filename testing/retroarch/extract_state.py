"""Write the first LOAD_SAVESTATE a RetroArch host sent in a capture, inflated.

    extract_state.py RADECODE_PY CAPTURE.pcap OUT.bin

RADECODE_PY is testing/results/tools/radecode.py (its pcap and TCP reassembly).
Exits 1 when the capture holds no savestate."""
import importlib.util
import struct
import sys
import zlib

spec = importlib.util.spec_from_file_location("rd", sys.argv[1]); rd = importlib.util.module_from_spec(spec); spec.loader.exec_module(rd)
out, t0 = rd.streams(sys.argv[2])
for (s, d), chunks in out.items():
    if s[1] != 55435: continue
    buf = b"".join(c for _, c, _ in chunks); off = 24
    while off + 8 <= len(buf):
        cmd, size = struct.unpack(">II", buf[off:off+8]); body = buf[off+8:off+8+size]
        if cmd == 0x42:
            open(sys.argv[3], "wb").write(zlib.decompress(body[8:])); sys.exit(0)
        off += 8 + size
sys.exit(1)
