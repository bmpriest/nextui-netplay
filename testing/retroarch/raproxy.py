"""Forward one RetroArch netplay connection, rewriting the protocol the
client offers (header word 3) so the server picks an older one.
  raproxy.py LISTEN_PORT SERVER PROTOCOL
Listens on 127.0.0.1:LISTEN_PORT; point a client there and it reaches SERVER
port 55435 offering PROTOCOL."""
import socket, struct, sys, threading
listen_port, target, proto = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", listen_port)); srv.listen(1)
print("listening", flush=True)
c, _ = srv.accept()
u = socket.create_connection((target, 55435))
for s in (c, u): s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
hdr = b""
while len(hdr) < 24:
    part = c.recv(24 - len(hdr))
    if not part: sys.exit(1)
    hdr += part
words = list(struct.unpack(">6I", hdr))
print("client header", [hex(w) for w in words], flush=True)
words[3] = proto
u.sendall(struct.pack(">6I", *words))
def pump(a, b):
    try:
        while True:
            d = a.recv(65536)
            if not d: break
            b.sendall(d)
    except OSError: pass
    for s in (a, b):
        try: s.shutdown(socket.SHUT_RDWR)
        except OSError: pass
t = threading.Thread(target=pump, args=(u, c)); t.start()
pump(c, u); t.join()
print("closed", flush=True)
