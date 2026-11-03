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
	/* Cost accounting for the advance loop, which scoped timing showed consuming
	 * 12.9 s of the host's multiplayer phase against DraStic's own 16.5 s. */
	uint64_t tick_count;
	uint64_t rx_halfword_writes;
	uint64_t pre_beacon_raises;
	uint64_t advance_calls;
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
	/* When the current command transfer began, in this console's own clock. A
	 * reply received after it answers it, which lets the reply barrier stop
	 * waiting for something that has already arrived. */
	uint64_t mp_cmd_started_us;
	uint8_t mp_reply_seen;
	/* A received command schedules this station's reply instead of transmitting
	 * it inline. The guest ARM7 only runs between calls into the model, so a
	 * reply staged in response to the command's interrupt cannot exist until at
	 * least one such boundary has passed. */
	/* Sequence number of the last multiplayer command accepted from the host.
	 * Hardware ignores a command whose sequence number repeats. */
	/* Negative while the transceiver is powering up, counting toward zero in
	 * microseconds. The radio is not usable until it reaches zero. */
	int32_t us_until_power_on;
	uint16_t mp_last_seqno;
	uint8_t mp_last_seqno_valid;
	uint8_t mp_reply_due;
	uint16_t mp_reply_client_time;
	uint64_t mp_reply_due_us;
	/* Diagnostic. The model normally answers every poll with a synthesised empty
	 * reply when the guest has staged nothing, which keeps the host's fail mask
	 * clear and hides the fact that the client's software never replied. Set to
	 * suppress that substitution so the host observes a genuine non-answer, as
	 * real hardware would report. */
	uint8_t suppress_default_reply;
	/* Medium contention. Real hardware serialises the air: a station physically
	 * cannot begin a frame while one is still being transmitted, so a host can
	 * never out-produce what a station's receiver is able to accept. This model
	 * had that constraint on the receiving side only — rx_active holds for each
	 * frame's airtime — while the transmit path emitted in zero emulated time,
	 * so the host over-drove a medium the client was obliged to honour. The air
	 * is occupied until this timestamp. */
	uint64_t medium_busy_until_us;
	/* Beacon obligation serviced while this console is stalled outside emulated
	 * time, and how many beacons that produced. */
	uint64_t beacon_wall_remainder_us;
	uint64_t beacon_wall_raises;
	/* Reply-path accounting. The station answers every command the host sends,
	 * but it can only answer with real data if its guest was given a chance to
	 * stage one — which means an RX-complete interrupt it actually saw. A
	 * command suppressed as a duplicate still owes a reply and still consumes
	 * the staging slot, without the guest ever learning the command existed. */
	uint64_t mp_cmd_delivered;
	uint64_t mp_cmd_duplicate;
	uint64_t mp_replies_sent;
	uint64_t mp_replies_populated;
	/* fire_tx() runs only from the guest's register writes, so a transmission
	 * refused because the medium was busy would simply be lost. Set when one is
	 * deferred, and retried from dwifi_model_advance_us(). */
	uint8_t tx_deferred;
};

/* Advance only the beacon obligation, by wall-clock microseconds, while this
 * console is stalled somewhere that emulated time cannot pass — specifically the
 * transport's reply barrier, which blocks for milliseconds of wall time with the
 * emulated clock frozen.
 *
 * The DS MAC transmits the beacon from W_TXSlotBeacon autonomously at each
 * target beacon time; a CPU that is not running does not stop it. The frame is
 * already staged in packet RAM by the guest, so no content is invented here.
 * The peer measures the beacon interval in its own emulated time, which tracks
 * wall time, so a stall that freezes this console's beacon timer is a real gap
 * on the air to the station that has to hear it. */
void dwifi_model_advance_beacon_wall_us(struct dwifi_model *model,
	uint64_t elapsed_us);

/* Serve W_RXTXAddr and W_TXSeqNo from the model rather than letting them fall
 * through to DraStic's stub. See the comment at dwifi_model_owns_read(). */
void dwifi_model_set_datapath_ownership(int enabled);

/* Serve W_RFStatus, W_RFPins, W_ModeReset and W_PowerState from the model,
 * retiring the two hardcoded stub constants. See dwifi_model_owns_read(). */
void dwifi_model_set_radio_ownership(int enabled);

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
