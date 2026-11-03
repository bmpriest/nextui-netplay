#include "drastic_wifi_transport.h"
#include "netlink.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* A host must not run dozens of command generations ahead of the client and
 * rely on the bounded radio inbox to discard them.  Give the peer one local
 * display/event scheduling interval to consume the command and answer.  This
 * remains bounded so an absent station causes a stutter, not a deadlock. */
/* This barrier does two jobs, which is why it cannot simply be shortened. It
 * paces the host so it does not run generations ahead of the client — the
 * bounded inbox keeps only the newest frame per role, so an unpaced host
 * overwrites its own commands before they are consumed — and it is also the
 * single largest cost in the host's wall clock. Measured: at 100 attempts the
 * host runs at 0.097x real time; at 1 attempt it reaches 0.646x but the client
 * receives 8 commands instead of 512 and never joins. Six is the tested
 * compromise. The real fix is a command queue rather than newest-wins, or a
 * coarser synchronisation boundary. */
#define DWIFI_REPLY_WAIT_ATTEMPTS 50
#define DWIFI_LINK_WAIT_ATTEMPTS  6
#define DWIFI_REPLY_WAIT_STEP_US  500
/* Bounds on the symmetric clock hold. */
#define DWIFI_CLOCK_HOLD_ATTEMPTS 50
#define DWIFI_PEER_CLOCK_FRESH_MS 250

/* This document's log re-tests the barrier length repeatedly, and each pass has
 * cost a cross-build and a redeploy — which also makes an A/B non-attributable
 * unless both libraries are checksummed. Read it from the environment instead,
 * defaulting to the compiled-in value, so the two arms differ by a launch
 * variable and nothing else. */
static unsigned reply_wait_attempts(void)
{
	static unsigned cached;
	if (!cached) {
		const char *setting = getenv("DRASTIC_WIFI_BARRIER_ATTEMPTS");
		unsigned long parsed = setting ? strtoul(setting, NULL, 10) : 0;
		cached = (parsed >= 1 && parsed <= 10000) ?
			(unsigned)parsed : DWIFI_REPLY_WAIT_ATTEMPTS;
		fprintf(stderr, "[drastic-wifi] barrier attempts=%u (%s)\n",
			cached, setting ? "environment" : "default");
	}
	return cached;
}

static int transport_active;
static unsigned tx_count;
static unsigned rx_count;
/* DraStic's event callback runs on a small native stack. The transport is
 * single-threaded at that boundary, so packet-sized scratch storage belongs in
 * static state rather than adding 8 KiB to every send and poll call frame. */
#define DWIFI_MP_HEADER_SIZE 10
#define DWIFI_MP_TYPE_OTHER  0
#define DWIFI_MP_TYPE_REPLY  1
#define DWIFI_MP_TYPE_CMD    2
#define DWIFI_MP_TYPE_ACK    3

static uint8_t outgoing_frame[NETLINK_MAX_PACKET];
/* Packet-sized scratch is static, not automatic: DraStic's event callback runs
 * on a small native stack, as the note at the top of this file records. */
static uint8_t framed[DWIFI_MP_HEADER_SIZE + NETLINK_MAX_PACKET];
static uint8_t datagram_buffer[DWIFI_MP_HEADER_SIZE + NETLINK_MAX_PACKET];
static uint8_t incoming_frame[NETLINK_MAX_PACKET];
static uint8_t pending_beacon[NETLINK_MAX_PACKET];
static size_t pending_beacon_length;
/* Multiplayer command generations are queued rather than kept newest-wins. The
 * single-slot version coupled pacing to the host's wall clock: with the reply
 * barrier shortened the host ran ahead and overwrote its own commands, and the
 * client received 8 of 512. A queue lets the host run several generations ahead
 * without discarding any, which is what separates flow control from the stall.
 *
 * A generation is only eligible for delivery once its closing ACK has arrived,
 * preserving the invariant the two-deep version enforced: never splice an ACK
 * onto a different host cycle. */
/* Depth of the command-generation ring. The array is sized at the maximum and
 * the working depth is read from the environment, so depth can be A/B tested
 * without a rebuild — the same reason the reply barrier is settable. Earlier
 * passes at this question rebuilt between arms, which cannot rule out an
 * unrelated change riding along. */
#define DWIFI_MP_GENERATIONS     8
#define DWIFI_MP_GENERATIONS_MAX 256

/* Whether the beacon obligation is serviced while the reply barrier stalls this
 * console. Off by default so the compiled-in behaviour is unchanged and the A/B
 * runs on one library. */
/* Commands this console has put on the air and replies it has had back. The
 * difference is how many generations the peer is behind. */
static uint64_t mp_cmds_out;
static uint64_t mp_replies_in;
static uint64_t barrier_window_credits;

static uint64_t mp_outstanding(void)
{
	return mp_cmds_out > mp_replies_in ? mp_cmds_out - mp_replies_in : 0;
}

/* How far ahead of the peer this console may run before the barrier stops it.
 * Zero keeps the original behaviour: block on every single transfer until that
 * transfer's own reply is in hand.
 *
 * Blocking per transfer is what costs the host its clock — measured at 8.4 s of
 * an 11 s multiplayer phase, leaving it emulating at 0.13x while the client runs
 * near 1x. The pacing that matters is only ever "do not run far ahead of the
 * station", which is a window, not a stop. Shortening the stop instead was
 * tested and fails: the host free-runs and the client's queue overflows. */
static unsigned mp_window(void)
{
	static unsigned cached;
	if (!cached) {
		const char *setting = getenv("DRASTIC_WIFI_MP_WINDOW");
		unsigned long parsed = setting ? strtoul(setting, NULL, 10) : 0;
		cached = (parsed >= 1 && parsed <= 64) ? (unsigned)parsed + 1 : 1;
		fprintf(stderr, "[drastic-wifi] mp window=%u\n", cached - 1);
	}
	return cached - 1;
}

/* The host's microsecond counter as of the last command it sent. Both consoles
 * work in the host's time base — the client adopts it from every beacon — so
 * this is directly comparable with the client's own counter. */
static uint64_t peer_clock_us;
/* Wall-clock arrival of the command that set peer_clock_us. The hold must never
 * outlive the session: once the peer stops sending, this console's counter runs
 * past a frozen peer_clock_us for ever and the condition stays true. The first
 * version of this had no such guard and hung the client at shutdown, which is
 * how it was found. */
static uint64_t peer_clock_seen_ms;
static uint64_t clock_holds;
static uint64_t clock_hold_ns;

/* How far this console may run ahead of the peer's clock, in emulated
 * microseconds, before it waits for the peer to catch up. Zero disables it.
 *
 * The failure this addresses: the client disassociates itself after about 1.7 s
 * of its own emulated time, having decided the access point is not answering.
 * It is not wrong. The host emulates at roughly 0.13x during multiplayer while
 * the client runs near 1x, so in the client's frame of reference the host is
 * seven times too slow, and Nintendo's library times out exactly as it would
 * against real hardware behaving that way.
 *
 * Every attempt so far has tried to speed the host up. The symmetric answer is
 * the one netplay normally takes: both sides run at the speed of the slower.
 * Holding the client back costs nothing that matters — it is already waiting —
 * and it removes the divergence rather than chasing it. */
static unsigned clock_lead_us(void)
{
	static unsigned cached;
	if (!cached) {
		const char *setting = getenv("DRASTIC_WIFI_CLOCK_LEAD_US");
		unsigned long parsed = setting ? strtoul(setting, NULL, 10) : 0;
		cached = (parsed >= 1000 && parsed <= 5000000) ?
			(unsigned)parsed : 1;
		fprintf(stderr, "[drastic-wifi] clock lead=%u us%s\n",
			cached > 1 ? cached : 0, cached > 1 ? "" : " (off)");
	}
	return cached > 1 ? cached : 0;
}

static int beacon_in_barrier(void)
{
	static int cached;
	if (!cached) {
		const char *setting = getenv("DRASTIC_WIFI_BEACON_IN_BARRIER");
		cached = (setting && setting[0] == '1') ? 1 : -1;
		fprintf(stderr, "[drastic-wifi] beacon in barrier=%s\n",
			cached > 0 ? "on" : "off");
	}
	return cached > 0;
}

static unsigned mp_generation_limit(void)
{
	static unsigned cached;
	if (!cached) {
		const char *setting = getenv("DRASTIC_WIFI_MP_GENERATIONS");
		unsigned long parsed = setting ? strtoul(setting, NULL, 10) : 0;
		cached = (parsed >= 1 && parsed <= DWIFI_MP_GENERATIONS_MAX) ?
			(unsigned)parsed : DWIFI_MP_GENERATIONS;
		fprintf(stderr, "[drastic-wifi] mp generations=%u (%s)\n",
			cached, setting ? "environment" : "default");
	}
	return cached;
}
struct mp_generation {
	uint8_t cmd[NETLINK_MAX_PACKET];
	size_t cmd_length;
	uint8_t ack[NETLINK_MAX_PACKET];
	size_t ack_length;
	uint64_t arrived_ms;
};
static struct mp_generation mp_queue[DWIFI_MP_GENERATIONS_MAX];
static unsigned mp_queue_head;
static unsigned mp_queue_count;
static uint64_t mp_generations_dropped;
static unsigned mp_queue_high_water;
static uint8_t candidate_mp_cmd[NETLINK_MAX_PACKET];
static size_t candidate_mp_cmd_length;
static uint8_t pending_mp_reply[NETLINK_MAX_PACKET];
static size_t pending_mp_reply_length;
static uint64_t pending_mp_reply_ms;
static uint8_t pending_other[NETLINK_MAX_PACKET];
static size_t pending_other_length;
/* MP is an ordered command -> reply -> ACK exchange.  When a busy client has
 * both periodic roles pending, alternate them so neither stream can starve the
 * other. */
static int pending_mp_wants_ack;
static uint64_t last_beacon_rx_us;
/* A station stays associated by hearing beacons and gives up when it misses
 * several. Only the newest beacon is retained and delivery competes with MP
 * traffic, so a starved beacon stream is a candidate for the client dropping
 * out shortly after it joins. */
static uint64_t beacon_delivered_count;
static uint64_t beacon_max_gap_us;
static uint64_t beacon_starved_reports;

/* melonDS discards any queued multiplayer packet older than one video frame
 * before doing anything else with the queue: nothing stale is ever delivered.
 * Its transport is unreliable and unsequenced, so excess frames are simply
 * lost. This one is TCP and cannot drop anything, so without an explicit sweep
 * the client is left working through commands whose moment has passed, which is
 * what fills the queue and desynchronises the two consoles. */
#define DWIFI_STALE_FRAME_MS UINT64_C(16)
static uint64_t stale_generations_evicted;
static uint64_t stale_replies_evicted;

static uint64_t monotonic_ms(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * UINT64_C(1000) +
		(uint64_t)now.tv_nsec / UINT64_C(1000000);
}
/* Distinguishes a host that stops beaconing from a client transport that drops
 * them: the priority fix did not change the gaps, so which end is at fault
 * needs establishing rather than assuming. */
static uint64_t beacon_tx_count;
static uint64_t beacon_rx_count;
static uint64_t last_beacon_tx_us;
static uint64_t beacon_tx_reports;
static uint8_t local_firmware_mac[6];
static uint8_t local_wire_mac[6];
static int local_mac_known;
/* Set when both DraStic instances read the same firmware MAC and their frames
 * must be told apart on the wire. */
static int dwifi_transport_shared_mac;
static unsigned reply_wait_timeouts;
static unsigned mp_tx_counts[4];
static unsigned mp_rx_counts[4];

static void log_mp_progress(const char *direction, const uint8_t *frame,
	size_t length);

static int is_beacon(const uint8_t *frame, size_t length)
{
	if (!frame || length < 14)
		return 0;
	uint16_t control = (uint16_t)(frame[12] | ((uint16_t)frame[13] << 8));
	return ((control >> 2) & 3) == 0 && ((control >> 4) & 15) == 8;
}

static uint32_t wifi_crc32(const uint8_t *data, size_t length)
{
	uint32_t crc = UINT32_C(0xffffffff);
	for (size_t index = 0; index < length; ++index) {
		crc ^= data[index];
		for (unsigned bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (UINT32_C(0xedb88320) &
				(uint32_t)-(int32_t)(crc & 1));
	}
	return ~crc;
}

static void update_fcs(uint8_t *frame, size_t length)
{
	/* The transport frame includes a 12-byte radio header followed by the
	 * complete 802.11 frame, including its four-byte FCS. */
	if (length < 12 + 4)
		return;
	uint8_t *wifi = frame + 12;
	size_t wifi_length = length - 12;
	uint32_t fcs = wifi_crc32(wifi, wifi_length - 4);
	wifi[wifi_length - 4] = (uint8_t)fcs;
	wifi[wifi_length - 3] = (uint8_t)(fcs >> 8);
	wifi[wifi_length - 2] = (uint8_t)(fcs >> 16);
	wifi[wifi_length - 1] = (uint8_t)(fcs >> 24);
}

static void make_source_mac_unique(uint8_t *frame, size_t length)
{
	if (length < 12 + 24)
		return;
	uint8_t *wifi = frame + 12;
	uint8_t original[6];
	memcpy(original, wifi + 10, sizeof(original));
	uint8_t replacement[6];
	memcpy(replacement, original, sizeof(replacement));
	/* Each console now carries its own MAC in system/nds_firmware.bin, so no
	 * rewriting is needed and the guests see each other's real addresses. The
	 * translation stays in place for units that still share the shipped image;
	 * it is a no-op once the addresses already differ. */
	if (dwifi_transport_shared_mac)
		replacement[5] ^= NetLink_getRole() == NETLINK_ROLE_HOST ? 1 : 2;
	memcpy(local_firmware_mac, original, sizeof(local_firmware_mac));
	memcpy(local_wire_mac, replacement, sizeof(local_wire_mac));
	local_mac_known = 1;
	for (size_t offset = 4; offset <= 16; offset += 6) {
		if (memcmp(wifi + offset, original, sizeof(original)) == 0)
			memcpy(wifi + offset, replacement, sizeof(replacement));
	}
	update_fcs(frame, length);
}

static void restore_local_mac(uint8_t *frame, size_t length)
{
	if (!local_mac_known || length < 12 + 24)
		return;
	uint8_t *wifi = frame + 12;
	int changed = 0;
	/* The wire MAC is analogous to a NAT address. Translate only occurrences
	 * of this device's identity; the peer's unique source/BSSID must remain
	 * visible so Nintendo's software does not mistake it for itself. */
	for (size_t offset = 4; offset <= 16; offset += 6) {
		if (memcmp(wifi + offset, local_wire_mac,
			sizeof(local_wire_mac)) == 0) {
			memcpy(wifi + offset, local_firmware_mac,
				sizeof(local_firmware_mac));
			changed = 1;
		}
	}
	if (changed)
		update_fcs(frame, length);
}

/* Beacons repeat forever and would drown the log; every other frame is part of
 * the association handshake and is what needs reading. Bounded so a stuck
 * exchange cannot fill the device log. */
static void log_frame(const char *direction, const uint8_t *frame, size_t length)
{
	static unsigned logged;
	if (length < 12 + 24 || is_beacon(frame, length) || logged >= 64)
		return;
	logged++;
	const uint8_t *wifi = frame + 12;
	uint16_t control = (uint16_t)(wifi[0] | ((uint16_t)wifi[1] << 8));
	fprintf(stderr,
		"[drastic-wifi] frame dir=%s len=%zu fc=%04x type=%u sub=%u seq=%u"
		" da=%02x%02x%02x%02x%02x%02x sa=%02x%02x%02x%02x%02x%02x"
		" bssid=%02x%02x%02x%02x%02x%02x\n",
		direction, length, control, (control >> 2) & 3, (control >> 4) & 15,
		(unsigned)((wifi[22] | ((unsigned)wifi[23] << 8)) >> 4),
		wifi[4], wifi[5], wifi[6], wifi[7], wifi[8], wifi[9],
		wifi[10], wifi[11], wifi[12], wifi[13], wifi[14], wifi[15],
		wifi[16], wifi[17], wifi[18], wifi[19], wifi[20], wifi[21]);
	/* Management-frame bodies carry the reason/status codes and the game's
	 * vendor payload, which is what decides whether a join is accepted. */
	if (length <= 12 + 128) {
		fprintf(stderr, "[drastic-wifi] frame-body=");
		for (size_t index = 24; index + 12 < length; ++index)
			fprintf(stderr, "%02x", wifi[index]);
		fputc('\n', stderr);
	}
}


/* Out-of-band packet header, mirroring melonDS DS (src/libretro/net/mp.cpp):
 * a big-endian 64-bit timestamp, an association id, and a frame type. Both
 * melonDS implementations carry type and timestamp beside the frame rather
 * than inferring them; this transport had been re-deriving type from the
 * multicast MAC addresses, which is where the reply/command misclassifications
 * came from, and had no timestamp at all, so the staleness rule both of them
 * rely on was impossible to express.
 *
 * melonDS DS uses 0=other, 1=reply, 2=cmd and sends its ACK as a cmd. The ACK
 * is kept distinct here because this transport pairs a command with its closing
 * ACK into a generation before delivering, which melonDS DS has no need to do.
 *
 * The timestamp is the DS microsecond counter, which both consoles synchronise
 * from the host's beacons, so it is comparable across the link. */

/* melonDS DS rejects a reply older than the command it answers, less a small
 * tolerance: `p.Timestamp() < (timestamp - 32)`. */
#define DWIFI_REPLY_STALE_US UINT64_C(32)

static void write_mp_header(uint8_t *out, uint64_t timestamp, uint8_t aid,
	uint8_t type)
{
	for (unsigned index = 0; index < 8; ++index)
		out[index] = (uint8_t)(timestamp >> (56 - index * 8));
	out[8] = aid;
	out[9] = type;
}

static uint64_t read_mp_timestamp(const uint8_t *header)
{
	uint64_t value = 0;
	for (unsigned index = 0; index < 8; ++index)
		value = (value << 8) | header[index];
	return value;
}

static int frame_has_address(const uint8_t *frame, size_t length,
	size_t offset, const uint8_t address[6]);
static int mp_transport_role(const uint8_t *frame, size_t length);

static int frame_mp_type(const uint8_t *frame, size_t length)
{
	static const uint8_t reply_flow[6] = {3, 9, 0xbf, 0, 0, 0x10};
	if (frame_has_address(frame, length, 16, reply_flow))
		return DWIFI_MP_TYPE_REPLY;
	int role = mp_transport_role(frame, length);
	if (role == 1)
		return DWIFI_MP_TYPE_CMD;
	if (role == 2)
		return DWIFI_MP_TYPE_ACK;
	return DWIFI_MP_TYPE_OTHER;
}

/* Split a received packet into its melonDS-DS-shaped header and the radio
 * frame behind it. A packet shorter than the header is not one of ours. */
static uint64_t last_rx_timestamp;
static int last_rx_type;

static int split_mp_packet(uint8_t *packet, size_t length, uint8_t **body,
	size_t *body_length, uint64_t *timestamp, int *type)
{
	if (length < DWIFI_MP_HEADER_SIZE + 12)
		return 0;
	*timestamp = read_mp_timestamp(packet);
	*type = packet[9];
	*body = packet + DWIFI_MP_HEADER_SIZE;
	*body_length = length - DWIFI_MP_HEADER_SIZE;
	return 1;
}


/* Unreliable multiplayer channel.
 *
 * melonDS DS sends every multiplayer frame with
 * RETRO_NETPACKET_UNSEQUENCED | RETRO_NETPACKET_UNRELIABLE | FLUSH_HINT, and
 * melonDS's own LAN backend uses ENET_PACKET_FLAG_UNSEQUENCED. A frame that
 * cannot be delivered promptly is dropped, never queued behind others. That is
 * the property this transport lacked: NetLink is TCP, so a late frame is
 * retained and delivered stale, and a reply can sit behind the peer's own
 * backlog (head-of-line blocking) until the reply barrier times out.
 *
 * NetLink is shared with the other cores, so its semantics are left alone and a
 * datagram channel is opened alongside it, one port above the session port,
 * carrying multiplayer frames only. Everything else keeps the reliable path.
 * If the channel cannot be opened the transport silently falls back to TCP. */
static int mp_udp_fd = -1;
static struct sockaddr_in mp_udp_peer;
static int mp_udp_ready;
static uint64_t mp_udp_tx;
static uint64_t mp_udp_rx;
static uint64_t tcp_popped;
static uint64_t packets_processed;
static uint64_t packets_rejected;
/* The command this station is currently answering, in the shared DS
 * microsecond domain. melonDS DS rejects a reply predating it:
 * `if (p.Timestamp() < (timestamp - 32)) continue;` */
static uint64_t last_cmd_timestamp;
static uint64_t stale_replies_rejected;
/* Why the reply barrier costs what it does: how often it is entered, how often
 * it runs out, and how much wall time it actually consumes. */
static uint64_t barrier_entries;
static uint64_t barrier_timeouts;
static uint64_t barrier_ns;
static uint64_t barrier_short_circuits;

static void start_mp_datagrams(const char *session_path)
{
	char peer[64] = {0};
	unsigned port = 0;
	FILE *file = fopen(session_path, "r");
	if (!file)
		return;
	char line[128];
	while (fgets(line, sizeof(line), file)) {
		if (!strncmp(line, "port=", 5))
			port = (unsigned)strtoul(line + 5, NULL, 10);
		else if (!strncmp(line, "peer=", 5)) {
			size_t used = strcspn(line + 5, "\r\n");
			if (used < sizeof(peer)) {
				memcpy(peer, line + 5, used);
				peer[used] = 0;
			}
		}
	}
	fclose(file);
	if (!port || !peer[0])
		return;

	mp_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (mp_udp_fd < 0)
		return;
	int flags = fcntl(mp_udp_fd, F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(mp_udp_fd, F_SETFL, flags | O_NONBLOCK);

	struct sockaddr_in local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = htons((uint16_t)(port + 1));
	if (bind(mp_udp_fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
		close(mp_udp_fd);
		mp_udp_fd = -1;
		return;
	}

	memset(&mp_udp_peer, 0, sizeof(mp_udp_peer));
	mp_udp_peer.sin_family = AF_INET;
	mp_udp_peer.sin_port = htons((uint16_t)(port + 1));
	if (inet_pton(AF_INET, peer, &mp_udp_peer.sin_addr) != 1) {
		close(mp_udp_fd);
		mp_udp_fd = -1;
		return;
	}
	/* Accept datagrams only from the peer. The socket had been left
	 * unconnected on INADDR_ANY, so anything on the network that broadcast to
	 * this port was stripped of ten bytes and injected into the Wi-Fi model as
	 * a radio frame — observed in practice, with frames whose source address
	 * decoded to the ASCII "nextui" and a body containing "playwithme".
	 * Connecting the socket makes the kernel drop everything else. */
	if (connect(mp_udp_fd, (struct sockaddr *)&mp_udp_peer,
		sizeof(mp_udp_peer)) != 0) {
		close(mp_udp_fd);
		mp_udp_fd = -1;
		return;
	}
	mp_udp_ready = 1;
	fprintf(stderr, "[drastic-wifi] mp datagrams on port %u to %s\n",
		port + 1, peer);
}

static void stop_mp_datagrams(void)
{
	if (mp_udp_fd >= 0)
		close(mp_udp_fd);
	mp_udp_fd = -1;
	mp_udp_ready = 0;
	fprintf(stderr,
		"[drastic-wifi] barrier entries=%llu timeouts=%llu total_ms=%llu"
		" short_circuits=%llu would_reject_stale=%llu"
		" window_credits=%llu cmds_out=%llu replies_in=%llu"
		" clock_holds=%llu clock_hold_ms=%llu peer_clock_us=%llu\n",
		(unsigned long long)barrier_entries,
		(unsigned long long)barrier_timeouts,
		(unsigned long long)(barrier_ns / 1000000),
		(unsigned long long)barrier_short_circuits,
		(unsigned long long)stale_replies_rejected,
		(unsigned long long)barrier_window_credits,
		(unsigned long long)mp_cmds_out,
		(unsigned long long)mp_replies_in,
		(unsigned long long)clock_holds,
		(unsigned long long)(clock_hold_ns / 1000000),
		(unsigned long long)peer_clock_us);
	clock_holds = 0;
	clock_hold_ns = 0;
	peer_clock_us = 0;
	peer_clock_seen_ms = 0;
	barrier_window_credits = 0;
	mp_cmds_out = 0;
	mp_replies_in = 0;
	barrier_entries = 0;
	barrier_timeouts = 0;
	barrier_ns = 0;
	barrier_short_circuits = 0;
	stale_replies_rejected = 0;
	last_cmd_timestamp = 0;
	fprintf(stderr,
		"[drastic-wifi] rx path tcp_popped=%llu processed=%llu rejected=%llu\n",
		(unsigned long long)tcp_popped,
		(unsigned long long)packets_processed,
		(unsigned long long)packets_rejected);
	tcp_popped = 0;
	packets_processed = 0;
	packets_rejected = 0;
	fprintf(stderr, "[drastic-wifi] mp datagrams tx=%llu rx=%llu\n",
		(unsigned long long)mp_udp_tx, (unsigned long long)mp_udp_rx);
	mp_udp_tx = 0;
	mp_udp_rx = 0;
}

static int send_mp_datagram(const uint8_t *packet, size_t length)
{
	if (!mp_udp_ready)
		return 0;
	ssize_t sent = send(mp_udp_fd, packet, length, 0);
	if (sent < 0) {
		static unsigned reported;
		if (reported++ < 8)
			fprintf(stderr,
				"[drastic-wifi] mp datagram send failed: %s (len=%zu)\n",
				strerror(errno), length);
		return 0;
	}
	mp_udp_tx++;
	return 1;
}

static void send_frame(void *opaque, const uint8_t *frame, size_t length)
{
	if (is_beacon(frame, length)) {
		/* Report the interval this console actually beacons at, in its own
		 * emulated time. A host beaconing at the right emulated interval but
		 * running slow is a different fault from one whose beacon timer is
		 * wrong, and the two are indistinguishable from the client's side. */
		struct dwifi_model *sender = opaque;
		if (sender) {
			uint64_t gap = last_beacon_tx_us ?
				sender->us_timestamp - last_beacon_tx_us : 0;
			if (gap && beacon_tx_reports++ < 24)
				fprintf(stderr,
					"[drastic-wifi] beacon tx gap %llu us at us=%llu\n",
					(unsigned long long)gap,
					(unsigned long long)sender->us_timestamp);
			last_beacon_tx_us = sender->us_timestamp;
		}
		beacon_tx_count++;
	}
	if (length <= NETLINK_MAX_PACKET) {
		memcpy(outgoing_frame, frame, length);
		/* Hardware supplies the active RF channel in the TX radio header. Games
		 * commonly leave packet RAM byte 9 clear and expect the MAC to fill it. */
		if (length >= 12 && outgoing_frame[9] == 0)
			outgoing_frame[9] = 1;
		make_source_mac_unique(outgoing_frame, length);
		log_mp_progress("tx", outgoing_frame, length);
		if (tx_count++ == 0 && length >= 14)
		{
			fprintf(stderr,
				"[drastic-wifi] first-tx len=%zu rate=%u channel=%u framectl=%02x%02x\n",
				length, outgoing_frame[8], outgoing_frame[9],
				outgoing_frame[13], outgoing_frame[12]);
			fprintf(stderr, "[drastic-wifi] first-tx-hex=");
			for (size_t index = 0; index < length; ++index)
				fprintf(stderr, "%02x", outgoing_frame[index]);
			fputc('\n', stderr);
		}
		log_frame("tx", outgoing_frame, length);
		/* melonDS DS sets UNSEQUENCED | UNRELIABLE | FLUSH_HINT so a delayed
		 * frame is lost rather than queued. NetLink_send ignores its flags
		 * today (TCP), so the intent is recorded here and honoured once an
		 * unreliable datagram path exists. */
		struct dwifi_model *sender_model = opaque;
		uint8_t aid = 0;
		int type = frame_mp_type(outgoing_frame, length);
		if (sender_model) {
			if (type == DWIFI_MP_TYPE_REPLY)
				aid = (uint8_t)(dwifi_model_read16(sender_model,
					UINT32_C(0x04800028)) & 0x0f);
		}
		uint64_t stamp = sender_model ? sender_model->us_counter : 0;
		if (type == DWIFI_MP_TYPE_CMD) {
			last_cmd_timestamp = stamp;
			mp_cmds_out++;
		}
		write_mp_header(framed, stamp, aid, (uint8_t)type);
		memcpy(framed + DWIFI_MP_HEADER_SIZE, outgoing_frame, length);
		size_t framed_length = length + DWIFI_MP_HEADER_SIZE;
		if (type == DWIFI_MP_TYPE_OTHER ||
		    !send_mp_datagram(framed, framed_length))
			(void)NetLink_send(0, framed, framed_length,
				NetLink_remoteClientId());
	}
}

int dwifi_transport_start(struct dwifi_model *model, const char *session_path)
{
	if (!model || !NetLink_configure(session_path) || !NetLink_start())
		return 0;
	start_mp_datagrams(session_path);
	dwifi_model_set_send_callback(model, send_frame, model);
	dwifi_model_set_peer_wait_callback(model, dwifi_transport_await_for_peer,
		model);
	transport_active = 1;
	return 1;
}

/* Returns non-zero when the frame was a multiplayer reply, which is what a
 * command transfer is waiting for. */
static int deliver_frame(struct dwifi_model *model, uint8_t *frame,
	size_t length)
{
	/* A peer whose source address is our own firmware MAC means both units
	 * still run the shipped nds_firmware.bin. Translate from then on so the
	 * two consoles remain distinguishable on the wire. */
	if (local_mac_known && !dwifi_transport_shared_mac &&
	    length >= 12 + 16 &&
	    memcmp(frame + 12 + 10, local_firmware_mac,
		sizeof(local_firmware_mac)) == 0) {
		dwifi_transport_shared_mac = 1;
		fprintf(stderr,
			"[drastic-wifi] peer shares this unit's firmware MAC;"
			" enabling wire-address translation\n");
	}
	restore_local_mac(frame, length);
	log_mp_progress("rx", frame, length);
	if (rx_count++ == 0 && length >= 14)
		fprintf(stderr,
			"[drastic-wifi] first-rx len=%zu rate=%u channel=%u framectl=%02x%02x\n",
			length, frame[8], frame[9], frame[13], frame[12]);
	log_frame("rx", frame, length);

	/* A TCP connection may contain old periodic beacons queued before the
	 * guest enabled RX. Real radio would only expose the newest advertisement,
	 * never replay the entire backlog into the RX ring at once. */
	if (is_beacon(frame, length)) {
		memcpy(pending_beacon, frame, length);
		pending_beacon_length = length;
		return 0;
	}

	(void)dwifi_model_receive_frame(model, frame, length);

	static const uint8_t reply_flow[6] = {3, 9, 0xbf, 0, 0, 0x10};
	return length >= 12 + 22 &&
		memcmp(frame + 12 + 16, reply_flow, sizeof(reply_flow)) == 0;
}

static int frame_has_address(const uint8_t *frame, size_t length,
	size_t offset, const uint8_t address[6])
{
	return length >= 12 + offset + 6 &&
		memcmp(frame + 12 + offset, address, 6) == 0;
}

static int mp_transport_role(const uint8_t *frame, size_t length)
{
	static const uint8_t cmd_flow[6] = {3, 9, 0xbf, 0, 0, 0};
	static const uint8_t ack_flow[6] = {3, 9, 0xbf, 0, 0, 3};
	if (frame_has_address(frame, length, 4, cmd_flow))
		return 1;
	if (frame_has_address(frame, length, 4, ack_flow))
		return 2;
	return 0;
}

static uint16_t frame_sequence(const uint8_t *frame, size_t length)
{
	if (length < 12 + 24)
		return UINT16_C(0xffff);
	return (uint16_t)((frame[12 + 22] | ((uint16_t)frame[12 + 23] << 8)) >> 4);
}

static int mp_frame_kind(const uint8_t *frame, size_t length)
{
	static const uint8_t reply_flow[6] = {3, 9, 0xbf, 0, 0, 0x10};
	if (frame_has_address(frame, length, 16, reply_flow))
		return 3;
	int role = mp_transport_role(frame, length);
	return role;
}

static void log_mp_progress(const char *direction, const uint8_t *frame,
	size_t length)
{
	static unsigned long_frames_logged;
	int kind = mp_frame_kind(frame, length);
	if (!kind)
		return;
	unsigned *counts = direction[0] == 't' ? mp_tx_counts : mp_rx_counts;
	unsigned count = ++counts[kind];
	/* The lobby transition happens thousands of transfers after association.
	 * Keep a sparse heartbeat after the generic frame log reaches its cap so a
	 * trace still reveals which role stopped without flooding stderr. */
	/* A reply that is not the 40-byte empty default carries an actual payload.
	 * Those are rare and are exactly what the sparse heartbeat would hide, so
	 * they are always reported. */
	int populated_reply = kind == 3 && length != 40;
	if (!populated_reply && count > 8 && (count & (count - 1)) != 0)
		return;
	uint16_t word24 = length >= 12 + 26 ?
		(uint16_t)(frame[12 + 24] | ((uint16_t)frame[12 + 25] << 8)) : 0;
	uint16_t word26 = length >= 12 + 28 ?
		(uint16_t)(frame[12 + 26] | ((uint16_t)frame[12 + 27] << 8)) : 0;
	fprintf(stderr,
		"[drastic-wifi] mp-progress dir=%s kind=%d count=%u len=%zu"
		" seq=%u word24=%04x word26=%04x\n",
		direction, kind, count, length, frame_sequence(frame, length),
		word24, word26);
	/* The transition payload is rare (one frame in thousands of MP cycles),
	 * so retain its complete radio/802.11 bytes for offline comparison. */
	if (length > 44 && long_frames_logged++ < 4) {
		fprintf(stderr, "[drastic-wifi] mp-long dir=%s hex=", direction);
		for (size_t index = 0; index < length; ++index)
			fprintf(stderr, "%02x", frame[index]);
		fputc('\n', stderr);
	}
}

/* A radio does not reliably FIFO every frame transmitted while it is busy.
 * Retain the newest frame of each MP role so periodic command traffic can
 * supersede stale generations without losing the command/ACK distinction. */
static void stash_busy_frame(const uint8_t *frame, size_t length)
{
	static const uint8_t reply_flow[6] = {3, 9, 0xbf, 0, 0, 0x10};
	int mp_role = mp_transport_role(frame, length);
	uint8_t *target = pending_other;
	size_t *target_length = &pending_other_length;
	if (is_beacon(frame, length)) {
		target = pending_beacon;
		target_length = &pending_beacon_length;
	} else if (frame_has_address(frame, length, 16, reply_flow)) {
		target = pending_mp_reply;
		target_length = &pending_mp_reply_length;
		pending_mp_reply_ms = monotonic_ms();
	} else if (mp_role == 1) {
		memcpy(candidate_mp_cmd, frame, length);
		candidate_mp_cmd_length = length;
		return;
	} else if (mp_role == 2) {
		/* A station may send application data on the ACK multicast flow. The
		 * NetLink host has no incoming command to pair it with; deliver it as an
		 * ordinary hardware-classified ACK frame instead of silently dropping
		 * the payload. */
		if (NetLink_getRole() == NETLINK_ROLE_HOST &&
		    !candidate_mp_cmd_length) {
			target = pending_other;
			target_length = &pending_other_length;
			memcpy(target, frame, length);
			*target_length = length;
			return;
		}
		/* Commit only complete generations.  An ACK closes the most recently
		 * observed command; keeping that pair together prevents independent
		 * coalescing from splicing two host cycles. */
		if (!candidate_mp_cmd_length)
			return;
		uint16_t command_sequence = frame_sequence(candidate_mp_cmd,
			candidate_mp_cmd_length);
		uint16_t ack_sequence = frame_sequence(frame, length);
		if (command_sequence == UINT16_C(0xffff) ||
		    ack_sequence != (uint16_t)((command_sequence + 1) & 0x0fff)) {
			/* The command which this ACK closes was lost before reaching the
			 * bounded inbox.  Do not splice it onto a different generation. */
			candidate_mp_cmd_length = 0;
			return;
		}
		if (mp_queue_count >= mp_generation_limit()) {
			/* The client is further behind than the queue is deep. Keep the
			 * generations already held, in order, and count what is lost. */
			mp_generations_dropped++;
			candidate_mp_cmd_length = 0;
			return;
		}
		unsigned slot = (mp_queue_head + mp_queue_count) %
			mp_generation_limit();
		memcpy(mp_queue[slot].cmd, candidate_mp_cmd, candidate_mp_cmd_length);
		mp_queue[slot].cmd_length = candidate_mp_cmd_length;
		memcpy(mp_queue[slot].ack, frame, length);
		mp_queue[slot].ack_length = length;
		mp_queue[slot].arrived_ms = monotonic_ms();
		mp_queue_count++;
		if (mp_queue_count > mp_queue_high_water)
			mp_queue_high_water = mp_queue_count;
		candidate_mp_cmd_length = 0;
		return;
	}
	memcpy(target, frame, length);
	*target_length = length;
}


/* One place where a received packet is unwrapped and routed, shared by the
 * reliable and datagram paths. Returns the wire type, or -1 if the packet is
 * not one of ours. */
static int process_incoming_packet(struct dwifi_model *model, uint8_t *packet,
	size_t length)
{
	uint8_t *body;
	size_t body_length;
	uint64_t stamp;
	int wire_type;
	if (!split_mp_packet(packet, length, &body, &body_length, &stamp,
		&wire_type)) {
		packets_rejected++;
		static unsigned short_packets;
		if (short_packets++ < 8)
			fprintf(stderr,
				"[drastic-wifi] dropped undersized packet len=%zu\n", length);
		return -1;
	}
	last_rx_timestamp = stamp;
	last_rx_type = wire_type;
	/* melonDS DS rejects a reply predating the command it answers:
	 * `if (p.Timestamp() < (timestamp - 32)) continue;`. Ported literally, that
	 * rejected 1054 of 1347 replies and made the reply barrier time out on 1233
	 * of 1367 entries, costing 36 s of wall time in a single run.
	 *
	 * The rule depends on both instances sharing a tight clock domain. Ours do
	 * not: the client re-adopts the host's clock from every beacon while the
	 * host runs at a fraction of real time, so the two counters diverge and
	 * re-converge constantly and the comparison is not sound. Currency is
	 * instead enforced by the 16 ms arrival-age sweep, which uses local time and
	 * needs no agreement between the consoles.
	 *
	 * The count is kept, without the rejection, to measure how far apart the two
	 * clocks actually run. */
	if (wire_type == DWIFI_MP_TYPE_REPLY && last_cmd_timestamp &&
	    stamp + DWIFI_REPLY_STALE_US < last_cmd_timestamp)
		stale_replies_rejected++;
	packets_processed++;
	if (is_beacon(body, body_length))
		beacon_rx_count++;
	/* The sender states the type; it is not inferred from the multicast
	 * address, as neither melonDS backend infers it. */
	if (model->rx_active || wire_type == DWIFI_MP_TYPE_CMD ||
	    wire_type == DWIFI_MP_TYPE_ACK)
		stash_busy_frame(body, body_length);
	else
		(void)deliver_frame(model, body, body_length);
	/* Counted here rather than at the barrier's own recognition points. Most
	 * replies are consumed by an ordinary dwifi_transport_poll() before the
	 * model ever reaches its reply deadline, so counting them inside the
	 * barrier undercounted badly: outstanding grew without bound, the window
	 * was never again satisfiable, and every entry ran to a full timeout. */
	if (wire_type == DWIFI_MP_TYPE_REPLY)
		mp_replies_in++;
	if (wire_type == DWIFI_MP_TYPE_CMD && stamp > peer_clock_us) {
		peer_clock_us = stamp;
		peer_clock_seen_ms = monotonic_ms();
	}
	return wire_type;
}

static int drain_mp_datagrams(struct dwifi_model *model)
{
	int saw_reply = 0;
	if (!mp_udp_ready)
		return 0;
	for (;;) {
		ssize_t got = recv(mp_udp_fd, datagram_buffer,
			sizeof(datagram_buffer), 0);
		if (got <= 0)
			break;
		mp_udp_rx++;
		if (process_incoming_packet(model, datagram_buffer, (size_t)got) ==
			DWIFI_MP_TYPE_REPLY)
			saw_reply = 1;
	}
	return saw_reply;
}

static int deliver_pending(struct dwifi_model *model)
{
	uint8_t *frame = NULL;
	size_t *length = NULL;
	/* Replies have priority on the host because its command window is bounded.
	 * A client must see both sides of the periodic command/ACK exchange. */
	int delivering_cmd = 0;
	int delivering_ack = 0;
	struct mp_generation *head = mp_queue_count ?
		&mp_queue[mp_queue_head] : NULL;
	if (pending_mp_reply_length) {
		frame = pending_mp_reply;
		length = &pending_mp_reply_length;
	} else if (head && pending_mp_wants_ack) {
		frame = head->ack;
		length = &head->ack_length;
		delivering_ack = 1;
	} else if (head) {
		frame = head->cmd;
		length = &head->cmd_length;
		delivering_cmd = 1;
	} else if (pending_other_length) {
		frame = pending_other;
		length = &pending_other_length;
	}
	if (!length)
		return 0;
	size_t frame_length = *length;
	if (!delivering_cmd && !delivering_ack)
		*length = 0;
	/* Command delivery can synchronously send the automatic client reply and
	 * re-enter transport peer-wait handling.  Publish wants_ack before that
	 * happens so the nested poll cannot consume/promote the wrong generation. */
	if (delivering_cmd)
		pending_mp_wants_ack = 1;
	int result = deliver_frame(model, frame, frame_length) | 1;
	if (delivering_ack) {
		pending_mp_wants_ack = 0;
		head->cmd_length = 0;
		head->ack_length = 0;
		mp_queue_head = (mp_queue_head + 1) % mp_generation_limit();
		mp_queue_count--;
	}
	return result;
}

/* A command transfer's reply window is tens of microseconds of emulated time,
 * and emulated time tracks wall time, so a reply cannot cross a TCP link and
 * arrive inside it. The hardware behaviour that matters is that the host does
 * not proceed until the window is over, so stall here and let the receive
 * thread fill the queue: the peer is running the same code and converges on the
 * same point. Bounded, because a peer that is not in multiplayer mode must not
 * bring the emulator to a halt. */
void dwifi_transport_await_for_peer(void *opaque)
{
	struct dwifi_model *model = opaque;
	if (!transport_active || !model || !NetLink_isConnected())
		return;
	/* This callback is also used at the end of the post-beacon listening
	 * window.  That path predates the multiplayer barrier and only needs a
	 * brief opportunity to receive any late management frame.  Waiting 50 ms
	 * specifically for an MP reply there stalls both consoles once per beacon
	 * and eventually makes an otherwise healthy client time out. */
	int awaiting_mp_reply = model->mp_cmd_active != 0;
	unsigned attempts = awaiting_mp_reply ? reply_wait_attempts() :
		DWIFI_LINK_WAIT_ATTEMPTS;
	struct timespec barrier_start;
	if (awaiting_mp_reply) {
		/* The host runs at a fraction of the client's speed, so a reply is
		 * often already in hand — consumed by an ordinary poll — before the
		 * model reaches its reply deadline and asks to wait. Waiting 25 ms for
		 * something that has arrived is most of what this barrier was costing.
		 * Both timestamps are this console's own clock, so the comparison is
		 * sound where the cross-console one was not. */
		if (model->mp_last_reply_us &&
		    model->mp_last_reply_us >= model->mp_cmd_started_us) {
			barrier_short_circuits++;
			return;
		}
		/* The station is still within the window this console is allowed to run
		 * ahead of it. Nothing is owed yet, so do not stall. */
		if (mp_window() && mp_outstanding() <= mp_window()) {
			barrier_window_credits++;
			return;
		}
		barrier_entries++;
		(void)clock_gettime(CLOCK_MONOTONIC, &barrier_start);
	}
	for (unsigned attempt = 0; attempt < attempts; ++attempt) {
		size_t length;
		int delivered = 0;
		if (!model->rx_active)
			delivered |= deliver_pending(model);
		if (drain_mp_datagrams(model)) {
			model->mp_reply_seen = 1;
			model->mp_client_fail = 0;
			delivered |= 2;
		}
		while (NetLink_popPacket(incoming_frame, sizeof(incoming_frame),
			&length)) {
			int reply_arrived = process_incoming_packet(model,
				incoming_frame, length) == DWIFI_MP_TYPE_REPLY;
			delivered |= 1;
			/* Emulated RX airtime cannot advance while this wall-clock callback
			 * is blocked. A reply which arrived while another frame is active is
			 * therefore queued for packet-RAM delivery, but it has already met the
			 * command deadline on the medium and must close reply accounting now. */
			if (reply_arrived) {
				model->mp_reply_seen = 1;
				model->mp_client_fail = 0;
				delivered |= 2;
			}
		}
		/* Only a station reply closes this barrier. Management traffic and stale
		 * periodic frames still get delivered, but must not let the host run
		 * another command generation ahead of the client. */
		int satisfied;
		if (!awaiting_mp_reply)
			satisfied = delivered;
		else if (mp_window())
			satisfied = mp_outstanding() <= mp_window();
		else
			satisfied = (delivered & 2) != 0;
		if (satisfied) {
			if (awaiting_mp_reply) {
				struct timespec now;
				if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
					barrier_ns += (uint64_t)(now.tv_sec -
						barrier_start.tv_sec) * UINT64_C(1000000000) +
						(uint64_t)now.tv_nsec - (uint64_t)barrier_start.tv_nsec;
			}
			return;
		}
		usleep(DWIFI_REPLY_WAIT_STEP_US);
		/* This sleep is a transport artifact with no counterpart on hardware,
		 * and the emulated clock does not move across it — so the host's beacon
		 * timer is frozen for the whole stall while the client, which measures
		 * the interval in its own emulated time, keeps counting. That is the
		 * measured 1.2-1.4 s beacon gap. Keep the beacon obligation running on
		 * wall time here; the MAC sends it without the CPU. */
		if (beacon_in_barrier())
			dwifi_model_advance_beacon_wall_us(model,
				DWIFI_REPLY_WAIT_STEP_US);
	}
	if (!awaiting_mp_reply)
		return;
	barrier_timeouts++;
	{
		struct timespec now;
		if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
			barrier_ns += (uint64_t)(now.tv_sec - barrier_start.tv_sec) *
				UINT64_C(1000000000) + (uint64_t)now.tv_nsec -
				(uint64_t)barrier_start.tv_nsec;
	}
	reply_wait_timeouts++;
	if (reply_wait_timeouts <= 8)
		fprintf(stderr,
			"[drastic-wifi] peer wait timed out (%u)\n",
			reply_wait_timeouts);
}


/* A beacon is what keeps a station associated, and it must not lose its receive
 * window to multiplayer traffic. deliver_pending() takes every free window it
 * finds and marks the radio busy, so an overdue beacon offered afterwards never
 * got in: the client missed 16 to 22 consecutive beacon periods once commands
 * started flowing, and gave up on the network. Give an overdue beacon first
 * refusal instead. Returns non-zero when one was delivered. */

/* Mirrors melonDS ProcessLAN(): sweep anything older than one frame's time out
 * of the queue before delivering. The generation currently being delivered is
 * exempt — its command has already reached the guest and the matching ACK must
 * still follow it, or an ACK would be spliced onto a different host cycle. */
static void evict_stale_frames(void)
{
	uint64_t now = monotonic_ms();
	while (mp_queue_count && !pending_mp_wants_ack) {
		uint64_t arrived = mp_queue[mp_queue_head].arrived_ms;
		if (now < arrived || now - arrived <= DWIFI_STALE_FRAME_MS)
			break;
		mp_queue[mp_queue_head].cmd_length = 0;
		mp_queue[mp_queue_head].ack_length = 0;
		mp_queue_head = (mp_queue_head + 1) % mp_generation_limit();
		mp_queue_count--;
		stale_generations_evicted++;
	}
	if (pending_mp_reply_length && now >= pending_mp_reply_ms &&
	    now - pending_mp_reply_ms > DWIFI_STALE_FRAME_MS) {
		pending_mp_reply_length = 0;
		stale_replies_evicted++;
	}
}

static int deliver_overdue_beacon(struct dwifi_model *model)
{
	if (model->rx_active || !pending_beacon_length)
		return 0;
	if (model->us_timestamp - last_beacon_rx_us < UINT64_C(100000))
		return 0;
	if (!dwifi_model_receive_frame(model, pending_beacon,
		pending_beacon_length))
		return 0;
	uint64_t gap = last_beacon_rx_us ?
		model->us_timestamp - last_beacon_rx_us : 0;
	if (gap > beacon_max_gap_us)
		beacon_max_gap_us = gap;
	/* Five beacon periods without one is roughly where a station concludes the
	 * network is gone. */
	if (gap > UINT64_C(500000) && beacon_starved_reports++ < 16)
		fprintf(stderr,
			"[drastic-wifi] beacon gap %llu us after %llu delivered\n",
			(unsigned long long)gap,
			(unsigned long long)beacon_delivered_count);
	beacon_delivered_count++;
	last_beacon_rx_us = model->us_timestamp;
	pending_beacon_length = 0;
	return 1;
}

void dwifi_transport_poll(struct dwifi_model *model)
{
	size_t length;
	if (!transport_active || !model)
		return;
	/* NetLink_markFrame drives libretro frontend-stall detection. DraStic has no
	 * equivalent video-frame boundary here; using its fine-grained event hook
	 * creates false pause/resume storms and must remain disabled. */
	evict_stale_frames();
	(void)deliver_overdue_beacon(model);
	if (!model->rx_active)
		(void)deliver_pending(model);
	while (NetLink_popPacket(incoming_frame, sizeof(incoming_frame), &length)) {
		tcp_popped++;
		(void)process_incoming_packet(model, incoming_frame, length);
	}
	(void)drain_mp_datagrams(model);

	(void)deliver_overdue_beacon(model);

	/* Symmetric clock coupling. Run no further ahead of the peer than the
	 * configured lead, so the two consoles emulate at the same rate and neither
	 * library sees the other as unresponsive. Only a console that is receiving
	 * commands throttles, which is the station; the host has no peer clock to
	 * compare against and is unaffected. Bounded, so a peer that stops sending
	 * causes a stutter rather than a hang, and the beacon obligation is
	 * serviced across the wait exactly as it is in the reply barrier. */
	unsigned lead = clock_lead_us();
	/* Only while the peer is demonstrably still talking. DWIFI_PEER_CLOCK_FRESH_MS
	 * is generous against the measured 3 ms round trip but short enough that a
	 * peer which stops is noticed within one frame. */
	int peer_live = peer_clock_seen_ms &&
		monotonic_ms() - peer_clock_seen_ms < DWIFI_PEER_CLOCK_FRESH_MS;
	if (lead && peer_live && model->us_counter > peer_clock_us + lead) {
		struct timespec hold_start, hold_end;
		(void)clock_gettime(CLOCK_MONOTONIC, &hold_start);
		clock_holds++;
		/* One reply-barrier's worth per poll, not four. This is called well
		 * over a million times a run; a long hold here is not a pause, it is a
		 * hang. */
		for (unsigned attempt = 0; attempt < DWIFI_CLOCK_HOLD_ATTEMPTS;
		     ++attempt) {
			if (model->us_counter <= peer_clock_us + lead)
				break;
			if (monotonic_ms() - peer_clock_seen_ms >=
			    DWIFI_PEER_CLOCK_FRESH_MS)
				break;
			usleep(DWIFI_REPLY_WAIT_STEP_US);
			if (beacon_in_barrier())
				dwifi_model_advance_beacon_wall_us(model,
					DWIFI_REPLY_WAIT_STEP_US);
			while (NetLink_popPacket(incoming_frame,
				sizeof(incoming_frame), &length))
				(void)process_incoming_packet(model, incoming_frame,
					length);
			(void)drain_mp_datagrams(model);
		}
		if (clock_gettime(CLOCK_MONOTONIC, &hold_end) == 0)
			clock_hold_ns += (uint64_t)(hold_end.tv_sec -
				hold_start.tv_sec) * UINT64_C(1000000000) +
				(uint64_t)hold_end.tv_nsec -
				(uint64_t)hold_start.tv_nsec;
	}
}

void dwifi_transport_stop(void)
{
	if (transport_active) {
		NetLink_stop();
		stop_mp_datagrams();
	}
	transport_active = 0;
	pending_beacon_length = 0;
	candidate_mp_cmd_length = 0;
	fprintf(stderr,
		"[drastic-wifi] mp queue high_water=%u dropped=%llu"
		" stale_gen=%llu stale_reply=%llu\n",
		mp_queue_high_water,
		(unsigned long long)mp_generations_dropped,
		(unsigned long long)stale_generations_evicted,
		(unsigned long long)stale_replies_evicted);
	stale_generations_evicted = 0;
	stale_replies_evicted = 0;
	for (unsigned index = 0; index < DWIFI_MP_GENERATIONS_MAX; ++index) {
		mp_queue[index].cmd_length = 0;
		mp_queue[index].ack_length = 0;
	}
	mp_queue_head = 0;
	mp_queue_count = 0;
	mp_queue_high_water = 0;
	mp_generations_dropped = 0;
	pending_mp_reply_length = 0;
	pending_other_length = 0;
	pending_mp_wants_ack = 0;
	last_beacon_rx_us = 0;
	fprintf(stderr,
		"[drastic-wifi] beacons tx=%llu rx=%llu delivered=%llu"
		" max_gap_us=%llu\n",
		(unsigned long long)beacon_tx_count,
		(unsigned long long)beacon_rx_count,
		(unsigned long long)beacon_delivered_count,
		(unsigned long long)beacon_max_gap_us);
	beacon_tx_count = 0;
	beacon_rx_count = 0;
	last_beacon_tx_us = 0;
	beacon_tx_reports = 0;
	beacon_delivered_count = 0;
	beacon_max_gap_us = 0;
	beacon_starved_reports = 0;
	local_mac_known = 0;
	memset(mp_tx_counts, 0, sizeof(mp_tx_counts));
	memset(mp_rx_counts, 0, sizeof(mp_rx_counts));
}

int dwifi_transport_is_active(void)
{
	return transport_active;
}
