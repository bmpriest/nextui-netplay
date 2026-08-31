#ifndef DRASTIC_WIFI_MODEL_H
#define DRASTIC_WIFI_MODEL_H

#include <stdint.h>
#include <stddef.h>

#define DWIFI_PACKET_RAM_SIZE UINT32_C(0x2000)
#define DWIFI_REGISTER_SIZE   UINT32_C(0x1000)
#define DWIFI_BASEBAND_SIZE   UINT32_C(0x0100)

struct dwifi_model {
	uint8_t packet_ram[DWIFI_PACKET_RAM_SIZE];
	uint8_t registers[DWIFI_REGISTER_SIZE];
	uint8_t baseband[DWIFI_BASEBAND_SIZE];
	uint64_t us_timestamp;
	uint64_t us_counter;
	uint64_t us_compare;
	uint16_t ms_remainder_us;
	uint64_t read_count;
	uint64_t write_count;
	uint64_t tx_frame_count;
	uint64_t rx_frame_count;
	void (*raise_irq)(void *opaque);
	void *irq_opaque;
	void (*send_frame)(void *opaque, const uint8_t *frame, size_t length);
	void *send_opaque;
	void (*write_packet)(void *opaque, uint16_t offset, uint16_t value);
	void *packet_opaque;
	void (*trace_receive)(void *opaque, const char *stage,
		const uint8_t *frame, size_t length, uint16_t address);
	void *trace_opaque;
	/* Called at the points where the protocol waits on the peer: a command
	 * transfer's reply window, and the tail of a host's post-beacon listening
	 * window. Emulated time advances in bursts far shorter than the link's real
	 * latency, so a transport that cannot deliver inside such a window can hold
	 * the guest there instead. */
	void (*wait_for_peer)(void *opaque);
	void *peer_wait_opaque;
	uint8_t irq_asserted;
	uint8_t timer_remainder_us;
	/* One per transmit slot, in the order fire_tx scans them. Set when the
	 * guest enables a slot, cleared when that frame goes out. */
	uint8_t tx_slot_armed[4];
	/* W_US_COMPARE bit 0 is a write-only control, not part of the compare
	 * value: on hardware it blocks the W_BEACONCOUNT interrupt until the
	 * microsecond counter reaches the compare. Tracked here so the bit is not
	 * silently discarded; nothing consumes it until W_BEACONCOUNT is modelled,
	 * because gating the model's synthesised beacon interrupt on it broke
	 * discovery. */
	uint8_t beacon_irq_blocked;
	/* The radio exposes RX-start before the MAC has transferred the complete
	 * frame.  Keep one transport frame in flight until its emulated airtime has
	 * elapsed; local-multiplayer traffic is serialized, as it is on the medium. */
	uint8_t rx_active;
	uint8_t rx_completing;
	size_t rx_pending_length;
	uint64_t rx_complete_at_us;
	uint64_t rx_next_halfword_at_us;
	uint16_t rx_pending_frame_length;
	uint16_t rx_pending_stored_length;
	uint16_t rx_pending_ptr;
	uint8_t rx_halfword_us;
	uint8_t rx_pending[DWIFI_PACKET_RAM_SIZE];
	/* An in-flight multiplayer command transfer: who is being polled, who has
	 * not answered yet, where the command frame sits, and when the reply window
	 * closes. */
	uint8_t mp_cmd_active;
	uint8_t mp_ack_active;
	uint8_t mp_reply_active;
	uint16_t mp_client_mask;
	uint16_t mp_client_fail;
	uint16_t mp_cmd_address;
	uint16_t mp_cmd_slot;
	uint16_t mp_cmd_busy_bit;
	uint8_t mp_cmd_rate;
	uint64_t mp_reply_deadline_us;
	uint64_t mp_ack_deadline_us;
	uint64_t mp_reply_tx_deadline_us;
	uint64_t mp_last_reply_us;
	uint8_t mp_reply_seen;
};

void dwifi_model_reset(struct dwifi_model *model);
void dwifi_model_set_irq_callback(struct dwifi_model *model,
	void (*raise_irq)(void *opaque), void *opaque);
void dwifi_model_set_send_callback(struct dwifi_model *model,
	void (*send_frame)(void *opaque, const uint8_t *frame, size_t length),
	void *opaque);
void dwifi_model_set_packet_write_callback(struct dwifi_model *model,
	void (*write_packet)(void *opaque, uint16_t offset, uint16_t value),
	void *opaque);
void dwifi_model_set_peer_wait_callback(struct dwifi_model *model,
	void (*wait_for_peer)(void *opaque), void *opaque);
void dwifi_model_set_receive_trace_callback(struct dwifi_model *model,
	void (*trace_receive)(void *opaque, const char *stage,
		const uint8_t *frame, size_t length, uint16_t address),
	void *opaque);
/* Inject a DS hardware-format frame: 12-byte radio header followed by the
 * 802.11 frame. Returns zero when RX is disabled or the frame is malformed. */
int dwifi_model_receive_frame(struct dwifi_model *model,
	const uint8_t *frame, size_t length);
/* True for MMIO values whose side effects/state must come from this model once
 * packet transport is active. Other registers remain DraStic-authoritative. */
int dwifi_model_owns_read(uint32_t address);
int dwifi_model_irq_pending(const struct dwifi_model *model);
void dwifi_model_raise_irq(struct dwifi_model *model, unsigned interrupt);
/* Advance deterministic Wi-Fi time supplied by an emulator-cycle adapter.
 * Hardware-visible work occurs on the DS Wi-Fi timer's 8 us boundaries. */
void dwifi_model_advance_us(struct dwifi_model *model, uint64_t elapsed_us);
uint16_t dwifi_model_read16(struct dwifi_model *model, uint32_t address);
void dwifi_model_write16(struct dwifi_model *model, uint32_t address,
	uint16_t value);

#endif
