#!/usr/bin/env python3
"""Test relay, WSL side: TCP listener on 127.0.0.1:TCP_PORT (the far end of an
ssh -R tunnel) <-> UDP to 127.0.0.1:UDP_PORT. 2-byte length framing."""
import select, socket, struct, sys
tcp_port, udp_port = int(sys.argv[1]), int(sys.argv[2])
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", tcp_port)); srv.listen(1)
while True:
    c, _ = srv.accept(); c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.connect(("127.0.0.1", udp_port))
    print("tunnel up", flush=True)
    buf = b""
    try:
        while True:
            r, _, _ = select.select([c, u], [], [])
            if u in r:
                d = u.recv(65536)
                c.sendall(struct.pack(">H", len(d)) + d)
            if c in r:
                d = c.recv(65536)
                if not d: break
                buf += d
                while len(buf) >= 2:
                    n = struct.unpack(">H", buf[:2])[0]
                    if len(buf) < 2 + n: break
                    u.send(buf[2:2 + n]); buf = buf[2 + n:]
    except OSError as e:
        print("tunnel down:", e, flush=True)
    c.close(); u.close()
