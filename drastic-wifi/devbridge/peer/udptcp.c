/* Test relay: UDP on 127.0.0.1:<udp_port> <-> TCP to 127.0.0.1:<tcp_port>
 * (an ssh -R tunnel). Datagrams are framed with a 2-byte length. One UDP peer
 * (the last sender) gets the replies. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int read_full(int fd, unsigned char *p, int n) {
	while (n > 0) { int r = (int)read(fd, p, (size_t)n); if (r <= 0) return -1; p += r; n -= r; }
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 3) { fprintf(stderr, "udptcp UDP_PORT TCP_PORT\n"); return 2; }
	int u = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short)atoi(argv[1]));
	if (bind(u, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
	struct sockaddr_in peer = {0}; socklen_t peerlen = 0;
	int t = -1;
	unsigned char buf[65536 + 2];
	for (;;) {
		if (t < 0) {
			t = socket(AF_INET, SOCK_STREAM, 0);
			struct sockaddr_in b = {0}; b.sin_family = AF_INET; b.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			b.sin_port = htons((unsigned short)atoi(argv[2]));
			if (connect(t, (struct sockaddr *)&b, sizeof b) < 0) { close(t); t = -1; sleep(1); continue; }
			int one = 1; setsockopt(t, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
			fprintf(stderr, "udptcp: tunnel up\n");
		}
		struct pollfd p[2] = {{u, POLLIN, 0}, {t, POLLIN, 0}};
		if (poll(p, 2, 1000) <= 0) continue;
		if (p[0].revents & POLLIN) {
			socklen_t l = sizeof peer;
			int n = (int)recvfrom(u, buf + 2, 65536, 0, (struct sockaddr *)&peer, &l);
			peerlen = l;
			if (n > 0) { buf[0] = (unsigned char)(n >> 8); buf[1] = (unsigned char)n; if (write(t, buf, (size_t)n + 2) < 0) { close(t); t = -1; } }
		}
		if (t >= 0 && (p[1].revents & (POLLIN | POLLHUP))) {
			unsigned char h[2];
			if (read_full(t, h, 2) < 0) { close(t); t = -1; continue; }
			int n = (h[0] << 8) | h[1];
			if (read_full(t, buf, n) < 0) { close(t); t = -1; continue; }
			if (peerlen) sendto(u, buf, (size_t)n, 0, (struct sockaddr *)&peer, peerlen);
		}
	}
}
