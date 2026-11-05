#!/usr/bin/env python3
"""A model of RetroArch 1.22.2's netplay peer, for testing the shim's
RetroArch protocol (shim/raproto.c) on the host.

It plays either role over TCP against a shim running the fake core
(shim/test/fake_core.c with FAKE_STATE_INPUTS=1), and enforces the rules that
make RetroArch NAK or hang up, as written in network/netplay/netplay_frontend.c
at v1.22.2: handshake sizes and order, INPUT strictly in frame order,
MODE / LOAD_SAVESTATE / RESET only at the receiver's next expected host input
frame, and so on. Any violation fails the run.

What it does not model is RetroArch's rollback: it never runs a frame on a
predicted input. It simulates the fake core only over frames whose inputs
from both sides are known, so every CRC it computes or checks is over the
true state - which is what lets it prove the two sides agree.

    ra_model.py host   PORT  [--frames N] [--request-at F ...]
    ra_model.py client HOST PORT [--frames N]

Prints one 'model: ...' summary line and exits 0 on success.
"""
import argparse
import select
import socket
import struct
import sys
import time
import zlib

MAGIC, FULL = 0x52414E50, 0x46554C4C
CMD = dict(ACK=0x0000, NAK=0x0001, DISCONNECT=0x0002, INPUT=0x0003, NOINPUT=0x0004,
           NICK=0x0020, PASSWORD=0x0021, INFO=0x0022, SYNC=0x0023, SPECTATE=0x0024,
           PLAY=0x0025, MODE=0x0026, MODE_REFUSED=0x0027, CRC=0x0040,
           REQUEST_SAVESTATE=0x0041, LOAD_SAVESTATE=0x0042, PAUSE=0x0043,
           RESUME=0x0044, STALL=0x0045, RESET=0x0046,
           PING_REQUEST=0x1100, PING_RESPONSE=0x1101,
           SETTING_ALLOW_PAUSING=0x2000, SETTING_INPUT_LATENCY_FRAMES=0x2001)
NAME = {v: k for k, v in CMD.items()}
NICK_LEN = 32
SYNC_FIXED = 8 + 64 + 16 + 64 + 32
MODE_YOU, MODE_PLAYING = 1 << 31, 1 << 30
CORE_NAME, CORE_VERSION = b"FakeCore", b"9.9"
CHECK_FRAMES = 120   # our netplay_check_frames
FPS = 60.0


def impl_magic(ver=b"1.22.2", proto=7):
    res = 0
    for i, c in enumerate(ver):
        res ^= c << (i & 0xF)
    return (res ^ (proto << (len(ver) & 0xF))) & 0xFFFFFFFF


PLATFORM = (8 << 15) | 8       # 64-bit little-endian, as an AArch64 handheld


class Violation(Exception):
    pass


class FakeCore:
    """shim/test/fake_core.c with FAKE_STATE_INPUTS: byte 1 counts frames,
    bytes 4..7 fold both ports' input, byte 8 is the savestate context the
    frontend reported when the state was taken (3 = ROLLBACK_NETPLAY)."""

    @staticmethod
    def initial():
        b = bytearray([0xAB] * 16)
        b[8] = 3
        return bytes(b)

    @staticmethod
    def run(state, p0, p1):
        b = bytearray(state)
        b[1] = (b[1] + 1) & 0xFF
        h = struct.unpack("<I", bytes(b[4:8]))[0]
        h = ((h ^ (p0 & 0xFFFF)) * 16777619) & 0xFFFFFFFF
        h = ((h ^ (p1 & 0xFFFF)) * 16777619) & 0xFFFFFFFF
        b[4:8] = struct.pack("<I", h)
        b[8] = 3
        return bytes(b)


def container(core):
    """netplay_build_savestate with achievements compiled in, disabled."""
    pad = (-len(core)) % 8
    return (b"NETPLAY\x01" + b"MEM " + struct.pack("<I", len(core)) + core + b"\0" * pad
            + b"ACHV" + struct.pack("<I", 8) + b"\0" * 8 + b"END " + b"\0" * 4)


def uncontain(raw):
    if raw[:7] != b"NETPLAY":
        return raw
    if raw[7] != 1:
        raise Violation("unknown savestate container version %d" % raw[7])
    off = 8
    while off + 8 <= len(raw):
        tag, size = raw[off:off + 4], struct.unpack("<I", raw[off + 4:off + 8])[0]
        off += 8
        if tag == b"MEM ":
            return raw[off:off + size]
        off += size + ((-size) % 8)
    raise Violation("savestate container without a MEM block")


def host_input(frame):
    return (frame * 37 + (frame >> 4)) & 0x0FF0


def client_input(frame):
    return ((frame * 11) ^ (frame >> 3)) & 0x00FF


class Conn:
    def __init__(self, sock):
        self.s = sock
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        self.closed = False

    def send(self, cmd, payload=b""):
        self.s.sendall(struct.pack(">II", cmd, len(payload)) + payload)

    def fill(self, timeout):
        r, _, _ = select.select([self.s], [], [], timeout)
        if r:
            d = self.s.recv(1 << 20)
            if not d:
                self.closed = True
            self.buf += d

    def take(self, n, timeout=5.0):
        end = time.time() + timeout
        while len(self.buf) < n:
            if self.closed or time.time() > end:
                raise Violation("peer went quiet (%d of %d bytes)" % (len(self.buf), n))
            self.fill(0.05)
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def next_cmd(self):
        """A whole command if one is buffered, else None."""
        if len(self.buf) < 8:
            return None
        cmd, size = struct.unpack(">II", self.buf[:8])
        if len(self.buf) < 8 + size:
            return None
        body = self.buf[8:8 + size]
        self.buf = self.buf[8 + size:]
        return cmd, body

    def expect(self, cmd, timeout=5.0):
        hdr = self.take(8, timeout)
        c, size = struct.unpack(">II", hdr)
        if c != cmd:
            raise Violation("expected %s, got %s" % (NAME.get(cmd), NAME.get(c, hex(c))))
        return self.take(size, timeout)


def check_header(h, server_side):
    w = struct.unpack(">6I", h)
    if w[0] != MAGIC:
        raise Violation("bad magic %08x" % w[0])
    if (w[1] >> 30) & 1:
        raise Violation("peer claims big-endian")
    if w[5] != impl_magic():
        print("model: note: implementation magic %08x is not 1.22.2's %08x" % (w[5], impl_magic()))
    return w


def check_info(body):
    if len(body) != 4 + 2 * NICK_LEN:
        raise Violation("INFO payload %d bytes" % len(body))
    name = body[4:4 + NICK_LEN].split(b"\0")[0]
    if name.lower() != CORE_NAME.lower():
        raise Violation("core name %r" % name)
    return struct.unpack(">I", body[:4])[0]


def info_payload():
    return struct.pack(">I", 0x12345678) + CORE_NAME.ljust(NICK_LEN, b"\0") + CORE_VERSION.ljust(NICK_LEN, b"\0")


class Stats:
    def __init__(self):
        self.crc_ok = self.crc_bad = self.loads = self.requests = 0
        self.confirmed = 0
        self.pings = 0
        self.stalls = 0
        self.pauses = 0
        self.predicted = 0
        self.run_frames = 0


# --------------------------------------------------------------------------
# RetroArch as host (the shim joins)
# --------------------------------------------------------------------------

def run_host(port, frames, request_at, max_proto):
    st = Stats()
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", port))
    ls.listen(1)
    ls.settimeout(20)
    c = Conn(ls.accept()[0])

    # netplay_handshake_init (server side)
    w = check_header(c.take(24), True)
    hi, lo = w[3], w[4]
    proto = lo if not hi else min(hi, max_proto)
    if not 5 <= proto <= 7:
        raise Violation("protocol %d" % proto)
    c.s.sendall(struct.pack(">6I", MAGIC, PLATFORM, 1, 0, proto, impl_magic()))
    zlib_ok = bool(w[2] & 1)
    nick = c.expect(CMD["NICK"])
    if len(nick) != NICK_LEN:
        raise Violation("NICK size")
    c.send(CMD["NICK"], b"Anonymous".ljust(NICK_LEN, b"\0"))
    c.send(CMD["INFO"], info_payload())
    check_info(c.expect(CMD["INFO"]))

    # netplay_handshake_sync
    self_frame = 1000        # a host that has been running for a while
    sram = bytes([0x5A] * 16)
    devices = struct.pack(">16I", *([1] * 16))
    dclients = struct.pack(">16I", 1, *([0] * 15))
    body = struct.pack(">II", self_frame, 1) + devices + b"\0" * 16 + dclients \
        + nick.split(b"\0")[0][:28].ljust(NICK_LEN, b"\0") + sram
    c.send(CMD["SYNC"], body)
    if proto >= 6:
        c.send(CMD["SETTING_ALLOW_PAUSING"], struct.pack(">I", 1))
        c.send(CMD["SETTING_INPUT_LATENCY_FRAMES"], struct.pack(">ii", 0, 0))

    state = FakeCore.initial()
    sim_frame = self_frame       # first frame the simulation has not run
    states = {sim_frame: state}  # state before each simulated frame
    client_playing_from = None   # MODE frame
    client_next = None           # read_frame_count[1]
    client_inputs = {}
    sent_input_to = self_frame   # next host input frame to send
    want_state = True            # netplay_handshake_ready: force_send_savestate
    requests = list(request_at)
    paused = False
    t0 = time.time()
    last_ping = time.time()

    def send_state(frame):
        raw = container(states[frame]) if proto >= 7 else states[frame]
        data = zlib.compress(raw) if zlib_ok else raw
        c.send(CMD["LOAD_SAVESTATE"], struct.pack(">II", frame, len(raw)) + data)
        st.loads += 1

    while st.confirmed < frames:
        if time.time() - t0 > frames / FPS * 3 + 20:
            raise Violation("too slow: %d of %d frames confirmed" % (st.confirmed, frames))
        c.fill(0.002)
        if c.closed:
            raise Violation("shim closed the connection")
        while True:
            m = c.next_cmd()
            if not m:
                break
            cmd, b = m
            n = NAME.get(cmd, hex(cmd))
            if n == "PLAY":
                if client_playing_from is not None:
                    raise Violation("PLAY from a client already playing")
                if len(b) != 4:
                    raise Violation("PLAY size")
                client_playing_from = client_next = sent_input_to
                p = struct.pack(">III", client_playing_from, MODE_YOU | MODE_PLAYING | 1, 2) \
                    + b"\0" * 16 + b"shim".ljust(NICK_LEN, b"\0")
                c.send(CMD["MODE"], p)
            elif n == "INPUT":
                if client_playing_from is None:
                    raise Violation("INPUT from a spectator")
                if len(b) != 12:
                    raise Violation("INPUT payload %d bytes for one joypad" % len(b))
                f, cl, word = struct.unpack(">III", b)
                if f < client_next:
                    continue
                if f > client_next:
                    raise Violation("input out of order: %d, expected %d" % (f, client_next))
                client_inputs[f] = word
                client_next += 1
            elif n == "REQUEST_SAVESTATE":
                want_state = True
                st.requests += 1
            elif n == "PING_REQUEST":
                c.send(CMD["PING_RESPONSE"])
                st.pings += 1
            elif n == "PING_RESPONSE":
                pass
            elif n == "PAUSE":
                # RetroArch 1.22.2's remote_unpaused tests
                # flags & ((ACTIVE|PAUSED) == (ACTIVE|PAUSED)), i.e. flags & 1,
                # so a peer's RESUME never unpauses it. Modelled faithfully.
                paused = True
                st.pauses += 1
            elif n == "RESUME":
                pass
            elif n == "DISCONNECT":
                raise Violation("shim disconnected")
            else:
                raise Violation("unexpected %s from a client" % n)

        # Simulate every frame whose inputs are known.
        while sim_frame < sent_input_to:
            if client_playing_from is not None and sim_frame >= client_playing_from:
                if sim_frame not in client_inputs:
                    break
                p1 = client_inputs[sim_frame]
            else:
                p1 = 0
            state = FakeCore.run(state, host_input(sim_frame), p1)
            sim_frame += 1
            states[sim_frame] = state
            if client_playing_from is not None:
                st.confirmed += 1
            if sim_frame % CHECK_FRAMES == 0:
                c.send(CMD["CRC"], struct.pack(">II", sim_frame, zlib.crc32(states[sim_frame])))
            states.pop(sim_frame - 200, None)

        # Advance our own clock in real time, but never further than 60
        # frames past the client's input (NETPLAY_MAX_STALL_FRAMES).
        due = self_frame + int((time.time() - t0) * FPS)
        if paused:
            t0 += 0.002
            continue
        limit = (client_next + 60) if client_next is not None else sent_input_to + 1
        if client_next is not None and client_next > sent_input_to + 3:
            due = max(due, sent_input_to + 2)   # netplay catch-up
        while sent_input_to < min(due, limit):
            if requests and sent_input_to >= requests[0]:
                requests.pop(0)
                want_state = True
            if want_state:
                # A host that never predicts sends a state only for a frame it
                # has actually simulated.
                if sim_frame != sent_input_to:
                    break
                send_state(sent_input_to)
                want_state = False
            c.send(CMD["INPUT"], struct.pack(">III", sent_input_to, 0, host_input(sent_input_to)))
            sent_input_to += 1
        if sent_input_to >= limit:
            st.stalls += 1
        if time.time() - last_ping > 3:
            c.send(CMD["PING_REQUEST"])
            last_ping = time.time()

    c.send(CMD["DISCONNECT"])
    return st


# --------------------------------------------------------------------------
# RetroArch as client (the shim hosts)
# --------------------------------------------------------------------------

def run_client(host, port, frames, request_at, max_proto, no_predict=False, fps=FPS):
    st = Stats()
    end = time.time() + 20
    while True:
        try:
            s = socket.create_connection((host, port), timeout=2)
            break
        except OSError:
            if time.time() > end:
                raise Violation("cannot connect")
            time.sleep(0.2)
    c = Conn(s)
    c.s.sendall(struct.pack(">6I", MAGIC, PLATFORM, 1, max_proto, 5, impl_magic()))
    w = check_header(c.take(24), False)
    if w[3]:
        raise Violation("host asks for a password")
    proto = w[4]
    if not 5 <= proto <= max_proto:
        raise Violation("host picked protocol %d" % proto)
    zlib_ok = bool(w[2] & 1)
    c.send(CMD["NICK"], b"Anonymous".ljust(NICK_LEN, b"\0"))
    if len(c.expect(CMD["NICK"])) != NICK_LEN:
        raise Violation("NICK size")
    check_info(c.expect(CMD["INFO"]))
    c.send(CMD["INFO"], info_payload())

    # netplay_handshake_pre_sync
    body = c.expect(CMD["SYNC"], 10)
    if len(body) < SYNC_FIXED:
        raise Violation("SYNC %d bytes" % len(body))
    server_frame, client_num = struct.unpack(">II", body[:8])
    devices = struct.unpack(">16I", body[8:72])
    dclients = struct.unpack(">16I", body[88:152])
    if client_num & 0x7FFFFFFF != 1 or devices[0] != 1 or dclients[0] != 1:
        raise Violation("SYNC: client %d devices %s clients %s" % (client_num, devices[:2], dclients[:2]))
    c.send(CMD["PLAY"], struct.pack(">I", 0))      # as spruce's RetroArch sends it

    host_inputs = {}
    state = None
    sim_frame = None
    states = {}
    crcs = {}
    playing_from = None
    self_frame = server_frame   # our own input clock
    next_send = None
    requests = list(request_at)
    outstanding = False
    t0 = None
    latency = 0
    next_tick = time.time()
    peer_paused = False
    stall = 0

    def check_crc(f, crc):
        nonlocal outstanding
        if crc == zlib.crc32(states[f]):
            st.crc_ok += 1
            return
        st.crc_bad += 1
        print("model: CRC mismatch at frame %d" % f, flush=True)
        if not outstanding:
            c.send(CMD["REQUEST_SAVESTATE"])
            outstanding = True

    def violation(msg):
        c.send(CMD["NAK"])
        raise Violation(msg)

    deadline = time.time() + frames / FPS * 3 + 25
    while st.confirmed < frames:
        if time.time() > deadline:
            raise Violation("too slow: %d of %d frames confirmed" % (st.confirmed, frames))
        c.fill(0.002)
        if c.closed:
            raise Violation("shim closed the connection")
        while True:
            m = c.next_cmd()
            if not m:
                break
            cmd, b = m
            n = NAME.get(cmd, hex(cmd))
            if n == "INPUT":
                f, cl, word = struct.unpack(">III", b)
                if cl != 0:
                    violation("host input for client %d" % cl)
                if f < server_frame:
                    continue
                if f > server_frame:
                    violation("host input out of order: %d, expected %d" % (f, server_frame))
                host_inputs[f] = word
                server_frame += 1
            elif n == "LOAD_SAVESTATE":
                f, raw_len = struct.unpack(">II", b[:8])
                if f != server_frame:
                    violation("state load out of order: frame %d, expected %d" % (f, server_frame))
                raw = zlib.decompress(b[8:]) if zlib_ok else b[8:]
                if len(raw) != raw_len:
                    violation("state size")
                core = uncontain(raw)
                if proto < 7 and raw[:7] == b"NETPLAY":
                    violation("container sent to a protocol %d peer" % proto)
                if len(core) != 16:
                    violation("core state %d bytes" % len(core))
                state, sim_frame = core, f
                states = {f: core}
                st.loads += 1
                outstanding = False
                if t0 is None:
                    t0 = time.time()
                    self_frame = f
            elif n == "MODE":
                f, mode, devs = struct.unpack(">III", b[:12])
                if f != server_frame:
                    violation("mode change out of order: frame %d, expected %d" % (f, server_frame))
                if mode & MODE_YOU and mode & MODE_PLAYING:
                    if (mode & 0xFFFF) != 1 or devs != 2:
                        violation("MODE gives client %d devices %#x" % (mode & 0xFFFF, devs))
                    playing_from = f
                    next_send = f
                    # "It wanted past frames, better send 'em!": zeros up to
                    # where our own clock already is.
                    while next_send < self_frame:
                        mine[next_send] = 0
                        c.send(CMD["INPUT"], struct.pack(">III", next_send, 1, 0))
                        next_send += 1
            elif n == "CRC":
                f, crc = struct.unpack(">II", b)
                if sim_frame is not None and f < sim_frame:
                    if f in states:        # still in our buffer: check now
                        check_crc(f, crc)
                else:
                    crcs[f] = crc
            elif n == "SETTING_ALLOW_PAUSING":
                pass
            elif n == "SETTING_INPUT_LATENCY_FRAMES":
                # The client honours the host's minimum: its emulated frame
                # runs that far behind the input frame it samples and sends.
                latency = max(0, struct.unpack(">ii", b)[0])
            elif n == "PING_REQUEST":
                c.send(CMD["PING_RESPONSE"])
            elif n == "PING_RESPONSE":
                st.pings += 1
            elif n == "PAUSE":
                peer_paused = True             # and never resumed: see run_host
                st.pauses += 1
            elif n == "RESUME":
                pass
            elif n == "STALL":
                stall = min(struct.unpack(">I", b)[0], 60)
                st.stalls += 1
            elif n == "NOINPUT":
                violation("NOINPUT from a playing host")
            elif n == "DISCONNECT":
                raise Violation("shim host disconnected")
            else:
                violation("unexpected %s from a host" % n)

        # Simulate what both inputs are known for; check CRCs on the way.
        while sim_frame is not None and sim_frame in host_inputs:
            if playing_from is not None and sim_frame >= playing_from:
                if sim_frame not in mine:
                    break
                p1 = mine[sim_frame]
            else:
                p1 = 0
            states[sim_frame] = state
            states.pop(sim_frame - 300, None)
            if sim_frame in crcs:
                check_crc(sim_frame, crcs.pop(sim_frame))
            state = FakeCore.run(state, host_inputs[sim_frame], p1)
            sim_frame += 1
            if playing_from is not None and sim_frame > playing_from:
                st.confirmed += 1

        # Our own clock: real time, with RetroArch's catch-up when the host's
        # input is well ahead, and its stall when we are far ahead of it.
        # Like RetroArch, the clock runs from the first state on, player or not.
        if t0 is not None and not peer_paused:
            now = time.time()
            steps = 0
            while next_tick <= now:
                steps += 1
                next_tick += 1.0 / fps
            steps = min(steps, 2)
            if server_frame > self_frame + 3:
                steps = max(steps, 2)          # netplay catch-up
            if stall and steps:
                stall -= 1
                steps = 0
            if self_frame >= server_frame + 60:
                steps = 0                      # NETPLAY_STALL_RUNNING_FAST
            if no_predict:
                # A client that never runs a frame before the host's input for
                # it is here: the strictest pacing a host can meet, and the one
                # that exposes a host waiting on the client before it has sent
                # input up to the client's MODE frame.
                steps = min(steps, max(0, server_frame - (self_frame - latency)))
            for _ in range(steps):
                # RetroArch runs frame self_frame - latency now; without the
                # host's input for it, it predicts (and later rolls back).
                if self_frame - latency >= server_frame:
                    st.predicted += 1
                st.run_frames += 1
                if next_send is not None and self_frame >= next_send:
                    mine[self_frame] = client_input(self_frame)
                    c.send(CMD["INPUT"], struct.pack(">III", self_frame, 1, mine[self_frame]))
                    next_send = self_frame + 1
                self_frame += 1
                if requests and self_frame >= requests[0]:
                    requests.pop(0)
                    c.send(CMD["REQUEST_SAVESTATE"])
                    st.requests += 1
        else:
            next_tick = time.time()
        time.sleep(0.001)

    c.send(CMD["DISCONNECT"])
    return st


mine = {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("role", choices=["host", "client"])
    ap.add_argument("args", nargs="+")
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--request-at", type=int, action="append", default=[])
    ap.add_argument("--max-proto", type=int, default=7)
    ap.add_argument("--no-predict", action="store_true")
    ap.add_argument("--fps", type=float, default=FPS, help="client clock, frames per second")
    a = ap.parse_args()
    try:
        if a.role == "host":
            st = run_host(int(a.args[0]), a.frames, a.request_at, a.max_proto)
        else:
            st = run_client(a.args[0], int(a.args[1]), a.frames, a.request_at, a.max_proto,
                            a.no_predict, a.fps)
    except Violation as e:
        print("model: VIOLATION: %s" % e, flush=True)
        return 1
    print("model: ok confirmed=%d crc_ok=%d crc_bad=%d loads=%d requests=%d pings=%d stalls=%d pauses=%d"
          " predicted=%d run=%d"
          % (st.confirmed, st.crc_ok, st.crc_bad, st.loads, st.requests, st.pings, st.stalls, st.pauses,
             st.predicted, st.run_frames), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
