#include "drastic_wifi_transport.h"
#include "netlink.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* A host must not run dozens of command generations ahead of the client and
 * rely on the bounded radio inbox to discard them.  Give the peer one local
 * display/event scheduling interval to consume the command and answer.  This
 * remains bounded so an absent station causes a stutter, not a deadlock. */
#define DWIFI_REPLY_WAIT_ATTEMPTS 100
#define DWIFI_LINK_WAIT_ATTEMPTS  6
#define DWIFI_REPLY_WAIT_STEP_US  500

static int transport_active;
static unsigned tx_count;
static unsigned rx_count;
/* DraStic's event callback runs on a small native stack. The transport is
 * single-threaded at that boundary, so packet-sized scratch storage belongs in
 * static state rather than adding 8 KiB to every send and poll call frame. */
static uint8_t outgoing_frame[NETLINK_MAX_PACKET];
static uint8_t incoming_frame[NETLINK_MAX_PACKET];
static uint8_t pending_beacon[NETLINK_MAX_PACKET];
static size_t pending_beacon_length;
static uint8_t pending_mp_cmd[NETLINK_MAX_PACKET];
static size_t pending_mp_cmd_length;
static uint8_t pending_mp_ack[NETLINK_MAX_PACKET];
static size_t pending_mp_ack_length;
static uint8_t candidate_mp_cmd[NETLINK_MAX_PACKET];
static size_t candidate_mp_cmd_length;
static uint8_t next_mp_cmd[NETLINK_MAX_PACKET];
static size_t next_mp_cmd_length;
static uint8_t next_mp_ack[NETLINK_MAX_PACKET];
static size_t next_mp_ack_length;
static uint8_t pending_mp_reply[NETLINK_MAX_PACKET];
static size_t pending_mp_reply_length;
static uint8_t pending_other[NETLINK_MAX_PACKET];
static size_t pending_other_length;
/* MP is an ordered command -> reply -> ACK exchange.  When a busy client has
 * both periodic roles pending, alternate them so neither stream can starve the
 * other. */
static int pending_mp_wants_ack;
static uint64_t last_beacon_rx_us;
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

static void send_frame(void *opaque, const uint8_t *frame, size_t length)
{
	(void)opaque;
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
		(void)NetLink_send(0, outgoing_frame, length, NetLink_remoteClientId());
	}
}

int dwifi_transport_start(struct dwifi_model *model, const char *session_path)
{
	if (!model || !NetLink_configure(session_path) || !NetLink_start())
		return 0;
	dwifi_model_set_send_callback(model, send_frame, NULL);
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
	if (count > 8 && (count & (count - 1)) != 0)
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
		if (!pending_mp_cmd_length && !pending_mp_ack_length &&
		    !pending_mp_wants_ack) {
			memcpy(pending_mp_cmd, candidate_mp_cmd,
				candidate_mp_cmd_length);
			pending_mp_cmd_length = candidate_mp_cmd_length;
			memcpy(pending_mp_ack, frame, length);
			pending_mp_ack_length = length;
		} else {
			memcpy(next_mp_cmd, candidate_mp_cmd,
				candidate_mp_cmd_length);
			next_mp_cmd_length = candidate_mp_cmd_length;
			memcpy(next_mp_ack, frame, length);
			next_mp_ack_length = length;
		}
		candidate_mp_cmd_length = 0;
		return;
	}
	memcpy(target, frame, length);
	*target_length = length;
}

static int deliver_pending(struct dwifi_model *model)
{
	uint8_t *frame = NULL;
	size_t *length = NULL;
	/* Replies have priority on the host because its command window is bounded.
	 * A client must see both sides of the periodic command/ACK exchange. */
	if (pending_mp_reply_length) {
		frame = pending_mp_reply;
		length = &pending_mp_reply_length;
	} else if (pending_mp_ack_length &&
		(pending_mp_wants_ack || !pending_mp_cmd_length)) {
		frame = pending_mp_ack;
		length = &pending_mp_ack_length;
	} else if (pending_mp_cmd_length) {
		frame = pending_mp_cmd;
		length = &pending_mp_cmd_length;
	} else if (pending_mp_ack_length) {
		frame = pending_mp_ack;
		length = &pending_mp_ack_length;
	} else if (pending_other_length) {
		frame = pending_other;
		length = &pending_other_length;
	}
	if (!length)
		return 0;
	size_t frame_length = *length;
	*length = 0;
	if (frame == pending_mp_cmd)
		pending_mp_wants_ack = 1;
	/* Command delivery can synchronously send the automatic client reply and
	 * re-enter transport peer-wait handling.  Publish wants_ack before that
	 * happens so the nested poll cannot consume/promote the wrong generation. */
	int result = deliver_frame(model, frame, frame_length) | 1;
	if (frame == pending_mp_ack) {
		pending_mp_wants_ack = 0;
		if (next_mp_cmd_length && next_mp_ack_length) {
			memcpy(pending_mp_cmd, next_mp_cmd, next_mp_cmd_length);
			pending_mp_cmd_length = next_mp_cmd_length;
			memcpy(pending_mp_ack, next_mp_ack, next_mp_ack_length);
			pending_mp_ack_length = next_mp_ack_length;
			next_mp_cmd_length = 0;
			next_mp_ack_length = 0;
		}
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
	unsigned attempts = awaiting_mp_reply ? DWIFI_REPLY_WAIT_ATTEMPTS :
		DWIFI_LINK_WAIT_ATTEMPTS;
	for (unsigned attempt = 0; attempt < attempts; ++attempt) {
		size_t length;
		int delivered = 0;
		if (!model->rx_active)
			delivered |= deliver_pending(model);
		while (NetLink_popPacket(incoming_frame, sizeof(incoming_frame),
			&length)) {
			int reply_arrived = mp_frame_kind(incoming_frame, length) == 3;
			if (model->rx_active || mp_transport_role(incoming_frame, length))
				stash_busy_frame(incoming_frame, length);
			else
				delivered |= deliver_frame(model, incoming_frame, length) | 1;
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
		if (awaiting_mp_reply ? (delivered & 2) : delivered)
			return;
		usleep(DWIFI_REPLY_WAIT_STEP_US);
	}
	if (!awaiting_mp_reply)
		return;
	reply_wait_timeouts++;
	if (reply_wait_timeouts <= 8)
		fprintf(stderr,
			"[drastic-wifi] peer wait timed out (%u)\n",
			reply_wait_timeouts);
}

void dwifi_transport_poll(struct dwifi_model *model)
{
	size_t length;
	if (!transport_active || !model)
		return;
	/* NetLink_markFrame drives libretro frontend-stall detection. DraStic has no
	 * equivalent video-frame boundary here; using its fine-grained event hook
	 * creates false pause/resume storms and must remain disabled. */
	if (!model->rx_active)
		(void)deliver_pending(model);
	while (NetLink_popPacket(incoming_frame, sizeof(incoming_frame), &length)) {
		if (model->rx_active || mp_transport_role(incoming_frame, length))
			stash_busy_frame(incoming_frame, length);
		else
			(void)deliver_frame(model, incoming_frame, length);
	}

	if (!model->rx_active && pending_beacon_length &&
	    model->us_timestamp - last_beacon_rx_us >= UINT64_C(100000)) {
		if (dwifi_model_receive_frame(model, pending_beacon,
			pending_beacon_length)) {
			last_beacon_rx_us = model->us_timestamp;
			pending_beacon_length = 0;
		}
	}
}

void dwifi_transport_stop(void)
{
	if (transport_active)
		NetLink_stop();
	transport_active = 0;
	pending_beacon_length = 0;
	pending_mp_cmd_length = 0;
	pending_mp_ack_length = 0;
	candidate_mp_cmd_length = 0;
	next_mp_cmd_length = 0;
	next_mp_ack_length = 0;
	pending_mp_reply_length = 0;
	pending_other_length = 0;
	pending_mp_wants_ack = 0;
	last_beacon_rx_us = 0;
	local_mac_known = 0;
	memset(mp_tx_counts, 0, sizeof(mp_tx_counts));
	memset(mp_rx_counts, 0, sizeof(mp_rx_counts));
}

int dwifi_transport_is_active(void)
{
	return transport_active;
}
