#include "drastic_wifi_model.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void count_irq(void *opaque)
{
	(*(unsigned *)opaque)++;
}

struct packet_write { uint16_t offset; uint16_t value; unsigned calls; };
static void capture_packet_write(void *opaque, uint16_t offset, uint16_t value)
{
	struct packet_write *write = opaque;
	write->offset = offset;
	write->value = value;
	write->calls++;
}

/* capture_packet_write keeps only the newest write, which cannot answer
 * "did this particular store ever reach the guest". This keeps the whole
 * sequence so a single expected write can be looked for among many. */
struct write_log {
	struct { uint16_t offset; uint16_t value; } entries[256];
	unsigned calls;
};
static void log_packet_write(void *opaque, uint16_t offset, uint16_t value)
{
	struct write_log *log = opaque;
	if (log->calls < sizeof(log->entries) / sizeof(log->entries[0])) {
		log->entries[log->calls].offset = offset;
		log->entries[log->calls].value = value;
	}
	log->calls++;
}
static int write_log_has(const struct write_log *log, uint16_t offset,
	uint16_t value)
{
	unsigned limit = log->calls;
	if (limit > sizeof(log->entries) / sizeof(log->entries[0]))
		limit = sizeof(log->entries) / sizeof(log->entries[0]);
	for (unsigned index = 0; index < limit; ++index)
		if (log->entries[index].offset == offset &&
		    log->entries[index].value == value)
			return 1;
	return 0;
}

static int receive_frame_complete(struct dwifi_model *model,
	const uint8_t *frame, size_t length)
{
	int result = dwifi_model_receive_frame(model, frame, length);
	if (result && model->rx_active)
		dwifi_model_advance_us(model,
			model->rx_complete_at_us - model->us_timestamp + 7);
	return result;
}

static void test_reset_and_fixed_registers(void)
{
	struct dwifi_model model;
	dwifi_model_reset(&model);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800004)) == 0);
	assert(dwifi_model_read16(&model, UINT32_C(0x0480003c)) == UINT16_C(0x0200));
	assert(dwifi_model_read16(&model, UINT32_C(0x0480015e)) == 0);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800180)) == 0);
	assert(model.read_count == 4);
}

static void test_register_storage_and_mirroring(void)
{
	struct dwifi_model model;
	dwifi_model_reset(&model);
	dwifi_model_write16(&model, UINT32_C(0x04800210), UINT16_C(0x1234));
	assert(dwifi_model_read16(&model, UINT32_C(0x04801210)) == UINT16_C(0x1234));
	assert(model.write_count == 1);
	assert(model.read_count == 1);
}

static void test_packet_ram_mirroring(void)
{
	struct dwifi_model model;
	dwifi_model_reset(&model);
	dwifi_model_write16(&model, UINT32_C(0x04804022), UINT16_C(0xbeef));
	assert(dwifi_model_read16(&model, UINT32_C(0x04804022)) == UINT16_C(0xbeef));
	assert(dwifi_model_read16(&model, UINT32_C(0x0480c022)) == UINT16_C(0xbeef));
	/* 0x6000 returns to register mirrors; it is not a second RAM mirror. */
	dwifi_model_write16(&model, UINT32_C(0x04806022), UINT16_C(0x55aa));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800022)) == UINT16_C(0x55aa));
}

struct sent_frame { uint8_t bytes[128]; size_t length; unsigned calls; };
static void capture_frame(void *opaque, const uint8_t *frame, size_t length)
{
	struct sent_frame *sent = opaque;
	assert(length <= sizeof(sent->bytes));
	memcpy(sent->bytes, frame, length);
	sent->length = length;
	sent->calls++;
}

static void test_automatic_beacon(void)
{
	struct dwifi_model model;
	struct sent_frame sent = {0};
	dwifi_model_reset(&model);
	dwifi_model_set_send_callback(&model, capture_frame, &sent);
	dwifi_model_write16(&model, UINT32_C(0x0480c00a), UINT16_C(40));
	for (unsigned index = 0; index < 8; index += 2)
		dwifi_model_write16(&model, UINT32_C(0x0480c024) + index,
			UINT16_C(0xa5a5));
	dwifi_model_write16(&model, UINT32_C(0x0480c022), UINT16_C(0xffff));
	dwifi_model_write16(&model, UINT32_C(0x04800210), UINT16_C(7));
	dwifi_model_write16(&model, UINT32_C(0x048000e8), UINT16_C(1));
	dwifi_model_write16(&model, UINT32_C(0x04800080), UINT16_C(0x8000));
	dwifi_model_write16(&model, UINT32_C(0x0480008c), UINT16_C(10));

	/* Park the compare well past the test window so the beacon counter is the
	 * only interrupt source; a compare of zero matches immediately. */
	dwifi_model_write16(&model, UINT32_C(0x048000f2), UINT16_C(1));
	dwifi_model_write16(&model, UINT32_C(0x048000ea), UINT16_C(1));
	/* W_BeaconCount1 loads from W_BeaconInterval and counts down per
	 * millisecond; the beacon goes out when it reaches zero. */
	dwifi_model_advance_us(&model, 11000);
	assert(sent.calls == 1 && sent.length == 52);
	assert(sent.bytes[12 + 22] == UINT8_C(0x70));
	assert(sent.bytes[12 + 23] == 0);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800210)) == 8);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800010)) &
		UINT16_C(1 << 14));
	/* Each beacon arms the post-beacon countdown. */
	assert(dwifi_model_read16(&model, UINT32_C(0x04800134)) != 0);

	dwifi_model_advance_us(&model, 10000);
	assert(sent.calls == 2);
	assert(sent.bytes[12 + 22] == UINT8_C(0x80));
}

/* The beacon rides on the beacon interrupt, so a guest that has not enabled it
 * does not beacon however long the timer runs. */
static void test_beacon_requires_enabled_interrupt(void)
{
	struct dwifi_model model;
	struct sent_frame sent = {0};
	dwifi_model_reset(&model);
	dwifi_model_set_send_callback(&model, capture_frame, &sent);
	dwifi_model_write16(&model, UINT32_C(0x0480c00a), UINT16_C(40));
	dwifi_model_write16(&model, UINT32_C(0x048000e8), UINT16_C(1));
	dwifi_model_write16(&model, UINT32_C(0x04800080), UINT16_C(0x8000));
	dwifi_model_write16(&model, UINT32_C(0x0480008c), UINT16_C(10));
	dwifi_model_advance_us(&model, 40000);
	assert(sent.calls == 0);
	assert(!(dwifi_model_read16(&model, UINT32_C(0x04800010)) &
		UINT16_C(1 << 14)));
}

/* Auth and association frames arriving late still hold the host's post-beacon
 * window open; without that the host has stopped listening by the time the
 * association request lands. */
static void count_wait(void *opaque)
{
	(*(unsigned *)opaque)++;
}

/* The link gets a chance to deliver before a listening window closes, because
 * the guest measures that window in emulated time which passes far faster than
 * the link's real latency. */
static void test_post_beacon_window_waits_for_the_link(void)
{
	struct dwifi_model model;
	unsigned waits = 0;
	dwifi_model_reset(&model);
	dwifi_model_set_peer_wait_callback(&model, count_wait, &waits);
	dwifi_model_write16(&model, UINT32_C(0x04800134), UINT16_C(2));
	dwifi_model_advance_us(&model, 1000);
	assert(waits == 0);
	dwifi_model_advance_us(&model, 1000);
	assert(waits == 1);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800010)) &
		UINT16_C(1 << 13));
}

static void test_post_beacon_window_extends_for_association(void)
{
	struct dwifi_model model;
	uint8_t frame[12 + 30] = {0};
	dwifi_model_reset(&model);
	dwifi_model_write16(&model, UINT32_C(0x04800050), UINT16_C(0x4000));
	dwifi_model_write16(&model, UINT32_C(0x04800052), UINT16_C(0x4800));
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8001));
	dwifi_model_write16(&model, UINT32_C(0x04800018), UINT16_C(0x2211));
	dwifi_model_write16(&model, UINT32_C(0x0480001a), UINT16_C(0x4433));
	dwifi_model_write16(&model, UINT32_C(0x0480001c), UINT16_C(0x6655));
	dwifi_model_write16(&model, UINT32_C(0x04800134), UINT16_C(100));

	frame[10] = 30;
	frame[12] = 0xb0;
	static const uint8_t us[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	memcpy(frame + 12 + 4, us, sizeof(us));
	assert(receive_frame_complete(&model, frame, sizeof(frame)) == 1);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800134)) == 110);

	/* A unicast frame addressed to another station does not hold it open. */
	frame[12 + 4] = 0x22;
	assert(receive_frame_complete(&model, frame, sizeof(frame)) == 1);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800134)) == 110);
}

static void test_tx_path(void)
{
	struct dwifi_model model;
	struct sent_frame sent = {0};
	dwifi_model_reset(&model);
	dwifi_model_set_send_callback(&model, capture_frame, &sent);
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8000));
	for (unsigned i = 0; i < 16; i += 2)
		dwifi_model_write16(&model, UINT32_C(0x04804000) + i,
			(uint16_t)(i | ((i + 1) << 8)));
	dwifi_model_write16(&model, UINT32_C(0x0480400a), UINT16_C(4));
	dwifi_model_write16(&model, UINT32_C(0x048000a0), UINT16_C(0x8000));
	dwifi_model_write16(&model, UINT32_C(0x048000ae), UINT16_C(1));
	assert(sent.calls == 1 && sent.length == 16);
	assert(sent.bytes[12] == 12 && sent.bytes[15] == 15);
	assert(dwifi_model_read16(&model, UINT32_C(0x048000a0)) == 0);
	assert(dwifi_model_read16(&model, UINT32_C(0x048000b8)) == 1);
}

static void test_tx_request_before_slot(void)
{
	struct dwifi_model model;
	struct sent_frame sent = {0};
	dwifi_model_reset(&model);
	dwifi_model_set_send_callback(&model, capture_frame, &sent);
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8000));
	for (unsigned i = 0; i < 16; i += 2)
		dwifi_model_write16(&model, UINT32_C(0x04804000) + i,
			(uint16_t)(i | ((i + 1) << 8)));
	dwifi_model_write16(&model, UINT32_C(0x0480400a), UINT16_C(4));
	dwifi_model_write16(&model, UINT32_C(0x048000ae), UINT16_C(1));
	assert(sent.calls == 0);
	dwifi_model_write16(&model, UINT32_C(0x048000a0), UINT16_C(0x8000));
	assert(sent.calls == 1 && sent.length == 16);
}

/* Mario Kart's host replies to an authentication frame by setting W_TXREQ bit
 * 3 and then programming the slot register at 0x00a4. A fixed bit-to-slot map
 * pairs those differently and drops the reply. */
static void test_tx_request_bit_does_not_select_slot(void)
{
	struct dwifi_model model;
	struct sent_frame sent = {0};
	dwifi_model_reset(&model);
	dwifi_model_set_send_callback(&model, capture_frame, &sent);
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8000));
	for (unsigned i = 0; i < 16; i += 2)
		dwifi_model_write16(&model, UINT32_C(0x04804000) + i,
			(uint16_t)(i | ((i + 1) << 8)));
	dwifi_model_write16(&model, UINT32_C(0x0480400a), UINT16_C(4));
	dwifi_model_write16(&model, UINT32_C(0x048000ae), UINT16_C(8));
	dwifi_model_write16(&model, UINT32_C(0x048000a4), UINT16_C(0x8000));
	assert(sent.calls == 1 && sent.length == 16);
	/* One request cannot send the same slot twice. */
	dwifi_model_write16(&model, UINT32_C(0x048000ae), UINT16_C(8));
	assert(sent.calls == 1);
}

/* The guest reads the microsecond counter through the same ownership gate as
 * the interrupt and transmit registers; DraStic supplies zero for it. */
static void test_microsecond_counter_is_model_owned(void)
{
	assert(dwifi_model_owns_read(UINT32_C(0x048000e8)));
	assert(dwifi_model_owns_read(UINT32_C(0x048000f8)));
	assert(dwifi_model_owns_read(UINT32_C(0x048000fe)));

	struct dwifi_model model;
	dwifi_model_reset(&model);
	dwifi_model_write16(&model, UINT32_C(0x048000e8), UINT16_C(1));
	dwifi_model_advance_us(&model, 4096);
	assert(dwifi_model_read16(&model, UINT32_C(0x048000f8)) == UINT16_C(0x1000));
}

/* Every transmitted frame must carry its own sequence number, or a receiver's
 * duplicate filter drops everything after the first frame from that source. */
static void test_tx_frames_carry_sequence_numbers(void)
{
	struct dwifi_model model;
	struct sent_frame sent = {0};
	dwifi_model_reset(&model);
	dwifi_model_set_send_callback(&model, capture_frame, &sent);
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8000));
	dwifi_model_write16(&model, UINT32_C(0x0480400a), UINT16_C(24));
	dwifi_model_write16(&model, UINT32_C(0x048000ae), UINT16_C(1));
	dwifi_model_write16(&model, UINT32_C(0x048000a0), UINT16_C(0x8000));
	assert(sent.calls == 1);
	assert(sent.bytes[12 + 22] == 0 && sent.bytes[12 + 23] == 0);
	/* The medium is occupied for the first frame's airtime, so the second
	 * cannot go out in the same instant. Hardware serialises the air; let it
	 * clear, as test_medium_serialises_transmission() covers directly. */
	dwifi_model_advance_us(&model, 512);
	dwifi_model_write16(&model, UINT32_C(0x048000a0), UINT16_C(0x8000));
	assert(sent.calls == 2);
	assert(sent.bytes[12 + 22] == UINT8_C(0x10));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800210)) == 2);
}

/* The guest polls W_RFPins constantly; the transceiver must report a state it
 * recognises, and the transmit path must stamp its outcome back into the
 * header where Nintendo's wireless library looks for it. */
static void test_rf_status_and_tx_header_writeback(void)
{
	struct dwifi_model model;
	struct sent_frame sent = {0};
	dwifi_model_reset(&model);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800214)) == 1);
	assert(dwifi_model_read16(&model, UINT32_C(0x0480019c)) == UINT16_C(0x84));
	/* Tracked but deliberately not served to the guest yet: doing so regressed
	 * the join while the radio state is still partial. See dwifi_model_owns_read. */
	assert(!dwifi_model_owns_read(UINT32_C(0x04800214)));

	dwifi_model_set_send_callback(&model, capture_frame, &sent);
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8000));
	dwifi_model_write16(&model, UINT32_C(0x0480400a), UINT16_C(24));
	dwifi_model_write16(&model, UINT32_C(0x04804004), UINT16_C(0xffff));
	dwifi_model_write16(&model, UINT32_C(0x048000ae), UINT16_C(1));
	dwifi_model_write16(&model, UINT32_C(0x048000a0), UINT16_C(0x8000));
	assert(sent.calls == 1);

	/* Transmitting reports the start of the frame and returns to idle. */
	assert(dwifi_model_read16(&model, UINT32_C(0x04800010)) &
		UINT16_C(1 << 7));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800214)) == 1);

	/* The header carries the completion status, and byte 5 is cleared. */
	assert(dwifi_model_read16(&model, UINT32_C(0x04804000)) == UINT16_C(1));
	assert(dwifi_model_read16(&model, UINT32_C(0x04804004)) ==
		UINT16_C(0x00ff));
}

/* A command transfer polls the clients its own body names, acknowledges when
 * the reply window closes, and reports the outcome back into the header. */
static void test_mp_command_transfer(void)
{
	struct dwifi_model model;
	struct sent_frame sent = {0};
	unsigned waits = 0;
	dwifi_model_reset(&model);
	dwifi_model_set_send_callback(&model, capture_frame, &sent);
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8000));
	dwifi_model_write16(&model, UINT32_C(0x04800008), UINT16_C(0x6000));
	dwifi_model_write16(&model, UINT32_C(0x048000c4), UINT16_C(10));
	dwifi_model_write16(&model, UINT32_C(0x04800210), UINT16_C(7));
	dwifi_model_set_peer_wait_callback(&model, count_wait, &waits);
	/* A command frame naming one client. */
	dwifi_model_write16(&model, UINT32_C(0x0480400a), UINT16_C(30));
	/* The polled-client mask sits two bytes into the frame body. */
	dwifi_model_write16(&model, UINT32_C(0x04804026), UINT16_C(0x0002));
	dwifi_model_write16(&model, UINT32_C(0x048000ae), UINT16_C(1));
	/* DraStic exposes Mario Kart's MP command through LOC2 rather than the
	 * nominal command slot; frame semantics must still select the MP path. */
	dwifi_model_write16(&model, UINT32_C(0x0480400c), UINT16_C(0x0228));
	dwifi_model_write16(&model, UINT32_C(0x04804010), UINT16_C(0x0903));
	dwifi_model_write16(&model, UINT32_C(0x04804012), UINT16_C(0x00bf));
	dwifi_model_write16(&model, UINT32_C(0x04804014), UINT16_C(0x0000));
	dwifi_model_write16(&model, UINT32_C(0x048000a4), UINT16_C(0x8000));

	assert(sent.calls == 1);
	/* Reported as sent, then the transceiver waits for the replies. */
	assert(dwifi_model_read16(&model, UINT32_C(0x048000b8)) ==
		UINT16_C(0x0800));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800214)) == 5);
	assert(dwifi_model_read16(&model, UINT32_C(0x048000b6)) & 4);
	assert(dwifi_model_read16(&model, UINT32_C(0x048000a4)) &
		UINT16_C(0x8000));
	assert(!(dwifi_model_read16(&model, UINT32_C(0x04800010)) &
		UINT16_C(1 << 12)));

	/* The transport wait belongs at the end of the reply window, not while the
	 * command has only just begun transmitting. */
	assert(waits == 0);

	/* Nobody answers. The reply window starts only after the 1 Mbit command's
	 * 432 us transmit time, then the MAC begins its generated ACK. */
	dwifi_model_advance_us(&model, 64);
	assert(sent.calls == 1);
	assert(!(dwifi_model_read16(&model, UINT32_C(0x04800010)) &
		UINT16_C(1 << 12)));
	dwifi_model_advance_us(&model, 408);
	assert(waits == 1);
	assert(sent.calls == 2 && sent.length == 12 + 32);
	assert(sent.bytes[12 + 22] == UINT8_C(0x80));
	assert(model.mp_ack_active);
	assert(!(dwifi_model_read16(&model, UINT32_C(0x04800010)) &
		UINT16_C(1 << 12)));
	/* IRQ12 and the command header outcome publish when the ACK airtime ends. */
	dwifi_model_advance_us(&model, 448);
	assert(dwifi_model_read16(&model, UINT32_C(0x048000b8)) ==
		UINT16_C(0x0b01));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800010)) &
		UINT16_C(1 << 12));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800268)) ==
		UINT16_C(0x0fc0));
	assert(!(dwifi_model_read16(&model, UINT32_C(0x048000b6)) & 4));
	assert(!(dwifi_model_read16(&model, UINT32_C(0x048000a4)) &
		UINT16_C(0x8000)));
	/* The header carries the failure status and the clients still missing. */
	assert(dwifi_model_read16(&model, UINT32_C(0x04804000)) == UINT16_C(5));
	assert(dwifi_model_read16(&model, UINT32_C(0x04804002)) == UINT16_C(2));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800210)) == UINT16_C(9));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800214)) == 1);
}

/* Receiving a command frame makes a station answer without the guest asking. */
/* Cross the call boundary a scheduled multiplayer reply is owed to. The model
 * deliberately will not answer inside the call that received the command, so a
 * test must let the clock move and then re-enter, exactly as the emulator's
 * event hook does around a turn of the guest CPU. */
static void run_scheduled_reply(struct dwifi_model *model)
{
	/* Comfortably past the model's turnaround, then re-enter. */
	dwifi_model_advance_us(model, 64);
	dwifi_model_advance_us(model, 8);
}

static void test_mp_reply_is_automatic(void)
{
	struct dwifi_model model;
	struct sent_frame sent = {0};
	uint8_t frame[12 + 30] = {0};
	dwifi_model_reset(&model);
	dwifi_model_set_send_callback(&model, capture_frame, &sent);
	dwifi_model_write16(&model, UINT32_C(0x04800050), UINT16_C(0x4000));
	dwifi_model_write16(&model, UINT32_C(0x04800052), UINT16_C(0x4800));
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8001));
	dwifi_model_write16(&model, UINT32_C(0x04800008), UINT16_C(0x1000));

	frame[10] = 30;
	frame[12] = 0x28;
	frame[13] = 0x02;
	frame[12 + 24] = 0xff;
	frame[12 + 25] = 0x7f;
	static const uint8_t cmd_flow[6] = {3, 9, 0xbf, 0, 0, 0};
	memcpy(frame + 12 + 4, cmd_flow, sizeof(cmd_flow));
	/* FromDS data frames carry the BSSID in address 2. */
	static const uint8_t bssid[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	memcpy(frame + 12 + 10, bssid, sizeof(bssid));
	dwifi_model_write16(&model, UINT32_C(0x04800020), UINT16_C(0x2211));
	dwifi_model_write16(&model, UINT32_C(0x04800022), UINT16_C(0x4433));
	dwifi_model_write16(&model, UINT32_C(0x04800024), UINT16_C(0x6655));
	assert(receive_frame_complete(&model, frame, sizeof(frame)) == 1);
	assert(dwifi_model_read16(&model, UINT32_C(0x04804000)) ==
		UINT16_C(0x801c));

	/* Nothing is answered from inside the receiving call: the guest has not run
	 * yet and could not have staged anything. */
	assert(sent.calls == 0);
	run_scheduled_reply(&model);

	/* With no reply slot armed the station still answers, with an empty frame. */
	assert(sent.calls == 1 && sent.length == 12 + 28);
	assert(sent.bytes[12] == UINT8_C(0x58) && sent.bytes[13] == UINT8_C(0x01));
	static const uint8_t reply_flow[6] = {3, 9, 0xbf, 0, 0, 0x10};
	assert(memcmp(sent.bytes + 12 + 16, reply_flow, sizeof(reply_flow)) == 0);
	assert(dwifi_model_read16(&model, UINT32_C(0x048000b6)) &
		UINT16_C(0x0080));
	assert(dwifi_model_read16(&model, UINT32_C(0x048000b8)) == 0);
	dwifi_model_advance_us(&model, 304);
	assert(!(dwifi_model_read16(&model, UINT32_C(0x048000b6)) &
		UINT16_C(0x0080)));
	assert(dwifi_model_read16(&model, UINT32_C(0x048000b8)) ==
		UINT16_C(0x0401));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800010)) &
		UINT16_C(1 << 7));
}

/* A station's populated reply slot uses the 03:09:bf:00:00:03 destination
 * also used by the host's generated ACK. During an active host command it is
 * nevertheless a reply and must clear the outstanding client mask. */
static void test_populated_mp_reply_uses_ack_flow(void)
{
	struct dwifi_model model;
	uint8_t frame[12 + 40] = {0};
	dwifi_model_reset(&model);
	dwifi_model_write16(&model, UINT32_C(0x04800050), UINT16_C(0x4000));
	dwifi_model_write16(&model, UINT32_C(0x04800052), UINT16_C(0x4800));
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8001));
	model.mp_cmd_active = 1;
	model.mp_client_fail = 2;
	model.mp_reply_deadline_us = UINT64_MAX;
	frame[10] = 40;
	frame[12] = 0x18;
	frame[13] = 0x02;
	static const uint8_t ack_flow[6] = {3, 9, 0xbf, 0, 0, 3};
	static const uint8_t reply_flow[6] = {3, 9, 0xbf, 0, 0, 0x10};
	memcpy(frame + 12 + 4, ack_flow, sizeof(ack_flow));
	memcpy(frame + 12 + 16, reply_flow, sizeof(reply_flow));
	assert(receive_frame_complete(&model, frame, sizeof(frame)) == 1);
	assert(dwifi_model_read16(&model, UINT32_C(0x04804000)) ==
		UINT16_C(0x801e));
	assert(model.mp_reply_seen);
	assert(model.mp_client_fail == 0);
}

/* An armed reply slot is sent once and latches across, so the guest can tell
 * its frame went out. */
static void test_mp_reply_slot_latches(void)
{
	struct dwifi_model model;
	struct sent_frame sent = {0};
	uint8_t frame[12 + 30] = {0};
	dwifi_model_reset(&model);
	dwifi_model_set_send_callback(&model, capture_frame, &sent);
	dwifi_model_write16(&model, UINT32_C(0x04800050), UINT16_C(0x4000));
	dwifi_model_write16(&model, UINT32_C(0x04800052), UINT16_C(0x4800));
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8001));
	/* Reply slot 0x8640 points at packet offset 0xc80. */
	dwifi_model_write16(&model, UINT32_C(0x04804c8a), UINT16_C(26));
	dwifi_model_write16(&model, UINT32_C(0x04800094), UINT16_C(0x8640));

	frame[10] = 30;
	frame[12] = 0x28;
	frame[13] = 0x02;
	frame[12 + 24] = 0xff;
	frame[12 + 25] = 0x7f;
	static const uint8_t cmd_flow[6] = {3, 9, 0xbf, 0, 0, 0};
	memcpy(frame + 12 + 4, cmd_flow, sizeof(cmd_flow));
	assert(receive_frame_complete(&model, frame, sizeof(frame)) == 1);
	assert(sent.calls == 0);
	run_scheduled_reply(&model);

	assert(sent.calls == 1 && sent.length == 12 + 26);
	assert(sent.bytes[4] == 1);
	static const uint8_t reply_flow[6] = {3, 9, 0xbf, 0, 0, 0x10};
	assert(memcmp(sent.bytes + 12 + 16, reply_flow, sizeof(reply_flow)) == 0);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800094)) == 0);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800098)) ==
		UINT16_C(0x8640));
}


/* Real hardware serialises the air. This model honoured that on the receiving
 * side only — rx_active holds for each frame's airtime — while the transmit
 * path emitted in zero emulated time, so the host out-produced what the
 * client's receiver could accept and the wall-clock reply barrier was left
 * compensating for it. */
static void test_medium_serialises_transmission(void)
{
	struct dwifi_model model;
	struct sent_frame sent = {0};
	dwifi_model_reset(&model);
	dwifi_model_set_send_callback(&model, capture_frame, &sent);
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8000));
	dwifi_model_write16(&model, UINT32_C(0x0480400a), UINT16_C(24));
	dwifi_model_write16(&model, UINT32_C(0x048000ae), UINT16_C(1));

	dwifi_model_write16(&model, UINT32_C(0x048000a0), UINT16_C(0x8000));
	assert(sent.calls == 1);

	/* A 24-byte frame at 1 Mbit/s occupies 192 + 24 * 8 = 384 us. Asking again
	 * inside that window must not put a second frame on the air. */
	dwifi_model_write16(&model, UINT32_C(0x048000a0), UINT16_C(0x8000));
	assert(sent.calls == 1);
	dwifi_model_advance_us(&model, 200);
	assert(sent.calls == 1);

	/* And it must not be lost either: fire_tx() runs only from the guest's
	 * register writes, so the retry has to come from the model's own clock. */
	dwifi_model_advance_us(&model, 200);
	assert(sent.calls == 2);
}


/* The beacon occupies the air like any other frame but is never made to wait
 * for it. Deferring the frame that keeps a station associated is precisely the
 * beacon starvation this project measured at length. */
static void test_beacon_is_not_blocked_by_a_busy_medium(void)
{
	struct dwifi_model model;
	struct sent_frame sent = {0};
	dwifi_model_reset(&model);
	dwifi_model_set_send_callback(&model, capture_frame, &sent);
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8000));
	dwifi_model_write16(&model, UINT32_C(0x0480c00a), UINT16_C(40));
	dwifi_model_write16(&model, UINT32_C(0x0480c022), UINT16_C(0xffff));
	dwifi_model_write16(&model, UINT32_C(0x048000e8), UINT16_C(1));
	dwifi_model_write16(&model, UINT32_C(0x04800080), UINT16_C(0x8000));
	dwifi_model_write16(&model, UINT32_C(0x0480008c), UINT16_C(10));
	dwifi_model_write16(&model, UINT32_C(0x048000f2), UINT16_C(1));
	dwifi_model_write16(&model, UINT32_C(0x048000ea), UINT16_C(1));

	/* Occupy the air with an ordinary transmission, then let the beacon fall
	 * due while it is still busy. */
	dwifi_model_write16(&model, UINT32_C(0x0480400a), UINT16_C(24));
	dwifi_model_write16(&model, UINT32_C(0x048000ae), UINT16_C(1));
	dwifi_model_write16(&model, UINT32_C(0x048000a0), UINT16_C(0x8000));
	unsigned before = sent.calls;
	assert(model.medium_busy_until_us > model.us_timestamp);

	dwifi_model_advance_us(&model, 11000);
	assert(sent.calls > before);
}

/* The reply barrier blocks this console for milliseconds of wall time with its
 * emulated clock frozen, so its beacon timer stops while the peer — which
 * measures the interval in its own emulated time — keeps counting. The DS MAC
 * sends the beacon from W_TXSlotBeacon without the CPU, so the obligation is
 * serviced across that stall from wall time. */
static void test_beacon_obligation_survives_a_stall(void)
{
	struct dwifi_model model;
	struct sent_frame sent = {0};
	dwifi_model_reset(&model);
	dwifi_model_set_send_callback(&model, capture_frame, &sent);
	dwifi_model_write16(&model, UINT32_C(0x0480c00a), UINT16_C(40));
	dwifi_model_write16(&model, UINT32_C(0x0480c022), UINT16_C(0xffff));
	/* A recognisable byte in the staged beacon body, to prove the frame that
	 * goes out is the guest's packet RAM rather than anything synthesised. */
	dwifi_model_write16(&model, UINT32_C(0x0480c014), UINT16_C(0xbeef));
	dwifi_model_write16(&model, UINT32_C(0x048000e8), UINT16_C(1));
	dwifi_model_write16(&model, UINT32_C(0x04800080), UINT16_C(0x8000));
	dwifi_model_write16(&model, UINT32_C(0x0480008c), UINT16_C(10));
	dwifi_model_write16(&model, UINT32_C(0x048000f2), UINT16_C(1));
	dwifi_model_write16(&model, UINT32_C(0x048000ea), UINT16_C(1));

	/* No emulated time passes at all — this is the stall. */
	uint64_t frozen = model.us_timestamp;
	dwifi_model_advance_beacon_wall_us(&model, 11000);
	assert(model.us_timestamp == frozen);
	assert(model.beacon_wall_raises == 1);
	assert(sent.calls == 1 && sent.length == 52);

	/* And it is the guest's staged beacon that went out, not a fabrication:
	 * the frame body is the packet RAM the guest wrote. */
	assert(sent.bytes[12 + 8] == UINT8_C(0xef) &&
		sent.bytes[12 + 9] == UINT8_C(0xbe));
}


/* The guest was told the transceiver is permanently in state 0x0200 — powered
 * down — no matter what the power state machine did, and that W_ModeReset reads
 * back zero after it writes one. Both are constants predating power management.
 * With radio ownership on they must reflect the model. */
static void test_radio_ownership_retires_the_stub_constants(void)
{
	struct dwifi_model model;
	dwifi_model_reset(&model);

	dwifi_model_set_radio_ownership(0);
	assert(dwifi_model_read16(&model, UINT32_C(0x0480003c)) ==
		UINT16_C(0x0200));
	assert(!dwifi_model_owns_read(UINT32_C(0x04800214)));

	dwifi_model_set_radio_ownership(1);
	/* Now served from the model, which reset leaves powered up and idle. */
	assert(dwifi_model_read16(&model, UINT32_C(0x0480003c)) !=
		UINT16_C(0x0200));
	assert(dwifi_model_owns_read(UINT32_C(0x04800214)));
	assert(dwifi_model_owns_read(UINT32_C(0x0480019c)));

	/* W_ModeReset reads back what the guest wrote, rather than zero. */
	dwifi_model_write16(&model, UINT32_C(0x04800004), UINT16_C(1));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800004)) == UINT16_C(1));

	dwifi_model_set_radio_ownership(0);
}

/* Retiring a reply slot marks its buffer as sent, and that mark has to reach
 * the guest. model->packet_ram is a shadow, so a store16 alone leaves the
 * station unable to observe that the payload it staged ever went out — which is
 * the state a guest inspects before staging the next one. */
static void test_retired_reply_is_marked_sent_for_the_guest(void)
{
	struct dwifi_model model;
	struct sent_frame sent = {0};
	struct write_log log = {0};
	uint8_t frame[12 + 30] = {0};
	dwifi_model_reset(&model);
	dwifi_model_set_send_callback(&model, capture_frame, &sent);
	dwifi_model_set_packet_write_callback(&model, log_packet_write, &log);
	dwifi_model_write16(&model, UINT32_C(0x04800050), UINT16_C(0x4000));
	dwifi_model_write16(&model, UINT32_C(0x04800052), UINT16_C(0x4800));
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8001));

	frame[10] = 30;
	frame[12] = 0x28;
	frame[13] = 0x02;
	frame[12 + 24] = 0xff;
	frame[12 + 25] = 0x7f;
	static const uint8_t cmd_flow[6] = {3, 9, 0xbf, 0, 0, 0};
	memcpy(frame + 12 + 4, cmd_flow, sizeof(cmd_flow));

	/* First generation: stage 0x8640 (packet offset 0xc80) and let a command
	 * retire it into Reply2 and transmit it. */
	dwifi_model_write16(&model, UINT32_C(0x04804c8a), UINT16_C(26));
	dwifi_model_write16(&model, UINT32_C(0x04800094), UINT16_C(0x8640));
	assert(receive_frame_complete(&model, frame, sizeof(frame)) == 1);
	run_scheduled_reply(&model);
	assert(sent.calls == 1);
	assert(!write_log_has(&log, UINT16_C(0x0c80), UINT16_C(0x0001)));

	/* Second generation: staging again is what retires the first buffer, and
	 * the guest must see 0x0001 land at its offset 0. */
	dwifi_model_write16(&model, UINT32_C(0x04804d0a), UINT16_C(26));
	dwifi_model_write16(&model, UINT32_C(0x04800094), UINT16_C(0x8680));
	assert(receive_frame_complete(&model, frame, sizeof(frame)) == 1);
	run_scheduled_reply(&model);
	assert(sent.calls == 2);
	assert(write_log_has(&log, UINT16_C(0x0c80), UINT16_C(0x0001)));
}


/* The point of deferring the reply: a guest which stages its slot from the
 * command's own receive interrupt still gets its payload on the air. Under the
 * previous inline behaviour this frame was impossible to send. */
static void test_reply_staged_after_command_is_sent(void)
{
	struct dwifi_model model;
	struct sent_frame sent = {0};
	uint8_t frame[12 + 30] = {0};
	dwifi_model_reset(&model);
	dwifi_model_set_send_callback(&model, capture_frame, &sent);
	dwifi_model_write16(&model, UINT32_C(0x04800050), UINT16_C(0x4000));
	dwifi_model_write16(&model, UINT32_C(0x04800052), UINT16_C(0x4800));
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8001));

	frame[10] = 30;
	frame[12] = 0x28;
	frame[13] = 0x02;
	frame[12 + 24] = 0xff;
	frame[12 + 25] = 0x7f;
	static const uint8_t cmd_flow[6] = {3, 9, 0xbf, 0, 0, 0};
	memcpy(frame + 12 + 4, cmd_flow, sizeof(cmd_flow));
	assert(receive_frame_complete(&model, frame, sizeof(frame)) == 1);
	assert(sent.calls == 0);

	/* Stand in for the guest's interrupt handler: stage a 26-byte reply at
	 * packet offset 0xc80 only now, after the command was received. */
	dwifi_model_write16(&model, UINT32_C(0x04804c8a), UINT16_C(26));
	dwifi_model_write16(&model, UINT32_C(0x04800094), UINT16_C(0x8640));

	run_scheduled_reply(&model);
	assert(sent.calls == 1 && sent.length == 12 + 26);
	static const uint8_t reply_flow[6] = {3, 9, 0xbf, 0, 0, 0x10};
	assert(memcmp(sent.bytes + 12 + 16, reply_flow, sizeof(reply_flow)) == 0);
}


/* melonDS ignores a CMD frame whose sequence number repeats: the guest sees no
 * header and no RX-complete, but the station still owes the host a reply. */
static void test_duplicate_command_is_not_delivered(void)
{
	struct dwifi_model model;
	struct sent_frame sent = {0};
	uint8_t frame[12 + 30] = {0};
	dwifi_model_reset(&model);
	dwifi_model_set_send_callback(&model, capture_frame, &sent);
	dwifi_model_write16(&model, UINT32_C(0x04800050), UINT16_C(0x4000));
	dwifi_model_write16(&model, UINT32_C(0x04800052), UINT16_C(0x4800));
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8001));

	frame[10] = 30;
	frame[12] = 0x28;
	frame[13] = 0x02;
	frame[12 + 24] = 0xff;
	frame[12 + 25] = 0x7f;
	/* Sequence control at +22 is what the duplicate check compares. */
	frame[12 + 22] = 0x40;
	frame[12 + 23] = 0x01;
	static const uint8_t cmd_flow[6] = {3, 9, 0xbf, 0, 0, 0};
	memcpy(frame + 12 + 4, cmd_flow, sizeof(cmd_flow));

	assert(receive_frame_complete(&model, frame, sizeof(frame)) == 1);
	uint16_t cursor = dwifi_model_read16(&model, UINT32_C(0x04800054));
	run_scheduled_reply(&model);
	assert(sent.calls == 1);

	/* The very same frame again: still answered, but not delivered. */
	sent.calls = 0;
	dwifi_model_write16(&model, UINT32_C(0x04800010), UINT16_C(0xffff));
	assert(receive_frame_complete(&model, frame, sizeof(frame)) == 1);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800054)) == cursor);
	assert(!(dwifi_model_read16(&model, UINT32_C(0x04800010)) & UINT16_C(1)));
	run_scheduled_reply(&model);
	assert(sent.calls == 1);

	/* A new sequence number is delivered normally again. */
	frame[12 + 22] = 0x50;
	assert(receive_frame_complete(&model, frame, sizeof(frame)) == 1);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800054)) != cursor);
}


/* Transceiver power management. The guest polls the radio state and will not
 * proceed while it reads as nothing, so the state has to move in response to
 * the power registers and settle after the hardware's power-up delay. */
static void test_transceiver_power_management(void)
{
	struct dwifi_model model;
	dwifi_model_reset(&model);

	/* Mode 3 hands power control to W_PowerState. */
	dwifi_model_write16(&model, UINT32_C(0x04800006), UINT16_C(0x0003));
	/* W_ModeReset bit 0 must be set or the transceiver is forced off. */
	dwifi_model_write16(&model, UINT32_C(0x04800004), UINT16_C(0x0001));

	/* Ask for power down: the transceiver reports powered down. */
	dwifi_model_write16(&model, UINT32_C(0x0480003c), UINT16_C(0x0001));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800214)) == 9);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800034)) == 0);
	/* W_PowerState itself still reads back a stub constant to the guest, so the
	 * transceiver's own registers are what tell the truth here. */

	/* Ask for power up. The radio is not usable instantly: hardware takes about
	 * 2 ms, and the guest is told with IRQ11. */
	dwifi_model_write16(&model, UINT32_C(0x04800012), UINT16_C(0xffff));
	dwifi_model_write16(&model, UINT32_C(0x0480003c), UINT16_C(0x0002));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800010)) &
		UINT16_C(1 << 11));
	assert(model.us_until_power_on < 0);

	/* Still settling part way through. */
	dwifi_model_advance_us(&model, 1024);
	assert(model.us_until_power_on < 0);

	/* Once the delay elapses the transceiver is on and idle. */
	dwifi_model_advance_us(&model, 1200);
	assert(model.us_until_power_on == 0);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800214)) == 1);
	assert(dwifi_model_read16(&model, UINT32_C(0x0480019c)) == UINT16_C(0x84));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800034)) == 1);

	/* W_PowerForce overrides everything. */
	dwifi_model_write16(&model, UINT32_C(0x04800040), UINT16_C(0x8001));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800214)) == 9);
	dwifi_model_write16(&model, UINT32_C(0x04800040), UINT16_C(0x8000));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800034)) == 1);

	/* Clearing W_ModeReset bit 0 forces the transceiver off regardless. */
	dwifi_model_write16(&model, UINT32_C(0x04800040), 0);
	dwifi_model_write16(&model, UINT32_C(0x04800004), 0);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800214)) == 9);
}


/* Pre-beacon wakeup and post-beacon sleep. Auto wakeup applies in every power
 * management mode; auto power-down only in mode 0, because a station in mode 2
 * does not service these interrupts and would never wake again. */
static void test_beacon_power_interrupts(void)
{
	struct dwifi_model model;
	dwifi_model_reset(&model);
	dwifi_model_write16(&model, UINT32_C(0x04800006), UINT16_C(0x0003));
	dwifi_model_write16(&model, UINT32_C(0x04800004), UINT16_C(0x0001));

	/* Power down, then let the pre-beacon comparison come round. */
	dwifi_model_write16(&model, UINT32_C(0x0480003c), UINT16_C(0x0001));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800214)) == 9);

	/* W_PowerTX bit 0 enables auto wakeup. */
	dwifi_model_write16(&model, UINT32_C(0x04800038), UINT16_C(0x0001));
	dwifi_model_write16(&model, UINT32_C(0x04800110), 0);
	dwifi_model_write16(&model, UINT32_C(0x048000e8), UINT16_C(1));
	dwifi_model_write16(&model, UINT32_C(0x048000ea), UINT16_C(1));
	dwifi_model_advance_us(&model, 2048);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800010)) &
		UINT16_C(1 << 15));
	/* The wakeup was requested, so the power-up delay is now running. */
	assert(model.us_until_power_on < 0 ||
		dwifi_model_read16(&model, UINT32_C(0x04800034)) == 1);

	/* In mode 2 the post-beacon interrupt must not power the radio down: a
	 * station there does not service IRQ13/IRQ15 and would never wake. */
	struct dwifi_model quiet;
	dwifi_model_reset(&quiet);
	dwifi_model_write16(&quiet, UINT32_C(0x04800006), UINT16_C(0x0002));
	dwifi_model_write16(&quiet, UINT32_C(0x04800004), UINT16_C(0x0001));
	dwifi_model_write16(&quiet, UINT32_C(0x04800134), UINT16_C(1));
	dwifi_model_advance_us(&quiet, 1000);
	assert(dwifi_model_read16(&quiet, UINT32_C(0x04800010)) &
		UINT16_C(1 << 13));
	assert(dwifi_model_read16(&quiet, UINT32_C(0x04800214)) != 9);

	/* Mode 0 is the one that does power down on the same interrupt. */
	struct dwifi_model sleepy;
	dwifi_model_reset(&sleepy);
	dwifi_model_write16(&sleepy, UINT32_C(0x04800006), 0);
	dwifi_model_write16(&sleepy, UINT32_C(0x04800004), UINT16_C(0x0001));
	dwifi_model_write16(&sleepy, UINT32_C(0x04800134), UINT16_C(1));
	dwifi_model_advance_us(&sleepy, 1000);
	assert(dwifi_model_read16(&sleepy, UINT32_C(0x04800214)) == 9);
}

/* A station adopts the host's clock from a beacon on its own network, so that
 * what it schedules against the host's beacon lands when the host is
 * listening. */
static void test_beacon_synchronises_the_counter(void)
{
	struct dwifi_model model;
	uint8_t frame[12 + 40] = {0};
	dwifi_model_reset(&model);
	dwifi_model_write16(&model, UINT32_C(0x04800050), UINT16_C(0x4000));
	dwifi_model_write16(&model, UINT32_C(0x04800052), UINT16_C(0x4800));
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8001));
	dwifi_model_write16(&model, UINT32_C(0x04800020), UINT16_C(0x2211));
	dwifi_model_write16(&model, UINT32_C(0x04800022), UINT16_C(0x4433));
	dwifi_model_write16(&model, UINT32_C(0x04800024), UINT16_C(0x6655));

	frame[8] = 0x14;
	frame[10] = 40;
	frame[12] = 0x80;
	static const uint8_t bssid[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	memcpy(frame + 12 + 16, bssid, sizeof(bssid));
	frame[12 + 24] = 0x00;
	frame[12 + 25] = 0x10;

	assert(receive_frame_complete(&model, frame, sizeof(frame)) == 1);
	/* 40 bytes at the 2 Mbit rate is 160 us of air, less the fixed 76. */
	assert(model.us_counter == UINT64_C(0x1000) + (40 * 4 - 76));

	/* A beacon from another network leaves the clock alone. */
	uint64_t kept = model.us_counter;
	frame[12 + 16] = 0x99;
	assert(receive_frame_complete(&model, frame, sizeof(frame)) == 1);
	assert(model.us_counter == kept);
}

static void test_rx_ring_and_fifo(void)
{
	struct dwifi_model model;
	uint8_t frame[17] = {0};
	dwifi_model_reset(&model);
	frame[8] = 0x14;
	frame[10] = 5;
	for (unsigned i = 0; i < 5; ++i) frame[12 + i] = (uint8_t)(0xa0 + i);
	dwifi_model_write16(&model, UINT32_C(0x04800050), UINT16_C(0x4000));
	dwifi_model_write16(&model, UINT32_C(0x04800052), UINT16_C(0x4020));
	dwifi_model_write16(&model, UINT32_C(0x04800056), 0);
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8001));
	assert(receive_frame_complete(&model, frame, sizeof(frame)) == 1);
	assert(dwifi_model_read16(&model, UINT32_C(0x04804000)) == UINT16_C(0x0010));
	assert(dwifi_model_read16(&model, UINT32_C(0x04804006)) == UINT16_C(0x0014));
	assert(dwifi_model_read16(&model, UINT32_C(0x04804008)) == 8);
	assert(dwifi_model_read16(&model, UINT32_C(0x0480400c)) == UINT16_C(0xa1a0));
	assert((dwifi_model_read16(&model, UINT32_C(0x04800010)) &
		UINT16_C(0x0041)) == UINT16_C(0x0041));
	dwifi_model_write16(&model, UINT32_C(0x04800058), UINT16_C(0x000c));
	dwifi_model_write16(&model, UINT32_C(0x0480005c), 1);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800060)) == UINT16_C(0xa1a0));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800010)) & UINT16_C(1 << 9));
}

static void test_rx_start_precedes_completion(void)
{
	struct dwifi_model model;
	uint8_t frame[12 + 8] = {0};
	dwifi_model_reset(&model);
	frame[8] = 0x14;
	frame[10] = 8;
	frame[12] = 0x80;
	dwifi_model_write16(&model, UINT32_C(0x04800050), UINT16_C(0x4000));
	dwifi_model_write16(&model, UINT32_C(0x04800052), UINT16_C(0x4800));
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8001));

	assert(dwifi_model_receive_frame(&model, frame, sizeof(frame)) == 1);
	assert(model.rx_active);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800010)) ==
		UINT16_C(1 << 6));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800054)) == 0);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800268)) == 6);
	assert(dwifi_model_read16(&model, UINT32_C(0x04804000)) == 0);
	dwifi_model_advance_us(&model, 8);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800268)) == 7);
	assert(dwifi_model_read16(&model, UINT32_C(0x0480400c)) == 0x80);

	dwifi_model_advance_us(&model, 16);
	assert(model.rx_active);
	assert(!(dwifi_model_read16(&model, UINT32_C(0x04800010)) & 1));
	dwifi_model_advance_us(&model, 8);
	assert(!model.rx_active);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800010)) & 1);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800054)) != 0);
}

static void test_beacon_rx_classification(void)
{
	struct dwifi_model model;
	struct packet_write write = {0};
	uint8_t frame[12 + 24] = {0};
	dwifi_model_reset(&model);
	dwifi_model_set_packet_write_callback(&model, capture_packet_write, &write);
	frame[10] = 24;
	frame[12] = 0x80;
	dwifi_model_write16(&model, UINT32_C(0x04800050), UINT16_C(0x4000));
	dwifi_model_write16(&model, UINT32_C(0x04800052), UINT16_C(0x4800));
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8001));
	assert(receive_frame_complete(&model, frame, sizeof(frame)) == 1);
	assert((dwifi_model_read16(&model, UINT32_C(0x04804000)) & UINT16_C(0x7fff)) ==
		UINT16_C(0x0011));
	assert(write.calls == 18);
	assert(write.offset == UINT16_C(0x000a));
}

static void test_rx_length_crop(void)
{
	struct dwifi_model model;
	uint8_t frame[12 + 9] = {0};
	dwifi_model_reset(&model);
	frame[8] = 0x14;
	frame[10] = 9;
	for (unsigned index = 0; index < 9; ++index)
		frame[12 + index] = (uint8_t)(0x20 + index);
	dwifi_model_write16(&model, UINT32_C(0x04800050), UINT16_C(0x4000));
	dwifi_model_write16(&model, UINT32_C(0x04800052), UINT16_C(0x4020));
	dwifi_model_write16(&model, UINT32_C(0x04800056), 0);
	dwifi_model_write16(&model, UINT32_C(0x048000da), UINT16_C(0x0602));
	dwifi_model_write16(&model, UINT32_C(0x04800030), UINT16_C(0x8001));
	assert(receive_frame_complete(&model, frame, sizeof(frame)) == 1);
	assert(dwifi_model_read16(&model, UINT32_C(0x04804008)) == 8);
	assert(dwifi_model_read16(&model, UINT32_C(0x04804010)) == UINT16_C(0x0024));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800054)) == 10);
}

static void test_baseband_port(void)
{
	struct dwifi_model model;
	dwifi_model_reset(&model);
	dwifi_model_write16(&model, UINT32_C(0x0480015a), UINT16_C(0x00a5));
	dwifi_model_write16(&model, UINT32_C(0x04800158), UINT16_C(0x5012));

	/* Writes use operation 1. Reads only expose the selected byte for operation
	 * 2 with the start bit set, matching DraStic's existing placeholder. */
	assert(dwifi_model_read16(&model, UINT32_C(0x0480015c)) == 0);
	dwifi_model_write16(&model, UINT32_C(0x04800158), UINT16_C(0x6012));
	assert(dwifi_model_read16(&model, UINT32_C(0x0480015c)) == UINT16_C(0x00a5));
}

static void test_interrupt_registers(void)
{
	struct dwifi_model model;
	unsigned edges = 0;
	dwifi_model_reset(&model);
	dwifi_model_set_irq_callback(&model, count_irq, &edges);

	/* A masked source remains latched but does not raise ARM7 IRQ24. Enabling
	 * it later creates exactly one rising edge. */
	dwifi_model_raise_irq(&model, 3);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800010)) == UINT16_C(0x0008));
	assert(!dwifi_model_irq_pending(&model));
	assert(edges == 0);
	dwifi_model_write16(&model, UINT32_C(0x04800012), UINT16_C(0x0008));
	assert(dwifi_model_irq_pending(&model));
	assert(edges == 1);

	/* More enabled flags do not retrigger while the aggregate line is high. */
	dwifi_model_raise_irq(&model, 4);
	assert(edges == 1);

	/* W_IF is write-one-to-clear. Clearing the final enabled source lowers the
	 * modeled line, after which a new source can produce another edge. */
	dwifi_model_write16(&model, UINT32_C(0x04800010), UINT16_C(0x0018));
	assert(!dwifi_model_irq_pending(&model));
	dwifi_model_write16(&model, UINT32_C(0x0480021c), UINT16_C(0x0008));
	assert(dwifi_model_irq_pending(&model));
	assert(edges == 2);

	/* Hardware bit 10 cannot be forced through W_IFSet. */
	dwifi_model_write16(&model, UINT32_C(0x04800010), UINT16_C(0xffff));
	dwifi_model_write16(&model, UINT32_C(0x0480021c), UINT16_C(0x0400));
	assert(dwifi_model_read16(&model, UINT32_C(0x04800010)) == 0);
}

static void test_emulated_microsecond_counter(void)
{
	struct dwifi_model model;
	dwifi_model_reset(&model);

	/* Counter writes are visible immediately, but elapsed time is ignored until
	 * enabled and accumulated to the hardware's 8 us timer boundary. */
	dwifi_model_write16(&model, UINT32_C(0x048000f8), UINT16_C(0x1234));
	dwifi_model_write16(&model, UINT32_C(0x048000fa), UINT16_C(0xabcd));
	assert(dwifi_model_read16(&model, UINT32_C(0x048000f8)) == UINT16_C(0x1234));
	assert(dwifi_model_read16(&model, UINT32_C(0x048000fa)) == UINT16_C(0xabcd));
	dwifi_model_advance_us(&model, 16);
	assert(model.us_counter == UINT64_C(0xabcd1234));

	dwifi_model_write16(&model, UINT32_C(0x048000e8), UINT16_C(0xffff));
	assert(dwifi_model_read16(&model, UINT32_C(0x048000e8)) == 1);
	dwifi_model_advance_us(&model, 7);
	assert(model.us_counter == UINT64_C(0xabcd1234));
	dwifi_model_advance_us(&model, 1);
	assert(model.us_counter == UINT64_C(0xabcd123c));
	assert(model.us_timestamp == 24);
}

static void test_microsecond_compare_irq(void)
{
	struct dwifi_model model;
	unsigned edges = 0;
	dwifi_model_reset(&model);
	dwifi_model_set_irq_callback(&model, count_irq, &edges);
	dwifi_model_write16(&model, UINT32_C(0x04800012), UINT16_C(1 << 14));
	/* Park W_PreBeacon out of reach. With the beacon counter at zero the
	 * pre-beacon comparison would otherwise match once per 1024 us and set
	 * IRQ15 alongside the compare interrupt under test. */
	dwifi_model_write16(&model, UINT32_C(0x04800110), UINT16_C(0x0800));
	/* Preserve the counter's sub-tick low bits: the millisecond boundary is the
	 * first 8 us window, rather than only the exact value ending in zero. */
	dwifi_model_write16(&model, UINT32_C(0x048000f8), UINT16_C(4));
	dwifi_model_write16(&model, UINT32_C(0x048000f0), UINT16_C(0x041f));
	assert(dwifi_model_read16(&model, UINT32_C(0x048000f0)) == UINT16_C(0x0400));
	dwifi_model_write16(&model, UINT32_C(0x048000ea), UINT16_C(1));
	dwifi_model_write16(&model, UINT32_C(0x048000e8), UINT16_C(1));
	dwifi_model_advance_us(&model, 1024);
	assert(model.us_counter == 1028);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800010)) ==
		UINT16_C(1 << 14));
	assert(edges == 1);

	/* Control bit 1 force-sets the same sub-interrupt and is self-clearing. */
	dwifi_model_write16(&model, UINT32_C(0x04800010), UINT16_C(1 << 14));
	dwifi_model_write16(&model, UINT32_C(0x048000ea), UINT16_C(2));
	assert(dwifi_model_read16(&model, UINT32_C(0x048000ea)) == 0);
	assert(edges == 2);
}

int main(void)
{
	test_reset_and_fixed_registers();
	test_register_storage_and_mirroring();
	test_packet_ram_mirroring();
	test_tx_path();
	test_tx_request_before_slot();
	test_tx_request_bit_does_not_select_slot();
	test_automatic_beacon();
	test_beacon_requires_enabled_interrupt();
	test_post_beacon_window_extends_for_association();
	test_post_beacon_window_waits_for_the_link();
	test_microsecond_counter_is_model_owned();
	test_tx_frames_carry_sequence_numbers();
	test_rf_status_and_tx_header_writeback();
	test_mp_command_transfer();
	test_mp_reply_is_automatic();
	test_populated_mp_reply_uses_ack_flow();
	test_mp_reply_slot_latches();
	test_radio_ownership_retires_the_stub_constants();
	test_beacon_obligation_survives_a_stall();
	test_medium_serialises_transmission();
	test_beacon_is_not_blocked_by_a_busy_medium();
	test_retired_reply_is_marked_sent_for_the_guest();
	test_reply_staged_after_command_is_sent();
	test_duplicate_command_is_not_delivered();
	test_transceiver_power_management();
	test_beacon_power_interrupts();
	test_beacon_synchronises_the_counter();
	test_rx_ring_and_fifo();
	test_rx_start_precedes_completion();
	test_beacon_rx_classification();
	test_rx_length_crop();
	test_baseband_port();
	test_interrupt_registers();
	test_emulated_microsecond_counter();
	test_microsecond_compare_irq();
	puts("drastic-wifi model tests passed");
	return 0;
}
