/*
 * raproto - RetroArch netplay protocol 5-7 peer. See raproto.h and
 * docs/analysis/retroarch-protocol.md.
 *
 * Written from RetroArch's network/netplay/netplay_frontend.c at v1.22.2 and
 * checked against captures of two spruce devices running it. Function names
 * in comments are RetroArch's, so each rule can be found at its source.
 */

#include "raproto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <zlib.h>

/* netplay_private.h / netplay_frontend.c */
#define RA_MAGIC         0x52414E50u /* RANP */
#define RA_FULL_MAGIC    0x46554C4Cu /* FULL */
#define RA_POKE_MAGIC    0x504F4B45u /* POKE */
#define RA_BANNED_MAGIC  0x44454E59u /* DENY */
#define RA_LOW_PROTOCOL  5u
#define RA_HIGH_PROTOCOL 7u
#define RA_COMPRESSION_ZLIB 1u

enum {
	CMD_ACK = 0x0000, CMD_NAK = 0x0001, CMD_DISCONNECT = 0x0002,
	CMD_INPUT = 0x0003, CMD_NOINPUT = 0x0004,
	CMD_NICK = 0x0020, CMD_PASSWORD = 0x0021, CMD_INFO = 0x0022,
	CMD_SYNC = 0x0023, CMD_SPECTATE = 0x0024, CMD_PLAY = 0x0025,
	CMD_MODE = 0x0026, CMD_MODE_REFUSED = 0x0027,
	CMD_CRC = 0x0040, CMD_REQUEST_SAVESTATE = 0x0041,
	CMD_LOAD_SAVESTATE = 0x0042, CMD_PAUSE = 0x0043, CMD_RESUME = 0x0044,
	CMD_STALL = 0x0045, CMD_RESET = 0x0046, CMD_CHEATS = 0x0047,
	CMD_NETPACKET = 0x0048,
	CMD_CFG = 0x0061, CMD_CFG_ACK = 0x0062,
	CMD_PLAYER_CHAT = 0x1000,
	CMD_PING_REQUEST = 0x1100, CMD_PING_RESPONSE = 0x1101,
	CMD_SETTING_ALLOW_PAUSING = 0x2000,
	CMD_SETTING_INPUT_LATENCY_FRAMES = 0x2001,
};

/* struct info_buf_s: content CRC, core name, core version. */
#define INFO_PAYLOAD (4 + 2 * RAPROTO_NICK_LEN)
/* netplay_handshake_sync: frame, client number, 16 device types, 16 share
 * modes, 16 device-client masks, the client's nick, then SRAM. */
#define SYNC_FIXED (2 * 4 + RAPROTO_MAX_DEVICES * 4 + RAPROTO_MAX_DEVICES + \
                    RAPROTO_MAX_DEVICES * 4 + RAPROTO_NICK_LEN)
/* struct mode_payload */
#define MODE_PAYLOAD (3 * 4 + RAPROTO_MAX_DEVICES + RAPROTO_NICK_LEN)

#define RETRO_DEVICE_TYPE_MASK 0xff
#define DEV_JOYPAD   1
#define DEV_MOUSE    2
#define DEV_KEYBOARD 3
#define DEV_LIGHTGUN 4
#define DEV_ANALOG   5

#define INPUT_RING     1024
#define IO_TIMEOUT_MS  5000
#define CONNECT_RETRY_MS 500
#define CONNECT_GIVE_UP_MS 30000

typedef struct EventNode {
	RaProtoEvent ev;
	struct EventNode* next;
} EventNode;

static struct {
	RaProtoConfig cfg;
	pthread_t thread;
	int thread_started;
	volatile int stop;

	pthread_mutex_t lock;      /* everything below except fd writes */
	pthread_mutex_t send_lock; /* serialises writes to fd */
	int listen_fd;
	int fd;
	int connected;
	uint32_t protocol;
	uint32_t compression;
	uint32_t self_client;
	uint32_t remote_client;
	uint32_t config_devices[RAPROTO_MAX_DEVICES];
	uint32_t client_devices_remote; /* devices the peer's INPUT carries */
	bool host_allows_pausing;
	char peer_nick[RAPROTO_NICK_LEN];
	char client_nick[RAPROTO_NICK_LEN]; /* host: the nick we assigned */

	struct { uint32_t frame; uint32_t value; int valid; } remote[INPUT_RING];
	uint32_t remote_next;
	int remote_started;

	EventNode* head;
	EventNode* tail;

	struct timeval last_frame;
	int frames_seen;
	struct timeval ping_sent;
	int ping_pending;
	int32_t ping_ms;
	struct timeval next_ping;
} ra = { .listen_fd = -1, .fd = -1 };

static struct timeval ra_log_epoch;

static long ms_between(const struct timeval* a, const struct timeval* b) {
	return (b->tv_sec - a->tv_sec) * 1000L + (b->tv_usec - a->tv_usec) / 1000L;
}

static long ms_since(const struct timeval* t) {
	struct timeval now;
	gettimeofday(&now, NULL);
	return ms_between(t, &now);
}

static void ra_log(const char* fmt, ...) {
	if (!ra_log_epoch.tv_sec) gettimeofday(&ra_log_epoch, NULL);
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[raproto +%ldms] ", ms_since(&ra_log_epoch));
	vfprintf(stderr, fmt, args);
	va_end(args);
	fflush(stderr);
}

//////////////////////////////////////////////////////////////////////////////
// pure helpers
//////////////////////////////////////////////////////////////////////////////

/* netplay_impl_magic. PACKAGE_VERSION is RetroArch's version string. */
uint32_t RaProto_implMagic(const char* ver, uint32_t protocol) {
	if (!ver) ver = RAPROTO_DEFAULT_RA_VERSION;
	uint32_t res = 0;
	size_t i, len = strlen(ver);
	for (i = 0; i < len; i++) res ^= (uint32_t)(unsigned char)ver[i] << (i & 0xf);
	res ^= protocol << (i & 0xf);
	return res;
}

/* netplay_platform_magic: big-endian bit, sizeof(size_t) << 15, sizeof(long). */
uint32_t RaProto_platformMagic(void) {
	return ((uint32_t)(htonl(1) == 1) << 30) |
	       ((uint32_t)sizeof(size_t) << 15) |
	       (uint32_t)sizeof(long);
}

uint32_t RaProto_deviceWords(uint32_t device) {
	switch (device & RETRO_DEVICE_TYPE_MASK) {
	case DEV_JOYPAD:   return 1;
	case DEV_MOUSE:    return 2;
	case DEV_KEYBOARD: return 5;
	case DEV_LIGHTGUN: return 2;
	case DEV_ANALOG:   return 3;
	default:           return 0;
	}
}

#define ALIGN8(x) (((x) + 7) & ~(size_t)7)

static void put_block(uint8_t* out, const char* tag, uint32_t len) {
	memcpy(out, tag, 4);
	out[4] = (uint8_t)len; out[5] = (uint8_t)(len >> 8);
	out[6] = (uint8_t)(len >> 16); out[7] = (uint8_t)(len >> 24);
}

/* netplay_build_savestate, as a host with achievements compiled in but
 * disabled builds it: the ACHV block carries only its 8 flag bytes. That is
 * the layout spruce's RetroArch sends (5616 bytes for fceumm's 5572), and
 * netplay_process_savestate1 accepts it with or without HAVE_CHEEVOS. */
void* RaProto_buildContainer(const void* core_state, size_t len, size_t* out_len) {
	size_t total = 8 + 8 + ALIGN8(len) + 8 + 8 + 8;
	uint8_t* out = calloc(1, total);
	if (!out) return NULL;
	memcpy(out, "NETPLAY", 7);
	out[7] = 1;
	put_block(out + 8, "MEM ", (uint32_t)len);
	memcpy(out + 16, core_state, len);
	size_t off = 16 + ALIGN8(len);
	put_block(out + off, "ACHV", 8);
	off += 16; /* header + 8 zero flag bytes: not hardcore */
	put_block(out + off, "END ", 0);
	*out_len = total;
	return out;
}

/* netplay_get_savestate_coremem / netplay_process_savestate1. */
bool RaProto_unwrapContainer(const void* data, size_t len, const void** core, size_t* core_len) {
	const uint8_t* in = data;
	if (len < 8 || memcmp(in, "NETPLAY", 7) != 0) {
		*core = data;
		*core_len = len;
		return true;
	}
	if (in[7] != 1) return false;
	size_t off = 8;
	while (off + 8 <= len) {
		uint32_t block = (uint32_t)in[off + 4] | ((uint32_t)in[off + 5] << 8) |
		                 ((uint32_t)in[off + 6] << 16) | ((uint32_t)in[off + 7] << 24);
		const uint8_t* marker = in + off;
		off += 8;
		if (!memcmp(marker, "MEM ", 4)) {
			if (block > len - off) return false;
			*core = in + off;
			*core_len = block;
			return true;
		}
		if (!memcmp(marker, "END ", 4)) break;
		if (ALIGN8(block) > len - off) return false;
		off += ALIGN8(block);
	}
	return false;
}

//////////////////////////////////////////////////////////////////////////////
// events and input ring
//////////////////////////////////////////////////////////////////////////////

static void push_event(RaProtoEventType type, uint32_t frame, uint32_t a, uint32_t b,
                       void* data, size_t len, const char* text) {
	EventNode* n = calloc(1, sizeof(*n));
	if (!n) { free(data); return; }
	n->ev.type = type;
	n->ev.frame = frame;
	n->ev.a = a;
	n->ev.b = b;
	n->ev.data = data;
	n->ev.len = len;
	if (text) snprintf(n->ev.text, sizeof(n->ev.text), "%s", text);
	pthread_mutex_lock(&ra.lock);
	if (ra.tail) ra.tail->next = n; else ra.head = n;
	ra.tail = n;
	pthread_mutex_unlock(&ra.lock);
}

bool RaProto_pollEvent(RaProtoEvent* out) {
	pthread_mutex_lock(&ra.lock);
	EventNode* n = ra.head;
	if (n) {
		ra.head = n->next;
		if (!ra.head) ra.tail = NULL;
	}
	pthread_mutex_unlock(&ra.lock);
	if (!n) return false;
	*out = n->ev;
	free(n);
	return true;
}

void RaProto_freeEvent(RaProtoEvent* ev) {
	if (!ev) return;
	free(ev->data);
	ev->data = NULL;
	ev->len = 0;
}

/* Called with ra.lock held. RetroArch delivers each client's input strictly
 * in frame order (netplay_get_cmd NAKs anything else), so a frame below the
 * next expected one is a retransmission and one above it a protocol error -
 * which is logged and accepted rather than fatal, since dropping the session
 * helps nobody. */
static void store_remote_locked(uint32_t frame, uint32_t value) {
	if (!ra.remote_started) {
		ra.remote_next = frame;
		ra.remote_started = 1;
	}
	if (frame < ra.remote_next) return;
	if (frame > ra.remote_next)
		ra_log("peer input skipped frames %u..%u\n", ra.remote_next, frame - 1);
	unsigned slot = frame % INPUT_RING;
	ra.remote[slot].frame = frame;
	ra.remote[slot].value = value;
	ra.remote[slot].valid = 1;
	ra.remote_next = frame + 1;
}

bool RaProto_getRemoteInput(uint32_t frame, uint32_t* buttons) {
	bool ok = false;
	pthread_mutex_lock(&ra.lock);
	unsigned slot = frame % INPUT_RING;
	if (ra.remote[slot].valid && ra.remote[slot].frame == frame) {
		*buttons = ra.remote[slot].value;
		ok = true;
	}
	pthread_mutex_unlock(&ra.lock);
	return ok;
}

uint32_t RaProto_remoteHorizon(void) {
	pthread_mutex_lock(&ra.lock);
	uint32_t h = ra.remote_started ? ra.remote_next : 0;
	pthread_mutex_unlock(&ra.lock);
	return h;
}

void RaProto_expectRemoteFrom(uint32_t frame) {
	pthread_mutex_lock(&ra.lock);
	if (!ra.remote_started || frame > ra.remote_next) {
		ra.remote_next = frame;
		ra.remote_started = 1;
	}
	pthread_mutex_unlock(&ra.lock);
}

//////////////////////////////////////////////////////////////////////////////
// socket I/O
//////////////////////////////////////////////////////////////////////////////

static bool write_all(int fd, const void* buf, size_t len) {
	const uint8_t* p = buf;
	struct timeval start;
	gettimeofday(&start, NULL);
	while (len) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n > 0) { p += n; len -= (size_t)n; continue; }
		if (n < 0 && (errno == EINTR)) continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (ms_since(&start) > IO_TIMEOUT_MS) return false;
			struct pollfd pfd = { fd, POLLOUT, 0 };
			poll(&pfd, 1, 50);
			continue;
		}
		return false;
	}
	return true;
}

/* Reads exactly len bytes, giving up after IO_TIMEOUT_MS without progress or
 * when asked to stop. */
static bool read_all(int fd, void* buf, size_t len) {
	uint8_t* p = buf;
	struct timeval last;
	gettimeofday(&last, NULL);
	while (len) {
		if (ra.stop) return false;
		struct pollfd pfd = { fd, POLLIN, 0 };
		int r = poll(&pfd, 1, 100);
		if (r < 0 && errno == EINTR) continue;
		if (r < 0) return false;
		if (r == 0) {
			if (ms_since(&last) > IO_TIMEOUT_MS) return false;
			continue;
		}
		ssize_t n = recv(fd, p, len, 0);
		if (n == 0) return false;
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
			return false;
		}
		p += n;
		len -= (size_t)n;
		gettimeofday(&last, NULL);
	}
	return true;
}

static bool discard(int fd, size_t len) {
	uint8_t buf[1024];
	while (len) {
		size_t n = len > sizeof(buf) ? sizeof(buf) : len;
		if (!read_all(fd, buf, n)) return false;
		len -= n;
	}
	return true;
}

/* One command, header and payload, written atomically with respect to the
 * other thread. */
static bool send_cmd(uint32_t cmd, const void* payload, size_t len) {
	pthread_mutex_lock(&ra.send_lock);
	int fd = ra.fd;
	bool ok = fd >= 0;
	if (ok) {
		uint32_t hdr[2] = { htonl(cmd), htonl((uint32_t)len) };
		ok = write_all(fd, hdr, sizeof(hdr)) && (!len || write_all(fd, payload, len));
	}
	pthread_mutex_unlock(&ra.send_lock);
	return ok;
}

static bool send_raw(const void* data, size_t len) {
	pthread_mutex_lock(&ra.send_lock);
	bool ok = ra.fd >= 0 && write_all(ra.fd, data, len);
	pthread_mutex_unlock(&ra.send_lock);
	return ok;
}

static void set_socket_options(int fd) {
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void close_connection(const char* why) {
	pthread_mutex_lock(&ra.send_lock);
	int fd = ra.fd;
	ra.fd = -1;
	pthread_mutex_unlock(&ra.send_lock);
	if (fd >= 0) close(fd);
	/* The ring is kept: inputs that arrived are still the inputs for their
	 * frames, and a host playing on alone runs up to the horizon with them. */
	pthread_mutex_lock(&ra.lock);
	int was = ra.connected;
	uint32_t horizon = ra.remote_started ? ra.remote_next : 0;
	ra.connected = 0;
	ra.remote_started = 0;
	ra.ping_pending = 0;
	pthread_mutex_unlock(&ra.lock);
	ra_log("connection closed: %s\n", why);
	if (was || ra.cfg.role == RAPROTO_ROLE_CLIENT)
		push_event(RAPROTO_EV_DISCONNECT, horizon, 0, 0, NULL, 0, why);
}

//////////////////////////////////////////////////////////////////////////////
// handshake
//////////////////////////////////////////////////////////////////////////////

static uint32_t max_protocol(void) {
	uint32_t p = ra.cfg.max_protocol ? ra.cfg.max_protocol : RA_HIGH_PROTOCOL;
	return p > RA_HIGH_PROTOCOL ? RA_HIGH_PROTOCOL : p;
}

/* netplay_handshake_init_send. A client puts its highest protocol in the
 * salt word and its lowest in the protocol word; a server answers with the
 * salt (0: no password) and the protocol it selected. */
static bool send_header(uint32_t word3, uint32_t protocol) {
	uint32_t h[6] = {
		htonl(RA_MAGIC), htonl(RaProto_platformMagic()), htonl(RA_COMPRESSION_ZLIB),
		htonl(word3), htonl(protocol),
		htonl(RaProto_implMagic(ra.cfg.ra_version, RA_HIGH_PROTOCOL)),
	};
	return send_raw(h, sizeof(h));
}

static bool send_nick(void) {
	char nick[RAPROTO_NICK_LEN] = {0};
	snprintf(nick, sizeof(nick), "%s", ra.cfg.nick[0] ? ra.cfg.nick : "Netplay");
	return send_cmd(CMD_NICK, nick, sizeof(nick));
}

static bool send_info(void) {
	uint8_t p[INFO_PAYLOAD] = {0};
	uint32_t crc = htonl(ra.cfg.content_crc);
	memcpy(p, &crc, 4);
	snprintf((char*)p + 4, RAPROTO_NICK_LEN, "%s", ra.cfg.core_name);
	snprintf((char*)p + 4 + RAPROTO_NICK_LEN, RAPROTO_NICK_LEN, "%s", ra.cfg.core_version);
	return send_cmd(CMD_INFO, p, sizeof(p));
}

static bool read_cmd_header(int fd, uint32_t* cmd, uint32_t* size) {
	uint32_t h[2];
	if (!read_all(fd, h, sizeof(h))) return false;
	*cmd = ntohl(h[0]);
	*size = ntohl(h[1]);
	return true;
}

static bool expect_nick(int fd) {
	uint32_t cmd, size;
	char nick[RAPROTO_NICK_LEN];
	if (!read_cmd_header(fd, &cmd, &size)) return false;
	if (cmd != CMD_NICK || size != RAPROTO_NICK_LEN) {
		ra_log("expected NICK, got cmd 0x%x size %u\n", cmd, size);
		return false;
	}
	if (!read_all(fd, nick, sizeof(nick))) return false;
	nick[RAPROTO_NICK_LEN - 1] = '\0';
	pthread_mutex_lock(&ra.lock);
	snprintf(ra.peer_nick, sizeof(ra.peer_nick), "%s", nick);
	pthread_mutex_unlock(&ra.lock);
	return true;
}

/* netplay_handshake_pre_info. Core name must match, case-insensitively;
 * version and content CRC are warnings, as they are in RetroArch. */
static bool expect_info(int fd) {
	uint32_t cmd, size;
	if (!read_cmd_header(fd, &cmd, &size)) return false;
	if (cmd != CMD_INFO) {
		ra_log("expected INFO, got cmd 0x%x\n", cmd);
		return false;
	}
	if (size != INFO_PAYLOAD) {
		ra_log("peer sent INFO of %u bytes: it has no content loaded\n", size);
		return false;
	}
	uint8_t p[INFO_PAYLOAD];
	if (!read_all(fd, p, sizeof(p))) return false;
	uint32_t crc;
	memcpy(&crc, p, 4);
	crc = ntohl(crc);
	char name[RAPROTO_NICK_LEN], version[RAPROTO_NICK_LEN];
	memcpy(name, p + 4, sizeof(name));
	memcpy(version, p + 4 + RAPROTO_NICK_LEN, sizeof(version));
	name[sizeof(name) - 1] = version[sizeof(version) - 1] = '\0';
	ra_log("peer core '%s' version '%s' content crc %08x\n", name, version, crc);
	if (strcasecmp(name, ra.cfg.core_name)) {
		ra_log("core differs: peer '%s', ours '%s' - RetroArch refuses this\n",
		       name, ra.cfg.core_name);
		return false;
	}
	if (strcasecmp(version, ra.cfg.core_version))
		ra_log("WARNING: core version differs (peer '%s', ours '%s'); lockstep needs the same build\n",
		       version, ra.cfg.core_version);
	if (ra.cfg.content_crc && crc && crc != ra.cfg.content_crc)
		ra_log("WARNING: content CRC differs (peer %08x, ours %08x)\n", crc, ra.cfg.content_crc);
	return true;
}

static bool client_handshake(int fd) {
	if (!send_header(max_protocol(), RA_LOW_PROTOCOL)) return false;

	uint32_t h[6];
	if (!read_all(fd, h, 4)) return false;
	uint32_t magic = ntohl(h[0]);
	if (magic == RA_FULL_MAGIC) { ra_log("host is full\n"); return false; }
	if (magic == RA_BANNED_MAGIC) { ra_log("host banned us\n"); return false; }
	if (magic != RA_MAGIC) { ra_log("not a RetroArch host (magic %08x)\n", magic); return false; }
	if (!read_all(fd, h + 1, sizeof(h) - 4)) return false;
	uint32_t salt = ntohl(h[3]);
	uint32_t protocol = ntohl(h[4]);
	if (protocol < RA_LOW_PROTOCOL || protocol > max_protocol()) {
		ra_log("host chose protocol %u, outside %u..%u\n", protocol, RA_LOW_PROTOCOL, max_protocol());
		return false;
	}
	if ((ntohl(h[1]) & (1u << 30)) != (RaProto_platformMagic() & (1u << 30)))
		ra_log("WARNING: host has the other endianness\n");
	if (ntohl(h[5]) != RaProto_implMagic(ra.cfg.ra_version, RA_HIGH_PROTOCOL))
		ra_log("host implementation magic %08x is not RetroArch %s's\n", ntohl(h[5]),
		       ra.cfg.ra_version ? ra.cfg.ra_version : RAPROTO_DEFAULT_RA_VERSION);
	if (salt) {
		ra_log("host requires a password, which is not supported yet\n");
		return false;
	}
	pthread_mutex_lock(&ra.lock);
	ra.protocol = protocol;
	ra.compression = ntohl(h[2]) & RA_COMPRESSION_ZLIB;
	pthread_mutex_unlock(&ra.lock);
	ra_log("host header: protocol %u, compression %s\n", protocol, ra.compression ? "zlib" : "none");

	if (!send_nick()) return false;
	if (!expect_nick(fd)) return false;
	if (!expect_info(fd)) return false;
	if (!send_info()) return false;

	/* netplay_handshake_pre_sync */
	uint32_t cmd, size;
	if (!read_cmd_header(fd, &cmd, &size)) return false;
	if (cmd != CMD_SYNC || size < SYNC_FIXED) {
		ra_log("expected SYNC, got cmd 0x%x size %u\n", cmd, size);
		return false;
	}
	uint8_t* p = malloc(size);
	if (!p) return false;
	if (!read_all(fd, p, size)) { free(p); return false; }
	uint32_t w;
	memcpy(&w, p, 4); uint32_t frame = ntohl(w);
	memcpy(&w, p + 4, 4); uint32_t client = ntohl(w);
	bool paused = client & (1u << 31);
	client &= ~(1u << 31);
	uint32_t devices[RAPROTO_MAX_DEVICES], device_clients[RAPROTO_MAX_DEVICES];
	for (unsigned i = 0; i < RAPROTO_MAX_DEVICES; i++) {
		memcpy(&w, p + 8 + 4 * i, 4);
		devices[i] = ntohl(w);
		memcpy(&w, p + 8 + 64 + 16 + 4 * i, 4);
		device_clients[i] = ntohl(w);
	}
	char nick[RAPROTO_NICK_LEN];
	memcpy(nick, p + SYNC_FIXED - RAPROTO_NICK_LEN, sizeof(nick));
	nick[sizeof(nick) - 1] = '\0';
	size_t sram_len = size - SYNC_FIXED;
	void* sram = NULL;
	if (sram_len) {
		sram = malloc(sram_len);
		if (sram) memcpy(sram, p + SYNC_FIXED, sram_len);
	}
	free(p);

	uint32_t host_devices = 0;
	for (unsigned i = 0; i < RAPROTO_MAX_DEVICES; i++)
		if (device_clients[i] & 1u) host_devices |= 1u << i;
	pthread_mutex_lock(&ra.lock);
	memcpy(ra.config_devices, devices, sizeof(devices));
	ra.self_client = client;
	ra.remote_client = 0;
	ra.client_devices_remote = host_devices;
	pthread_mutex_unlock(&ra.lock);
	ra_log("SYNC: frame %u, we are client %u as '%s'%s, host devices %#x, sram %zu bytes\n",
	       frame, client, nick, paused ? ", host paused" : "", host_devices, sram_len);
	RaProto_expectRemoteFrom(frame);
	push_event(RAPROTO_EV_SYNC, frame, client, host_devices, sram, sram_len, nick);
	if (paused) push_event(RAPROTO_EV_PAUSE, 0, 0, 0, NULL, 0, ra.peer_nick);

	/* netplay_cmd_mode(PLAYING): no share preference, no particular device. */
	uint32_t play = htonl(1u << 16);
	if (!send_cmd(CMD_PLAY, &play, sizeof(play))) return false;
	return true;
}

static bool host_handshake(int fd) {
	uint32_t h[6];
	if (!read_all(fd, h, 4)) return false;
	uint32_t magic = ntohl(h[0]);
	if (magic == RA_POKE_MAGIC) {
		/* send_info_and_disconnect: a probe, not a player */
		send_header(0, max_protocol());
		ra_log("answered a POKE probe\n");
		return false;
	}
	if (magic != RA_MAGIC) { ra_log("not a RetroArch client (magic %08x)\n", magic); return false; }
	if (!read_all(fd, h + 1, sizeof(h) - 4)) return false;

	/* select_protocol: a non-zero salt word from a client is its highest. */
	uint32_t lo = ntohl(h[4]), hi = ntohl(h[3]);
	uint32_t protocol = !hi ? lo : (hi > max_protocol() ? max_protocol() : hi);
	if (protocol < RA_LOW_PROTOCOL || protocol > max_protocol()) {
		send_header(0, 0);
		ra_log("client offers protocol %u..%u, outside ours\n", lo, hi);
		return false;
	}
	if (!send_header(0, protocol)) return false;
	pthread_mutex_lock(&ra.lock);
	ra.protocol = protocol;
	ra.compression = ntohl(h[2]) & RA_COMPRESSION_ZLIB;
	pthread_mutex_unlock(&ra.lock);
	ra_log("client header: protocol %u, compression %s\n", protocol, ra.compression ? "zlib" : "none");

	if (!expect_nick(fd)) return false;
	if (!send_nick()) return false;
	if (!send_info()) return false;
	if (!expect_info(fd)) return false;

	/* select_nickname: the client may not share the host's nick. */
	pthread_mutex_lock(&ra.lock);
	char ours[RAPROTO_NICK_LEN];
	snprintf(ours, sizeof(ours), "%s", ra.cfg.nick[0] ? ra.cfg.nick : "Netplay");
	if (!strcmp(ra.peer_nick, ours)) {
		snprintf(ra.client_nick, sizeof(ra.client_nick), "%.27s (2)", ra.peer_nick);
	} else {
		snprintf(ra.client_nick, sizeof(ra.client_nick), "%s", ra.peer_nick);
	}
	ra.self_client = 0;
	ra.remote_client = 1;
	for (unsigned i = 0; i < RAPROTO_MAX_DEVICES; i++) ra.config_devices[i] = DEV_JOYPAD;
	ra.client_devices_remote = 0;
	pthread_mutex_unlock(&ra.lock);
	return true;
}

//////////////////////////////////////////////////////////////////////////////
// commands after the handshake
//////////////////////////////////////////////////////////////////////////////

static uint32_t words_for_devices_locked(uint32_t devices) {
	uint32_t n = 0;
	for (unsigned i = 0; i < RAPROTO_MAX_DEVICES; i++)
		if (devices & (1u << i)) n += RaProto_deviceWords(ra.config_devices[i]);
	return n;
}

static void* decode_state(const uint8_t* body, uint32_t size, uint32_t raw_len, size_t* out_len) {
	if (raw_len > RAPROTO_MAX_STATE) return NULL;
	uint8_t* raw = malloc(raw_len ? raw_len : 1);
	if (!raw) return NULL;
	if (ra.compression & RA_COMPRESSION_ZLIB) {
		uLongf got = raw_len;
		int z = uncompress(raw, &got, body, size);
		if (z != Z_OK || got != raw_len) {
			ra_log("savestate does not inflate (zlib %d, %lu of %u bytes)\n", z,
			       (unsigned long)got, raw_len);
			free(raw);
			return NULL;
		}
	} else {
		if (size != raw_len) { free(raw); return NULL; }
		memcpy(raw, body, size);
	}
	const void* core;
	size_t core_len;
	if (!RaProto_unwrapContainer(raw, raw_len, &core, &core_len)) {
		ra_log("savestate container has no MEM block\n");
		free(raw);
		return NULL;
	}
	void* out = malloc(core_len ? core_len : 1);
	if (out) memcpy(out, core, core_len);
	free(raw);
	*out_len = core_len;
	return out;
}

/* One command off the wire. False ends the connection. */
static bool handle_command(int fd) {
	uint32_t cmd, size;
	if (!read_cmd_header(fd, &cmd, &size)) return false;
	bool host = ra.cfg.role == RAPROTO_ROLE_HOST;
	uint32_t w[4];

	switch (cmd) {
	case CMD_ACK:
		return discard(fd, size);
	case CMD_NAK:
		ra_log("peer sent NAK: it rejected something we sent\n");
		return false;
	case CMD_DISCONNECT:
		ra_log("peer disconnected\n");
		return false;

	case CMD_INPUT: {
		if (size < 8 || size > 4 * 18) return false;
		uint32_t buf[18];
		if (!read_all(fd, buf, size)) return false;
		uint32_t frame = ntohl(buf[0]);
		uint32_t client = ntohl(buf[1]) & 0xFFFF;
		uint32_t words = size / 4 - 2;
		/* A host ignores the claimed client number: it is this connection. */
		if (!host && client != ra.remote_client) return true; /* another player's */
		uint32_t buttons = words ? ntohl(buf[2]) & 0xFFFF : 0;
		pthread_mutex_lock(&ra.lock);
		store_remote_locked(frame, buttons);
		pthread_mutex_unlock(&ra.lock);
		return true;
	}
	case CMD_NOINPUT: {
		if (host || size != 4) return false;
		if (!read_all(fd, w, 4)) return false;
		pthread_mutex_lock(&ra.lock);
		store_remote_locked(ntohl(w[0]), 0);
		pthread_mutex_unlock(&ra.lock);
		return true;
	}

	case CMD_PLAY: {
		if (!host || size != 4) return false;
		if (!read_all(fd, w, 4)) return false;
		push_event(RAPROTO_EV_PLAY, 0, ntohl(w[0]), 0, NULL, 0, ra.client_nick);
		return true;
	}
	case CMD_SPECTATE:
		if (!host || size) return false;
		push_event(RAPROTO_EV_SPECTATE, 0, 0, 0, NULL, 0, ra.client_nick);
		return true;

	case CMD_MODE: {
		if (host || size != MODE_PAYLOAD) return false;
		uint8_t p[MODE_PAYLOAD];
		if (!read_all(fd, p, sizeof(p))) return false;
		uint32_t frame, mode, devices;
		memcpy(&frame, p, 4); memcpy(&mode, p + 4, 4); memcpy(&devices, p + 8, 4);
		frame = ntohl(frame); mode = ntohl(mode); devices = ntohl(devices);
		char nick[RAPROTO_NICK_LEN];
		memcpy(nick, p + 12 + RAPROTO_MAX_DEVICES, sizeof(nick));
		nick[sizeof(nick) - 1] = '\0';
		uint32_t client = mode & 0xFFFF;
		if (client == ra.remote_client && !(mode & RAPROTO_MODE_YOU)) {
			pthread_mutex_lock(&ra.lock);
			ra.client_devices_remote = (mode & RAPROTO_MODE_PLAYING) ? devices : 0;
			pthread_mutex_unlock(&ra.lock);
		}
		push_event(RAPROTO_EV_MODE, frame, mode, devices, NULL, 0, nick);
		return true;
	}
	case CMD_MODE_REFUSED:
		if (host || size != 4) return false;
		if (!read_all(fd, w, 4)) return false;
		push_event(RAPROTO_EV_MODE_REFUSED, 0, ntohl(w[0]), 0, NULL, 0, NULL);
		return true;

	case CMD_CRC:
		if (size != 8) return false;
		if (!read_all(fd, w, 8)) return false;
		push_event(RAPROTO_EV_CRC, ntohl(w[0]), ntohl(w[1]), 0, NULL, 0, NULL);
		return true;
	case CMD_REQUEST_SAVESTATE:
		if (!discard(fd, size)) return false;
		if (host) push_event(RAPROTO_EV_REQUEST_STATE, 0, 0, 0, NULL, 0, NULL);
		return true;
	case CMD_LOAD_SAVESTATE: {
		if (host || size < 8 || size > RAPROTO_MAX_STATE) return false;
		uint8_t* body = malloc(size);
		if (!body) return false;
		if (!read_all(fd, body, size)) { free(body); return false; }
		uint32_t frame, raw_len;
		memcpy(&frame, body, 4); memcpy(&raw_len, body + 4, 4);
		frame = ntohl(frame); raw_len = ntohl(raw_len);
		size_t len = 0;
		void* state = decode_state(body + 8, size - 8, raw_len, &len);
		free(body);
		if (!state) return false;
		push_event(RAPROTO_EV_STATE, frame, 0, 0, state, len, NULL);
		return true;
	}
	case CMD_RESET:
		if (host || size != 4) return false;
		if (!read_all(fd, w, 4)) return false;
		push_event(RAPROTO_EV_RESET, ntohl(w[0]), 0, 0, NULL, 0, NULL);
		return true;

	case CMD_PAUSE: {
		if (size != RAPROTO_NICK_LEN) return false;
		char nick[RAPROTO_NICK_LEN];
		if (!read_all(fd, nick, sizeof(nick))) return false;
		nick[sizeof(nick) - 1] = '\0';
		push_event(RAPROTO_EV_PAUSE, 0, 0, 0, NULL, 0, nick);
		return true;
	}
	case CMD_RESUME:
		if (!discard(fd, size)) return false;
		push_event(RAPROTO_EV_RESUME, 0, 0, 0, NULL, 0, NULL);
		return true;
	case CMD_STALL:
		if (host || size != 4) return false;
		if (!read_all(fd, w, 4)) return false;
		push_event(RAPROTO_EV_STALL, 0, ntohl(w[0]), 0, NULL, 0, NULL);
		return true;

	case CMD_PING_REQUEST:
		if (!discard(fd, size)) return false;
		send_cmd(CMD_PING_RESPONSE, NULL, 0);
		/* A server requests its own ping after answering (netplay_get_cmd). */
		if (host && !ra.ping_pending && send_cmd(CMD_PING_REQUEST, NULL, 0)) {
			pthread_mutex_lock(&ra.lock);
			gettimeofday(&ra.ping_sent, NULL);
			ra.ping_pending = 1;
			pthread_mutex_unlock(&ra.lock);
		}
		return true;
	case CMD_PING_RESPONSE:
		if (!discard(fd, size)) return false;
		pthread_mutex_lock(&ra.lock);
		if (ra.ping_pending) {
			ra.ping_ms = (int32_t)ms_since(&ra.ping_sent);
			ra.ping_pending = 0;
		}
		pthread_mutex_unlock(&ra.lock);
		return true;

	case CMD_SETTING_ALLOW_PAUSING:
		if (host || size != 4) return false;
		if (!read_all(fd, w, 4)) return false;
		pthread_mutex_lock(&ra.lock);
		ra.host_allows_pausing = ntohl(w[0]) != 0;
		pthread_mutex_unlock(&ra.lock);
		ra_log("host %s pausing\n", ntohl(w[0]) ? "allows" : "does not allow");
		return true;
	case CMD_SETTING_INPUT_LATENCY_FRAMES:
		if (host || size != 8) return false;
		if (!read_all(fd, w, 8)) return false;
		ra_log("host input latency frames %d..%d\n", (int32_t)ntohl(w[0]), (int32_t)ntohl(w[1]));
		return true;

	case CMD_PLAYER_CHAT:
		/* Shown nowhere yet; RetroArch's chat is optional. */
		return discard(fd, size);
	case CMD_NETPACKET:
		ra_log("NETPACKET (link cores) is not supported yet\n");
		return discard(fd, size);
	default:
		ra_log("unknown command 0x%x (%u bytes), skipped\n", cmd, size);
		return discard(fd, size);
	}
}

//////////////////////////////////////////////////////////////////////////////
// worker
//////////////////////////////////////////////////////////////////////////////

static int open_listener(uint16_t port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(port);
	if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0 || listen(fd, 2) < 0) {
		ra_log("cannot listen on %u: %s\n", port, strerror(errno));
		close(fd);
		return -1;
	}
	ra_log("listening on %u\n", port);
	return fd;
}

static int dial(const char* host, uint16_t port) {
	char service[8];
	snprintf(service, sizeof(service), "%u", port);
	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, service, &hints, &res) != 0 || !res) return -1;
	int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd < 0) { freeaddrinfo(res); return -1; }
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	int r = connect(fd, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);
	if (r < 0 && errno != EINPROGRESS) { close(fd); return -1; }
	if (r < 0) {
		struct pollfd pfd = { fd, POLLOUT, 0 };
		if (poll(&pfd, 1, 2000) != 1) { close(fd); return -1; }
		int err = 0;
		socklen_t len = sizeof(err);
		getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
		if (err) { close(fd); return -1; }
	}
	return fd;
}

/* Pings, between commands.
 *
 * No PAUSE is ever sent. RetroArch 1.22.2 cannot be resumed once paused by a
 * peer: remote_unpaused tests
 *     sc->flags & ((ACTIVE | PAUSED) == (ACTIVE | PAUSED))
 * and the comparison binds first, so the test is "flags & 1" - true for any
 * active connection - and remote_paused is set straight back. A PAUSE from us
 * therefore freezes the RetroArch side for good; that was the 2026-11-04
 * deadlock with a spruce client, set off by the 260ms the host's own initial
 * serialize took. A frontend stall (menu, sleep) is simply silence: RetroArch
 * predicts for up to 60 frames, then stalls, and hangs up after 5 s (as a
 * host) or 10 s (as a client). See retroarch-protocol.md, Pause and menus. */
static void worker_housekeeping(void) {
	pthread_mutex_lock(&ra.lock);
	int connected = ra.connected;
	pthread_mutex_unlock(&ra.lock);
	if (!connected) return;

	/* netplay_request_ping: protocol 6+, every few seconds, from the client. */
	struct timeval now;
	gettimeofday(&now, NULL);
	if (ra.cfg.role == RAPROTO_ROLE_CLIENT && ra.protocol >= 6 &&
	    ms_between(&ra.next_ping, &now) >= 0 && !ra.ping_pending) {
		if (send_cmd(CMD_PING_REQUEST, NULL, 0)) {
			pthread_mutex_lock(&ra.lock);
			ra.ping_sent = now;
			ra.ping_pending = 1;
			pthread_mutex_unlock(&ra.lock);
		}
		ra.next_ping = now;
		ra.next_ping.tv_sec += 3;
	}
}

static void serve_connection(int fd) {
	while (!ra.stop) {
		worker_housekeeping();
		struct pollfd pfd = { fd, POLLIN, 0 };
		int r = poll(&pfd, 1, 50);
		if (r < 0 && errno == EINTR) continue;
		if (r < 0) { close_connection("poll failed"); return; }
		if (r == 0) continue;
		if (pfd.revents & (POLLERR | POLLNVAL)) { close_connection("socket error"); return; }
		if (!handle_command(fd)) { close_connection("peer closed the connection or broke protocol"); return; }
	}
}

static void* worker_main(void* unused) {
	(void)unused;
	struct timeval began;
	gettimeofday(&began, NULL);
	uint16_t port = ra.cfg.port ? ra.cfg.port : RAPROTO_DEFAULT_PORT;

	if (ra.cfg.role == RAPROTO_ROLE_HOST) {
		ra.listen_fd = open_listener(port);
		if (ra.listen_fd < 0) {
			push_event(RAPROTO_EV_DISCONNECT, 0, 0, 0, NULL, 0, "Cannot listen for RetroArch players.");
			return NULL;
		}
	}

	while (!ra.stop) {
		int fd = -1;
		if (ra.cfg.role == RAPROTO_ROLE_HOST) {
			struct pollfd pfd = { ra.listen_fd, POLLIN, 0 };
			if (poll(&pfd, 1, 100) != 1) continue;
			fd = accept(ra.listen_fd, NULL, NULL);
			if (fd < 0) continue;
			set_socket_options(fd);
			pthread_mutex_lock(&ra.send_lock);
			ra.fd = fd;
			pthread_mutex_unlock(&ra.send_lock);
			ra_log("client connecting\n");
			if (!host_handshake(fd)) {
				close_connection("client handshake failed");
				continue;
			}
			pthread_mutex_lock(&ra.lock);
			ra.connected = 1;
			pthread_mutex_unlock(&ra.lock);
			ra_log("client '%s' ready (protocol %u)\n", ra.peer_nick, ra.protocol);
			push_event(RAPROTO_EV_CLIENT_READY, 0, 0, 0, NULL, 0, ra.client_nick);
		} else {
			fd = dial(ra.cfg.peer, port);
			if (fd < 0) {
				if (ms_since(&began) > CONNECT_GIVE_UP_MS) {
					push_event(RAPROTO_EV_DISCONNECT, 0, 0, 0, NULL, 0, "RetroArch host not found.");
					return NULL;
				}
				usleep(CONNECT_RETRY_MS * 1000);
				continue;
			}
			set_socket_options(fd);
			pthread_mutex_lock(&ra.send_lock);
			ra.fd = fd;
			pthread_mutex_unlock(&ra.send_lock);
			ra_log("connected to %s:%u\n", ra.cfg.peer, port);
			gettimeofday(&ra.next_ping, NULL);
			ra.next_ping.tv_sec += 3;
			if (!client_handshake(fd)) {
				close_connection("Handshake with the RetroArch host failed.");
				return NULL;
			}
			pthread_mutex_lock(&ra.lock);
			ra.connected = 1;
			pthread_mutex_unlock(&ra.lock);
		}

		serve_connection(fd);
		/* A client's session ends with its host; a host keeps listening. */
		if (ra.cfg.role == RAPROTO_ROLE_CLIENT) break;

		/* Host: anyone else knocking while we played is turned away below. */
	}
	return NULL;
}

/* A second client while one is playing: RetroArch's netplay_full answer. The
 * check runs on the listener in the worker loop above only between
 * connections, so a knock during play waits in the backlog until then; it is
 * answered here instead, from the emulator thread's frame tick. */
static void turn_away_extra_clients(void) {
	if (ra.cfg.role != RAPROTO_ROLE_HOST || ra.listen_fd < 0 || !ra.connected) return;
	struct pollfd pfd = { ra.listen_fd, POLLIN, 0 };
	if (poll(&pfd, 1, 0) != 1) return;
	int fd = accept(ra.listen_fd, NULL, NULL);
	if (fd < 0) return;
	uint32_t full = htonl(RA_FULL_MAGIC);
	(void)!write(fd, &full, sizeof(full));
	close(fd);
	ra_log("turned away a second client: two players only\n");
}

//////////////////////////////////////////////////////////////////////////////
// public API
//////////////////////////////////////////////////////////////////////////////

bool RaProto_start(const RaProtoConfig* config) {
	if (ra.thread_started) return true;
	pthread_mutex_init(&ra.lock, NULL);
	pthread_mutex_init(&ra.send_lock, NULL);
	ra.cfg = *config;
	ra.fd = -1;
	ra.listen_fd = -1;
	ra.stop = 0;
	ra.ping_ms = -1;
	ra.self_client = config->role == RAPROTO_ROLE_HOST ? 0 : 1;
	ra.remote_client = config->role == RAPROTO_ROLE_HOST ? 1 : 0;
	for (unsigned i = 0; i < RAPROTO_MAX_DEVICES; i++) ra.config_devices[i] = DEV_JOYPAD;
	ra_log("starting as %s%s%s, core '%s' '%s', content crc %08x\n",
	       config->role == RAPROTO_ROLE_HOST ? "host" : "client",
	       config->role == RAPROTO_ROLE_CLIENT ? " of " : "",
	       config->role == RAPROTO_ROLE_CLIENT ? config->peer : "",
	       config->core_name, config->core_version, config->content_crc);
	if (pthread_create(&ra.thread, NULL, worker_main, NULL) != 0) return false;
	ra.thread_started = 1;
	return true;
}

void RaProto_stop(void) {
	if (!ra.thread_started) return;
	if (ra.connected) send_cmd(CMD_DISCONNECT, NULL, 0);
	ra.stop = 1;
	pthread_join(ra.thread, NULL);
	ra.thread_started = 0;
	pthread_mutex_lock(&ra.send_lock);
	if (ra.fd >= 0) close(ra.fd);
	ra.fd = -1;
	pthread_mutex_unlock(&ra.send_lock);
	if (ra.listen_fd >= 0) close(ra.listen_fd);
	ra.listen_fd = -1;
	ra.connected = 0;
	RaProtoEvent ev;
	while (RaProto_pollEvent(&ev)) RaProto_freeEvent(&ev);
}

bool RaProto_isConnected(void) { return ra.connected; }
uint32_t RaProto_protocol(void) { return ra.protocol; }
uint32_t RaProto_selfClient(void) { return ra.self_client; }
bool RaProto_hostAllowsPausing(void) { return ra.host_allows_pausing; }
const char* RaProto_peerNick(void) { return ra.peer_nick; }
int32_t RaProto_pingMs(void) { return ra.ping_ms; }

uint32_t RaProto_configDevice(unsigned port) {
	return port < RAPROTO_MAX_DEVICES ? ra.config_devices[port] : 0;
}

void RaProto_markFrame(void) {
	pthread_mutex_lock(&ra.lock);
	gettimeofday(&ra.last_frame, NULL);
	ra.frames_seen = 1;
	pthread_mutex_unlock(&ra.lock);
	turn_away_extra_clients();
}

bool RaProto_sendInput(uint32_t frame, uint32_t devices, uint32_t buttons) {
	uint32_t buf[2 + 16];
	pthread_mutex_lock(&ra.lock);
	uint32_t words = words_for_devices_locked(devices);
	uint32_t client = ra.self_client;
	pthread_mutex_unlock(&ra.lock);
	if (words > 16) words = 16;
	memset(buf, 0, sizeof(buf));
	buf[0] = htonl(frame);
	buf[1] = htonl(client);
	/* Joypad bits 0..15 (RETRO_DEVICE_ID_JOYPAD_B..R3) in the first word of
	 * the first device: get_self_input_state. */
	if (words) buf[2] = htonl(buttons & 0xFFFF);
	return send_cmd(CMD_INPUT, buf, (2 + words) * 4);
}

bool RaProto_sendSync(uint32_t frame, const uint32_t device_clients[RAPROTO_MAX_DEVICES],
                      const void* sram, size_t sram_len) {
	size_t len = SYNC_FIXED + sram_len;
	uint8_t* p = calloc(1, len);
	if (!p) return false;
	uint32_t w = htonl(frame);
	memcpy(p, &w, 4);
	w = htonl(1); /* the client's number; one client */
	memcpy(p + 4, &w, 4);
	pthread_mutex_lock(&ra.lock);
	for (unsigned i = 0; i < RAPROTO_MAX_DEVICES; i++) {
		w = htonl(ra.config_devices[i]);
		memcpy(p + 8 + 4 * i, &w, 4);
		w = htonl(device_clients[i]);
		memcpy(p + 8 + 64 + 16 + 4 * i, &w, 4);
	}
	/* share modes stay zero: no device is shared */
	memcpy(p + SYNC_FIXED - RAPROTO_NICK_LEN, ra.client_nick, RAPROTO_NICK_LEN);
	uint32_t protocol = ra.protocol;
	bool allow = ra.cfg.allow_pausing;
	int32_t lmin = ra.cfg.latency_min, lmax = ra.cfg.latency_max;
	pthread_mutex_unlock(&ra.lock);
	if (sram_len) memcpy(p + SYNC_FIXED, sram, sram_len);
	bool ok = send_cmd(CMD_SYNC, p, len);
	free(p);
	if (ok && protocol >= 6) {
		uint32_t a = htonl(allow ? 1 : 0);
		int32_t lat[2] = { (int32_t)htonl((uint32_t)lmin), (int32_t)htonl((uint32_t)lmax) };
		ok = send_cmd(CMD_SETTING_ALLOW_PAUSING, &a, 4) &&
		     send_cmd(CMD_SETTING_INPUT_LATENCY_FRAMES, lat, 8);
	}
	return ok;
}

bool RaProto_sendMode(uint32_t frame, uint32_t mode_word, uint32_t devices, const char* nick) {
	uint8_t p[MODE_PAYLOAD] = {0};
	uint32_t w = htonl(frame);
	memcpy(p, &w, 4);
	w = htonl(mode_word);
	memcpy(p + 4, &w, 4);
	w = htonl(devices);
	memcpy(p + 8, &w, 4);
	snprintf((char*)p + 12 + RAPROTO_MAX_DEVICES, RAPROTO_NICK_LEN, "%s", nick ? nick : "");
	if ((mode_word & 0xFFFF) == ra.remote_client) {
		pthread_mutex_lock(&ra.lock);
		ra.client_devices_remote = (mode_word & RAPROTO_MODE_PLAYING) ? devices : 0;
		pthread_mutex_unlock(&ra.lock);
	}
	return send_cmd(CMD_MODE, p, sizeof(p));
}

bool RaProto_sendModeRefused(uint32_t reason) {
	uint32_t w = htonl(reason);
	return send_cmd(CMD_MODE_REFUSED, &w, 4);
}

bool RaProto_sendState(uint32_t frame, const void* core_state, size_t len) {
	pthread_mutex_lock(&ra.lock);
	uint32_t protocol = ra.protocol;
	uint32_t compression = ra.compression;
	pthread_mutex_unlock(&ra.lock);

	/* netplay_send_savestate: protocol 7 peers get the container, older ones
	 * the bare core state. */
	size_t raw_len = len;
	void* container = NULL;
	const void* raw = core_state;
	if (protocol >= 7) {
		container = RaProto_buildContainer(core_state, len, &raw_len);
		if (!container) return false;
		raw = container;
	}
	uLongf zlen = compressBound(raw_len);
	uint8_t* p = malloc(8 + (compression ? zlen : raw_len));
	if (!p) { free(container); return false; }
	size_t body;
	if (compression) {
		if (compress2(p + 8, &zlen, raw, raw_len, Z_DEFAULT_COMPRESSION) != Z_OK) {
			free(p); free(container);
			return false;
		}
		body = zlen;
	} else {
		memcpy(p + 8, raw, raw_len);
		body = raw_len;
	}
	uint32_t w = htonl(frame);
	memcpy(p, &w, 4);
	w = htonl((uint32_t)raw_len);
	memcpy(p + 4, &w, 4);
	bool ok = send_cmd(CMD_LOAD_SAVESTATE, p, 8 + body);
	ra_log("sent savestate for frame %u: %zu bytes, %zu on the wire\n", frame, raw_len, body);
	free(p);
	free(container);
	return ok;
}

bool RaProto_sendCrc(uint32_t frame, uint32_t crc) {
	uint32_t w[2] = { htonl(frame), htonl(crc) };
	return send_cmd(CMD_CRC, w, 8);
}

bool RaProto_sendReset(uint32_t frame) {
	uint32_t w = htonl(frame);
	return send_cmd(CMD_RESET, &w, 4);
}

bool RaProto_sendStall(uint32_t frames) {
	uint32_t w = htonl(frames);
	return send_cmd(CMD_STALL, &w, 4);
}

bool RaProto_requestState(void) {
	return send_cmd(CMD_REQUEST_SAVESTATE, NULL, 0);
}
