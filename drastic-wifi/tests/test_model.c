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
	/* Not served to the guest yet: see set_rf_status. */
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

	assert(sent.calls == 1 && sent.length == 12 + 26);
	assert(sent.bytes[4] == 1);
	static const uint8_t reply_flow[6] = {3, 9, 0xbf, 0, 0, 0x10};
	assert(memcmp(sent.bytes + 12 + 16, reply_flow, sizeof(reply_flow)) == 0);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800094)) == 0);
	assert(dwifi_model_read16(&model, UINT32_C(0x04800098)) ==
		UINT16_C(0x8640));
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
