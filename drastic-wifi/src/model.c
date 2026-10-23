#include "drastic_wifi_model.h"

#include <string.h>

/* Nintendo DS Wi-Fi MMIO occupies the 0x04800000 window. The 8 KiB packet RAM
 * at +0x4000 is mirrored once through +0x7fff; registers mirror every 0x1000.
 * Keeping those rules here, rather than exposing DraStic's memory_struct, is
 * what lets the hardware model stay identical across the ARMv7 and AArch64
 * executables. */
#define PACKET_WINDOW_START UINT32_C(0x4000)
#define PACKET_WINDOW_END   UINT32_C(0x6000)

/* Behavior already present in DraStic's placeholder handlers. Reproducing it
 * gives the opt-in model a conservative baseline while register side effects,
 * interrupts and timing are added deliberately. */
#define REG_MODE_RST        UINT32_C(0x0004)
#define REG_MODE_WEP        UINT32_C(0x0006)
#define REG_TRX_POWER       UINT32_C(0x0034)
#define REG_POWER_US        UINT32_C(0x0036)
#define REG_POWER_TX        UINT32_C(0x0038)
#define REG_POWER_STATE     UINT32_C(0x003c)
#define REG_POWER_FORCE     UINT32_C(0x0040)
#define REG_POWER_DOWN_CTRL UINT32_C(0x0048)
#define REG_RX_CNT          UINT32_C(0x0030)
#define REG_RX_BUF_BEGIN    UINT32_C(0x0050)
#define REG_RX_BUF_END      UINT32_C(0x0052)
#define REG_RX_WRITE_CURSOR UINT32_C(0x0054)
#define REG_RX_WRITE_ADDR   UINT32_C(0x0056)
#define REG_RX_READ_ADDR    UINT32_C(0x0058)
#define REG_RX_READ_CURSOR  UINT32_C(0x005a)
#define REG_RX_BUF_COUNT    UINT32_C(0x005c)
#define REG_RX_DATA_READ    UINT32_C(0x0060)
#define REG_RX_GAP_ADDR     UINT32_C(0x0062)
#define REG_RX_GAP_SIZE     UINT32_C(0x0064)
#define REG_TX_WRITE_ADDR   UINT32_C(0x0068)
#define REG_TX_BUF_COUNT    UINT32_C(0x006c)
#define REG_TX_DATA_WRITE   UINT32_C(0x0070)
#define REG_TX_GAP_ADDR     UINT32_C(0x0074)
#define REG_TX_GAP_SIZE     UINT32_C(0x0076)
#define REG_TX_STAT_CNT     UINT32_C(0x0008)
#define REG_MAC_ADDR0       UINT32_C(0x0018)
#define REG_BSSID0          UINT32_C(0x0020)
#define REG_AID_LOW         UINT32_C(0x0028)
#define REG_TX_SLOT_REPLY1  UINT32_C(0x0094)
#define REG_TX_SLOT_REPLY2  UINT32_C(0x0098)
#define REG_CMD_REPLY_TIME  UINT32_C(0x00c4)
#define REG_CMD_COUNT       UINT32_C(0x0118)
#define REG_RX_TX_ADDR      UINT32_C(0x0268)
#define REG_LISTEN_COUNT    UINT32_C(0x0088)
#define REG_TX_SLOT_BEACON  UINT32_C(0x0080)
#define REG_LISTEN_INTERVAL UINT32_C(0x008e)
#define REG_PRE_BEACON      UINT32_C(0x0110)
#define REG_BEACON_COUNT1   UINT32_C(0x011c)
#define REG_BEACON_COUNT2   UINT32_C(0x0134)
#define REG_BEACON_INTERVAL UINT32_C(0x008c)
#define REG_TX_SLOT_CMD     UINT32_C(0x0090)
#define REG_TX_SLOT_LOC1    UINT32_C(0x00a0)
#define REG_TX_SLOT_LOC2    UINT32_C(0x00a4)
#define REG_TX_SLOT_LOC3    UINT32_C(0x00a8)
#define REG_TX_REQ_RESET    UINT32_C(0x00ac)
#define REG_TX_REQ_SET      UINT32_C(0x00ae)
#define REG_TX_REQ_READ     UINT32_C(0x00b0)
#define REG_TX_SLOT_RESET   UINT32_C(0x00b4)
#define REG_TX_BUSY         UINT32_C(0x00b6)
#define REG_TX_STAT         UINT32_C(0x00b8)
#define REG_RX_LEN_CROP     UINT32_C(0x00da)
#define REG_W_IF            UINT32_C(0x0010)
#define REG_W_IE            UINT32_C(0x0012)
#define REG_BB_CNT          UINT32_C(0x0158)
#define REG_BB_WRITE        UINT32_C(0x015a)
#define REG_BB_READ         UINT32_C(0x015c)
#define REG_BB_BUSY         UINT32_C(0x015e)
#define REG_RF_PINS         UINT32_C(0x019c)
#define REG_RF_BUSY         UINT32_C(0x0180)
#define REG_RF_STATUS       UINT32_C(0x0214)
#define REG_TX_HEADER_CNT   UINT32_C(0x0194)
#define REG_TX_SEQ_NO       UINT32_C(0x0210)
#define REG_W_IF_SET        UINT32_C(0x021c)
#define REG_US_COUNT_CNT    UINT32_C(0x00e8)
#define REG_US_COMPARE_CNT  UINT32_C(0x00ea)
#define REG_US_COMPARE0     UINT32_C(0x00f0)
#define REG_US_COMPARE3     UINT32_C(0x00f6)
#define REG_US_COUNT0       UINT32_C(0x00f8)
#define REG_US_COUNT3       UINT32_C(0x00fe)

#define WIFI_TIMER_INTERVAL_US UINT64_C(8)

/* The transceiver reports what it is doing through W_RFStatus, and mirrors it
 * on the pin register the guest actually polls. Mario Kart reads W_RFPins
 * hundreds of thousands of times per run; leaving it at zero tells the guest
 * the radio is in a state it never expects. */
enum dwifi_rf_status {
	DWIFI_RF_IDLE = 1,
	DWIFI_RF_TRANSMITTING = 3,
	DWIFI_RF_AWAITING_REPLIES = 5,
	DWIFI_RF_RECEIVING = 6,
	DWIFI_RF_REPLY_OR_ACK = 8,
	DWIFI_RF_POWERED_DOWN = 9
};

static void set_rf_status(struct dwifi_model *model, unsigned status);
static void update_power_status(struct dwifi_model *model, int power);
static void complete_tx_header(struct dwifi_model *model, uint32_t address);
static void send_mp_reply(struct dwifi_model *model, uint16_t client_time);
static void occupy_medium(struct dwifi_model *model, uint16_t frame_length,
	uint8_t rate);
static void fire_tx(struct dwifi_model *model);

/* fire_beacon runs inside DraStic's small event-thread stack. There is one
 * Wi-Fi model per emulator process, so static transmit scratch is sufficient. */
static uint8_t beacon_frame[DWIFI_PACKET_RAM_SIZE];
static uint8_t tx_frame[DWIFI_PACKET_RAM_SIZE];
static uint8_t mp_ack_frame[12 + 32];
/* akkit's register map: bit 14 is the beacon timeslot interrupt, "raised right
 * after a beacon should have been sent". The microsecond compare shares it. */
#define WIFI_IRQ_US_COMPARE    14
#define WIFI_IRQ_BEACON_SLOT   14
#define WIFI_IRQ_RX_COMPLETE   0
#define WIFI_IRQ_TX_COMPLETE   1
#define WIFI_IRQ_RX_START      6
#define WIFI_IRQ_TX_START      7
#define WIFI_IRQ_MP_END        12
#define WIFI_IRQ_POST_BEACON   13
#define WIFI_IRQ_TX_COUNT      8
#define WIFI_IRQ_RX_COUNT      9
#define WIFI_IRQ_RF_WAKEUP     11
#define WIFI_IRQ_PRE_BEACON    15

/* Emulated turnaround between a received multiplayer command and this station's
 * reply. Short enough to stay well inside the command's advertised client
 * window, and non-zero so the reply is owed to a later call into the model. */
#define DWIFI_MP_REPLY_TURNAROUND_US 16

/* How long a station stays credited on its last reply. Hardware marks a client
 * failed the moment it misses a reply window, but hardware is not waiting on a
 * TCP round trip: a reply that cannot cross the link inside an emulated window
 * of a few hundred microseconds is a transport limitation, not an absent
 * station. Roughly two beacon periods. */
#define DWIFI_MP_REPLY_GRACE_US UINT64_C(500000)

static uint32_t window_offset(uint32_t address)
{
	/* The DS Wi-Fi aperture repeats every 0x8000 bytes. DraStic's ARM7 I/O
	 * dispatcher presents the same packet RAM as 0xc000-0xdfff. */
	return address & UINT32_C(0x7ffe);
}

static uint32_t register_offset(uint32_t address)
{
	return (address & (DWIFI_REGISTER_SIZE - 1)) & ~UINT32_C(1);
}

static uint16_t load16(const uint8_t *memory, uint32_t offset)
{
	return (uint16_t)(memory[offset] | ((uint16_t)memory[offset + 1] << 8));
}
/* Transmit and receive are deliberately NOT gated on the transceiver power
 * state, though melonDS gates both. melonDS can, because its receive path and
 * its radio share one timeline. Here frames arrive whenever the socket has
 * them, and the guest forces the transceiver off and on over a hundred times a
 * run, so gating silenced the radio outright: with it in place neither console
 * emitted a single frame, not even a beacon, and Mario Kart never discovered a
 * group. The power state machine below is still modelled and driven; only its
 * effect on the data path is withheld until the two timelines are related. */

static void store16(uint8_t *memory, uint32_t offset, uint16_t value)
{
	memory[offset] = (uint8_t)value;
	memory[offset + 1] = (uint8_t)(value >> 8);
}

static void update_irq(struct dwifi_model *model)
{
	uint16_t flags = load16(model->registers, REG_W_IF);
	uint16_t enabled = load16(model->registers, REG_W_IE);
	int pending = (flags & enabled) != 0;
	if (pending && !model->irq_asserted && model->raise_irq)
		model->raise_irq(model->irq_opaque);
	model->irq_asserted = (uint8_t)pending;
}

void dwifi_model_reset(struct dwifi_model *model)
{
	if (!model)
		return;
	memset(model, 0, sizeof(*model));
	/* The model has no power management, so the transceiver is on and idle from
	 * reset rather than reporting the powered-down state. */
	set_rf_status(model, DWIFI_RF_IDLE);
}

void dwifi_model_set_irq_callback(struct dwifi_model *model,
	void (*raise_irq)(void *opaque), void *opaque)
{
	if (!model)
		return;
	model->raise_irq = raise_irq;
	model->irq_opaque = opaque;
}

void dwifi_model_set_send_callback(struct dwifi_model *model,
	void (*send_frame)(void *opaque, const uint8_t *frame, size_t length),
	void *opaque)
{
	if (!model)
		return;
	model->send_frame = send_frame;
	model->send_opaque = opaque;
}

void dwifi_model_set_packet_write_callback(struct dwifi_model *model,
	void (*write_packet)(void *opaque, uint16_t offset, uint16_t value),
	void *opaque)
{
	if (!model)
		return;
	model->write_packet = write_packet;
	model->packet_opaque = opaque;
}

void dwifi_model_set_peer_wait_callback(struct dwifi_model *model,
	void (*wait_for_peer)(void *opaque), void *opaque)
{
	if (!model)
		return;
	model->wait_for_peer = wait_for_peer;
	model->peer_wait_opaque = opaque;
}

void dwifi_model_set_receive_trace_callback(struct dwifi_model *model,
	void (*trace_receive)(void *opaque, const char *stage,
		const uint8_t *frame, size_t length, uint16_t address),
	void *opaque)
{
	if (!model)
		return;
	model->trace_receive = trace_receive;
	model->trace_opaque = opaque;
}

static void trace_receive(struct dwifi_model *model, const char *stage,
	const uint8_t *frame, size_t length, uint16_t address)
{
	if (model->trace_receive)
		model->trace_receive(model->trace_opaque, stage, frame, length, address);
}

/* The curated ownership set leaves every other register answered by DraStic's
 * stub, which returns zero — so each unowned register is a silent zero waiting
 * to gate something. Six registers are actively maintained by this model and
 * still not served; the guest polls them hundreds of times a run and is told
 * nothing.
 *
 * Two of those six are not radio state at all. W_RXTXAddr is the cursor melonDS
 * streams received payload through, and W_TXSeqNo is incremented by this model
 * on every transmit. The model is the sole authority for both, so serving them
 * is not the "partial radio state" that regressed the join when W_RFStatus and
 * W_RFPins were flipped — that pair is deliberately left alone here.
 *
 * Runtime-gated so the change stays attributable without a rebuild. */
static int own_datapath_registers;

void dwifi_model_set_datapath_ownership(int enabled)
{
	own_datapath_registers = enabled ? 1 : 0;
}

/* Serve the radio state the model already tracks: W_RFStatus, W_RFPins, and the
 * two registers still answered by hardcoded constants from before power
 * management existed.
 *
 * Direct evidence for this. Immediately before the client clears its AID and
 * transmits a disassociation, its last two reads are of W_RFStatus, and both
 * return zero — which is not a state this radio can be in. It then leaves. The
 * teardown is not a timeout; it is the station acting on a radio that reports
 * nothing.
 *
 * Flipping this pair regressed the join once before, but that was measured
 * before the power state machine, IRQ15, the reply-slot fix and medium
 * contention existed, and the log's own next-step list asks for it to be
 * retried as one deliberate change with its own verification run. The two stub
 * constants must go with it: while REG_POWER_STATE answers a constant 0x0200
 * the guest is told the transceiver is permanently powered down no matter what
 * the model does, which makes serving W_RFStatus incoherent rather than
 * merely partial. */
static int own_radio_registers;

void dwifi_model_set_radio_ownership(int enabled)
{
	own_radio_registers = enabled ? 1 : 0;
}

int dwifi_model_owns_read(uint32_t address)
{
	uint32_t window = window_offset(address);
	if (window >= PACKET_WINDOW_START && window < PACKET_WINDOW_END)
		return 1;
	if (window >= UINT32_C(0x2000) && window < UINT32_C(0x4000))
		return 1;
	if (window >= UINT32_C(0x6000))
		window &= DWIFI_REGISTER_SIZE - 1;
	else if (window >= DWIFI_REGISTER_SIZE)
		return 0;
	window &= ~UINT32_C(1);
	/* The microsecond counter block belongs to the model too. DraStic returns
	 * zero for it, and Mario Kart's host times the association exchange from
	 * W_US_COUNT: with a counter frozen at zero it abandons the join and
	 * deauthenticates instead of answering the association request. */
	/* Not served to the guest yet. The power state machine below is modelled
	 * and tracked, but serving W_RFStatus/W_RFPins on top of it regressed the
	 * join: the client stopped registering with the host entirely. The radio
	 * state is still partial — no TX/RX gating on power, no IRQ15 wake, no
	 * W_PowerUS — so the guest sees transitions it cannot reconcile. Flip this
	 * on again only once those are modelled, as its own change. */
	if (own_datapath_registers &&
	    (window == REG_RX_TX_ADDR || window == REG_TX_SEQ_NO))
		return 1;
	if (own_radio_registers &&
	    (window == REG_RF_STATUS || window == REG_RF_PINS ||
	     window == REG_MODE_RST || window == REG_POWER_STATE))
		return 1;
	return window == REG_W_IF || window == REG_W_IE ||
		(window >= REG_RX_CNT && window <= REG_TX_STAT) ||
		(window >= REG_US_COUNT_CNT && window <= REG_US_COUNT3) ||
		window == REG_BEACON_COUNT1 || window == REG_BEACON_COUNT2 ||
		window == REG_W_IF_SET;
}

int dwifi_model_irq_pending(const struct dwifi_model *model)
{
	return model && model->irq_asserted;
}

void dwifi_model_raise_irq(struct dwifi_model *model, unsigned interrupt)
{
	if (!model || interrupt >= 16)
		return;
	uint16_t flags = load16(model->registers, REG_W_IF);
	flags |= (uint16_t)(UINT16_C(1) << interrupt);
	store16(model->registers, REG_W_IF, flags);
	update_irq(model);
}

static uint16_t timer_halfword(uint64_t value, uint32_t offset,
	uint32_t first_offset)
{
	unsigned shift = (unsigned)(offset - first_offset) * 8;
	return (uint16_t)(value >> shift);
}

static uint16_t reg16(const struct dwifi_model *model, uint32_t offset)
{
	return load16(model->registers, offset);
}

static void set_reg16(struct dwifi_model *model, uint32_t offset, uint16_t value)
{
	store16(model->registers, offset, value);
}

static uint16_t increment_rx_addr(const struct dwifi_model *model,
	uint16_t address)
{
	address = (uint16_t)((address + 2) & UINT16_C(0x1ffe));
	if (address == (reg16(model, REG_RX_BUF_END) & UINT16_C(0x1ffe)))
		address = reg16(model, REG_RX_BUF_BEGIN) & UINT16_C(0x1ffe);
	return address;
}

static void packet_store_ring(struct dwifi_model *model, uint16_t *address,
	uint16_t value)
{
	uint16_t offset = *address & UINT16_C(0x1ffe);
	store16(model->packet_ram, offset, value);
	if (model->write_packet)
		model->write_packet(model->packet_opaque, offset, value);
	*address = increment_rx_addr(model, *address);
}

static int mac_equal(const uint8_t *left, const uint8_t *right)
{
	return memcmp(left, right, 6) == 0;
}

static uint16_t classify_rx_frame(const struct dwifi_model *model,
	const uint8_t *frame, uint16_t frame_length)
{
	uint16_t flags = UINT16_C(0x0010);
	if (frame_length < 24)
		return flags;
	const uint8_t *wifi = frame + 12;
	uint16_t control = load16(wifi, 0);
	unsigned type = (control >> 2) & 3;
	unsigned subtype = (control >> 4) & 15;
	const uint8_t *bssid = model->registers + UINT32_C(0x0020);

	if (type == 0) {
		if (mac_equal(wifi + 16, bssid))
			flags |= UINT16_C(0x8000);
		if (subtype == 8)
			flags |= UINT16_C(0x0001);
	} else if (type == 2) {
		/* Data frames select their BSSID field from ToDS/FromDS.  MP command
		 * and ACK frames use FromDS (address 2 at +10); client replies use ToDS
		 * (address 1 at +4).  Nintendo software requires the resulting MAC-good
		 * bit in addition to the low-nibble MP classification. */
		unsigned from_to = (control >> 8) & 3;
		static const uint8_t bssid_offsets[4] = {16, 4, 10, 0};
		if (bssid_offsets[from_to] &&
		    mac_equal(wifi + bssid_offsets[from_to], bssid))
			flags |= UINT16_C(0x8000);
		static const uint8_t mp_cmd[6] = {3, 9, 0xbf, 0, 0, 0};
		static const uint8_t mp_reply[6] = {3, 9, 0xbf, 0, 0, 0x10};
		static const uint8_t mp_ack[6] = {3, 9, 0xbf, 0, 0, 3};
		if (mac_equal(wifi + 16, mp_reply))
			flags |= subtype == 5 ? UINT16_C(0x000f) : UINT16_C(0x000e);
		else if (mac_equal(wifi + 4, mp_cmd))
			flags |= UINT16_C(0x000c);
		else if (mac_equal(wifi + 4, mp_ack))
			flags |= UINT16_C(0x000d);
		else
			flags |= UINT16_C(0x0008);
	}
	return flags;
}

/* A duplicate command is not delivered to the guest: no RX header, no cursor
 * movement and no RX-complete interrupt. The station still owes the host a
 * reply for it, so that half is scheduled exactly as for a fresh command. */
static int finish_duplicate_command(struct dwifi_model *model,
	const uint8_t *frame, uint16_t frame_length)
{
	model->mp_reply_client_time = frame_length >= 26 ?
		load16(frame, 12 + 24) : 0;
	model->mp_reply_due_us = model->us_timestamp +
		DWIFI_MP_REPLY_TURNAROUND_US;
	model->mp_reply_due = 1;
	model->mp_cmd_duplicate++;
	model->rx_frame_count++;
	return 1;
}

int dwifi_model_receive_frame(struct dwifi_model *model,
	const uint8_t *frame, size_t length)
{
	if (!model || !frame || length < 12 || length > DWIFI_PACKET_RAM_SIZE)
		return 0;
	if (!(reg16(model, REG_RX_CNT) & UINT16_C(0x8000)))
		return 0;
	uint16_t frame_length = load16(frame, 10) & UINT16_C(0x3fff);
	if ((size_t)frame_length + 12 > length)
		return 0;
	if (!model->rx_completing) {
		if (model->rx_active)
			return 0;
		memcpy(model->rx_pending, frame, length);
		model->rx_pending_length = length;
		model->rx_active = 1;
		uint16_t pending_control = frame_length >= 2 ? load16(frame, 12) : 0;
		uint16_t pending_crop = reg16(model, REG_RX_LEN_CROP);
		uint16_t pending_crop_bytes = pending_control & UINT16_C(1 << 14) ?
			(uint16_t)((pending_crop >> 7) & UINT16_C(0x01fe)) :
			(uint16_t)((pending_crop << 1) & UINT16_C(0x01fe));
		model->rx_pending_frame_length = pending_crop_bytes > frame_length ? 0 :
			(uint16_t)(frame_length - pending_crop_bytes);
		model->rx_pending_stored_length = (uint16_t)
			((model->rx_pending_frame_length + 3U) & ~UINT16_C(3));
		model->rx_pending_ptr = 0;
		/* The radio header uses 0x14 for 2 Mbit/s; other DS rates seen by
		 * dswifi here are 1 Mbit/s.  Completion is observed on the model's
		 * eight-microsecond hardware timer boundary. */
		model->rx_halfword_us = frame[8] == UINT8_C(0x14) ? 8u : 16u;
		uint32_t airtime = (uint32_t)model->rx_pending_frame_length *
			(frame[8] == UINT8_C(0x14) ? 4u : 8u);
		if (!airtime)
			airtime = WIFI_TIMER_INTERVAL_US;
		model->rx_complete_at_us = model->us_timestamp + airtime;
		model->rx_next_halfword_at_us =
			model->us_timestamp + model->rx_halfword_us;
		uint16_t start_address =
			(uint16_t)(reg16(model, REG_RX_WRITE_CURSOR) << 1);
		set_rf_status(model, DWIFI_RF_RECEIVING);
		trace_receive(model, "begin", frame, length, start_address);
		set_reg16(model, REG_RX_TX_ADDR,
			(uint16_t)(((start_address + 12) & UINT16_C(0x1ffe)) >> 1));
		dwifi_model_raise_irq(model, WIFI_IRQ_RX_START);
		trace_receive(model, "rx-start-irq", frame, length, start_address);
		return 1;
	}
	uint16_t address = (uint16_t)(reg16(model, REG_RX_WRITE_CURSOR) << 1);
	/* RXLenCrop is applied by the MAC before it writes the RX header and
	 * payload.  dswifi programs 0x0602 for ordinary frames, cropping the
	 * four-byte 802.11 FCS from the length visible to software. */
	uint16_t control = frame_length >= 2 ? load16(frame, 12) : 0;
	uint16_t crop = reg16(model, REG_RX_LEN_CROP);
	uint16_t crop_bytes = control & UINT16_C(1 << 14) ?
		(uint16_t)((crop >> 7) & UINT16_C(0x01fe)) :
		(uint16_t)((crop << 1) & UINT16_C(0x01fe));
	if (crop_bytes > frame_length)
		frame_length = 0;
	else
		frame_length = (uint16_t)(frame_length - crop_bytes);
	/* The MAC reports received lengths rounded to a four-byte boundary, and the
	 * software advances the circular read cursor from that advertised value. */
	uint16_t stored_length = (uint16_t)((frame_length + 3U) & ~UINT16_C(3));

	/* Hardware raises RX-start as the MAC begins writing the frame into packet
	 * RAM, before RX-complete. dswifi enables and acknowledges both, so both
	 * must appear in W_IF even though transport delivery is atomic within one
	 * DraStic event callback. */
	uint16_t receive_flags = classify_rx_frame(model, frame, frame_length);
	/* melonDS: a CMD frame repeating the previous sequence number is a
	 * duplicate. The header is not written and RX-complete is not raised, so
	 * the guest never sees it, but the station still owes the host a reply. */
	int cmd_dupe = 0;
	if ((receive_flags & UINT16_C(0x000f)) == UINT16_C(0x000c) &&
	    frame_length >= 24) {
		uint16_t seqno = load16(frame, 12 + 22);
		if (model->mp_last_seqno_valid && seqno == model->mp_last_seqno)
			cmd_dupe = 1;
		model->mp_last_seqno = seqno;
		model->mp_last_seqno_valid = 1;
	}
	if (cmd_dupe)
		return finish_duplicate_command(model, frame, frame_length);
	trace_receive(model, "classified", frame, length, address);
	uint16_t first_offset = address & UINT16_C(0x1ffe);
	store16(model->packet_ram, first_offset, receive_flags);
	trace_receive(model, "header-shadow-0", frame, length, address);
	if (model->write_packet)
		model->write_packet(model->packet_opaque, first_offset, receive_flags);
	trace_receive(model, "header-guest-0", frame, length, address);
	address = increment_rx_addr(model, address);
	trace_receive(model, "header-word-0", frame, length, address);
	packet_store_ring(model, &address, UINT16_C(0x0040));
	trace_receive(model, "header-word-1", frame, length, address);
	packet_store_ring(model, &address, 0);
	trace_receive(model, "header-word-2", frame, length, address);
	/* The transport's 12-byte radio header carries the transmit rate in
	 * byte 8.  The DS RX header exposes that rate as a 16-bit value at +6. */
	packet_store_ring(model, &address, frame[8]);
	trace_receive(model, "header-word-3", frame, length, address);
	packet_store_ring(model, &address, stored_length);
	trace_receive(model, "header-word-4", frame, length, address);
	packet_store_ring(model, &address, UINT16_C(0x4080));
	trace_receive(model, "header-written", frame, length, address);
	/* The payload was transferred progressively through W_RXTXAddr while the
	 * radio was receiving.  Completion only installs the header and publishes
	 * the final circular-buffer cursor. */
	address = (uint16_t)(reg16(model, REG_RX_TX_ADDR) << 1);
	trace_receive(model, "body-written", frame, length, address);
	if (address & 2)
		address = increment_rx_addr(model, address);
	set_reg16(model, REG_RX_WRITE_CURSOR, (uint16_t)((address & ~3U) >> 1));
	trace_receive(model, "cursor-written", frame, length, address);

	/* A host only stays awake to answer stations during the window after its
	 * beacon. Over an emulated link these frames are not time-synced to that
	 * window and routinely arrive late, so hold the window open when one lands.
	 * Without this the host receives the association request but has already
	 * closed its window, and refuses. */
	if (frame_length >= 24) {
		uint16_t control = load16(frame, 12);
		uint16_t kind = control & UINT16_C(0x00ff);
		const uint8_t *destination = frame + 12 + 4;
		int addressed_here = (destination[0] & 1) ||
			mac_equal(destination, model->registers + REG_MAC_ADDR0);
		uint16_t post_beacon = reg16(model, REG_BEACON_COUNT2);
		if (post_beacon && addressed_here &&
		    (kind == UINT16_C(0x00b0) || kind == UINT16_C(0x0010) ||
		     kind == UINT16_C(0x0000)))
			set_reg16(model, REG_BEACON_COUNT2,
				(uint16_t)(post_beacon + 10));
	}

	/* A beacon from the network this station belongs to carries the host's
	 * clock, and the station adopts it. Without this a client's microsecond
	 * counter is unrelated to the host's, so everything it schedules against
	 * the host's beacon — the authentication and association exchange above
	 * all — lands at an arbitrary time and misses the window the host is
	 * listening in. The timestamp is corrected for the airtime between it being
	 * stamped and the frame finishing. */
	if ((receive_flags & UINT16_C(0x800f)) == UINT16_C(0x8001) &&
	    frame_length >= 32) {
		uint64_t timestamp = 0;
		for (unsigned index = 0; index < 8; ++index)
			timestamp |= (uint64_t)frame[12 + 24 + index] << (index * 8);
		uint32_t airtime = (uint32_t)stored_length *
			(frame[8] == 0x14 ? 4u : 8u);
		model->us_counter = timestamp + (airtime > 76 ? airtime - 76 : 0);
	}

	dwifi_model_raise_irq(model, WIFI_IRQ_RX_COMPLETE);
	set_rf_status(model, DWIFI_RF_IDLE);

	if (frame_length >= 24) {
		static const uint8_t cmd_flow[6] = {3, 9, 0xbf, 0, 0, 0};
		static const uint8_t reply_flow[6] = {3, 9, 0xbf, 0, 0, 0x10};
		/* A command frame obliges this station to answer. */
		if (mac_equal(frame + 12 + 4, cmd_flow)) {
			uint16_t client_time = frame_length >= 26 ?
				load16(frame, 12 + 24) : 0;
			/* Do not answer from inside this call. Raising the receive
			 * interrupt above only sets IF and pokes the emulator's
			 * interrupt state; the guest has not executed an instruction
			 * yet, so its reply slot is necessarily still empty. Schedule
			 * the reply and let dwifi_model_advance_us() send it once the
			 * guest has had a turn. */
			model->mp_reply_client_time = client_time;
			model->mp_reply_due_us = model->us_timestamp +
				DWIFI_MP_REPLY_TURNAROUND_US;
			model->mp_reply_due = 1;
			model->mp_cmd_delivered++;
		}
		/* A reply clears its sender from the outstanding set. The link carries
		 * one client, so the reply identifies itself by arriving; telling
		 * several clients apart needs each station's AID, which a reply frame
		 * does not carry. */
		else if (mac_equal(frame + 12 + 16, reply_flow)) {
			model->mp_reply_seen = 1;
			model->mp_last_reply_us = model->us_timestamp;
			if (model->mp_cmd_active)
				model->mp_client_fail = 0;
		}
	}
	trace_receive(model, "rx-complete-irq", frame, length, address);
	model->rx_frame_count++;
	return 1;
}

/* Index into the transmit slot order used by fire_tx and tx_slot_armed. */
static size_t tx_slot_index(uint32_t offset)
{
	if (offset == REG_TX_SLOT_LOC3)
		return 0;
	if (offset == REG_TX_SLOT_LOC2)
		return 1;
	if (offset == REG_TX_SLOT_CMD)
		return 2;
	return 3;
}

/* The MAC fills in sequence control at transmit time from W_TXSeqNo unless the
 * guest disables header generation. Frames sent straight out of packet RAM keep
 * whatever the guest left there, which is the same value for every frame: a
 * receiver's duplicate filter then drops everything after the first frame from
 * that source. */
static void apply_tx_sequence(struct dwifi_model *model, uint8_t *frame,
	uint16_t frame_length)
{
	if (frame_length < 24 || frame[4] != 0 ||
	    (reg16(model, REG_TX_HEADER_CNT) & UINT16_C(1 << 2)))
		return;
	uint16_t sequence = reg16(model, REG_TX_SEQ_NO) & UINT16_C(0x0fff);
	store16(frame, 12 + 22, (uint16_t)(sequence << 4));
	set_reg16(model, REG_TX_SEQ_NO,
		(uint16_t)((sequence + 1) & UINT16_C(0x0fff)));
}

static unsigned count_clients(uint16_t mask)
{
	unsigned clients = 0;
	while (mask) {
		clients += mask & 1;
		mask >>= 1;
	}
	return clients;
}

/* The acknowledgement closing a command transfer is generated by the MAC, not
 * by the guest. It is addressed to the 03:09:bf:00:00:03 flow, and carries the
 * remaining command time and the mask of clients that did not answer. */
static void send_mp_ack(struct dwifi_model *model)
{
	if (!model->send_frame)
		return;
	memset(mp_ack_frame, 0, sizeof(mp_ack_frame));
	store16(mp_ack_frame, 10, 32);
	mp_ack_frame[8] = model->mp_cmd_rate;
	mp_ack_frame[9] = 1;

	uint8_t *wifi = mp_ack_frame + 12;
	static const uint8_t ack_flow[6] = {3, 9, 0xbf, 0, 0, 3};
	store16(wifi, 0, UINT16_C(0x0218));
	memcpy(wifi + 4, ack_flow, sizeof(ack_flow));
	memcpy(wifi + 10, model->registers + REG_BSSID0, 6);
	memcpy(wifi + 16, model->registers + REG_MAC_ADDR0, 6);
	store16(wifi, 22,
		(uint16_t)(reg16(model, REG_TX_SEQ_NO) << 4));
	store16(wifi, 24, (uint16_t)((reg16(model, REG_CMD_COUNT) + 9) / 10));
	store16(wifi, 26, model->mp_client_fail);

	model->send_frame(model->send_opaque, mp_ack_frame, sizeof(mp_ack_frame));
	model->tx_frame_count++;
}

/* Reply wait over: acknowledge, report which clients answered, and signal the
 * end of the whole command transfer. */
static void finish_mp_cmd(struct dwifi_model *model)
{
	trace_receive(model, "mp-reply-window-end",
		model->packet_ram + model->mp_cmd_address, 0,
		model->mp_cmd_address);
	model->mp_cmd_active = 0;
	/* Proceed on the last known reply rather than penalising a station whose
	 * answer simply could not arrive in time. Without this the host declares
	 * the client failed on almost every generation, because the emulated reply
	 * window is far shorter than the link's round trip. */
	if (model->mp_client_fail && model->mp_last_reply_us &&
	    model->us_timestamp - model->mp_last_reply_us <
		DWIFI_MP_REPLY_GRACE_US)
		model->mp_client_fail = 0;

	dwifi_model_raise_irq(model, WIFI_IRQ_TX_START);
	set_rf_status(model, DWIFI_RF_REPLY_OR_ACK);
	set_reg16(model, REG_RX_TX_ADDR, UINT16_C(0x0fc0));
	send_mp_ack(model);
	/* The command-transfer completion is the end of the generated ACK, not the
	 * instant it is handed to the transport.  Keeping IRQ12 behind that airtime
	 * prevents the guest from producing the next command faster than a DS radio
	 * can put ACK/CMD pairs on the medium. */
	model->mp_ack_active = 1;
	model->mp_ack_deadline_us = model->us_timestamp + 192 + 32 *
		(model->mp_cmd_rate == UINT8_C(0x14) ? 4 : 8);
}

static void finish_mp_ack(struct dwifi_model *model)
{
	model->mp_ack_active = 0;
	/* The guest reads the outcome out of the command frame's own header: a
	 * status of its own, and the clients still outstanding.  Hardware commits
	 * this header before exposing TX-complete; IRQ callbacks run synchronously
	 * here, so preserving that ordering is observable to the guest. */
	uint32_t address = model->mp_cmd_address;
	if (address + 4 <= DWIFI_PACKET_RAM_SIZE) {
		uint16_t status = model->mp_client_fail ? UINT16_C(0x0005) :
			UINT16_C(0x0001);
		store16(model->packet_ram, address, status);
		store16(model->packet_ram, address + 2, model->mp_client_fail);
		if (model->write_packet) {
			model->write_packet(model->packet_opaque, (uint16_t)address,
				status);
			model->write_packet(model->packet_opaque,
				(uint16_t)(address + 2), model->mp_client_fail);
		}
	}
	/* A successful MP transfer counts once when its generated ACK finishes.
	 * The low byte at +4 is the saturating hardware retry/TX count; the upper
	 * byte is cleared.  The ACK also consumes the sequence number after placing
	 * its pre-increment value in the generated frame. */
	if (!model->mp_client_fail && address + 6 <= DWIFI_PACKET_RAM_SIZE) {
		uint8_t count = model->packet_ram[address + 4];
		if (count != UINT8_MAX)
			count++;
		store16(model->packet_ram, address + 4, count);
		if (model->write_packet)
			model->write_packet(model->packet_opaque,
				(uint16_t)(address + 4), count);
	}
	set_reg16(model, REG_TX_SEQ_NO,
		(uint16_t)((reg16(model, REG_TX_SEQ_NO) + 1) & UINT16_C(0x0fff)));

	if (reg16(model, REG_TX_STAT_CNT) & UINT16_C(0x2000)) {
		set_reg16(model, REG_TX_STAT, UINT16_C(0x0b01));
		dwifi_model_raise_irq(model, WIFI_IRQ_TX_COMPLETE);
	}

	set_reg16(model, REG_TX_BUSY,
		(uint16_t)(reg16(model, REG_TX_BUSY) & ~model->mp_cmd_busy_bit));
	if (model->mp_cmd_slot)
		set_reg16(model, model->mp_cmd_slot,
			(uint16_t)(reg16(model, model->mp_cmd_slot) & UINT16_C(0x7fff)));
	set_rf_status(model, DWIFI_RF_IDLE);
	dwifi_model_raise_irq(model, WIFI_IRQ_MP_END);
	trace_receive(model, "mp-ack-complete",
		model->packet_ram + address, 0, (uint16_t)address);
}

/* A command frame polls the clients named by the mask in its own body. */
static void start_mp_cmd(struct dwifi_model *model, uint32_t address,
	uint16_t frame_length, uint32_t transmit_airtime, uint8_t rate,
	uint16_t slot, uint16_t busy_bit)
{
	if (reg16(model, REG_TX_STAT_CNT) & UINT16_C(0x4000)) {
		set_reg16(model, REG_TX_STAT, UINT16_C(0x0800));
		dwifi_model_raise_irq(model, WIFI_IRQ_TX_COMPLETE);
	}

	uint16_t mask = 0;
	if (frame_length >= 26 && address + 12 + 24 + 4 <= DWIFI_PACKET_RAM_SIZE)
		mask = load16(model->packet_ram, address + 12 + 24 + 2);
	model->mp_client_mask = mask;
	model->mp_client_fail = mask;
	/* A reply has no command-generation identifier. With one client, a valid
	 * reply received just across the previous window boundary still proves the
	 * station is participating; carry that observation across the wall-clock
	 * scheduling skew between the two emulator processes. */
	if (model->mp_reply_seen)
		model->mp_client_fail = 0;
	model->mp_cmd_address = (uint16_t)address;
	model->mp_cmd_slot = slot;
	model->mp_cmd_busy_bit = busy_bit;
	model->mp_cmd_rate = rate;
	model->mp_cmd_active = 1;
	model->mp_cmd_started_us = model->us_timestamp;

	set_rf_status(model, DWIFI_RF_AWAITING_REPLIES);
	model->mp_reply_deadline_us = model->us_timestamp + transmit_airtime + 16 +
		((10 + (uint64_t)reg16(model, REG_CMD_REPLY_TIME)) *
			count_clients(mask));
	trace_receive(model, "mp-cmd-start", model->packet_ram + address,
		frame_length, (uint16_t)address);
	/* A transfer with nobody to poll still has to terminate. */
	if (!mask) {
		finish_mp_cmd(model);
		return;
	}
}

/* Clients have no control over the reply transfer: receiving a command frame
 * makes the MAC answer in the station's own slot. A station with nothing to say
 * still answers, with an empty frame, because the host counts the replies. */
static void send_mp_reply(struct dwifi_model *model, uint16_t client_time)
{
	if (!model->send_frame)
		return;
	/* Reply1 is the guest's staging register. Hardware moves it into Reply2 at
	 * the command boundary and transmits from that latch. This ordering matters
	 * when dswifi prepares the next payload while inspecting the previous one. */
	uint16_t previous = reg16(model, REG_TX_SLOT_REPLY2);
	if (previous & UINT16_C(0x8000)) {
		uint32_t previous_address =
			(uint32_t)(previous & UINT16_C(0x0fff)) << 1;
		if (previous_address + 2 <= DWIFI_PACKET_RAM_SIZE) {
			store16(model->packet_ram, previous_address, UINT16_C(0x0001));
			/* model->packet_ram is a shadow: a write reaches the guest only
			 * through write_packet.  This mark — melonDS's "the packet has been
			 * sent" store at offset 0 of the retired reply buffer — was the one
			 * model-generated status write in this file that never made that
			 * call, so the station staged replies into a slot whose previous
			 * occupant it could never observe as transmitted. */
			if (model->write_packet)
				model->write_packet(model->packet_opaque,
					(uint16_t)previous_address, UINT16_C(0x0001));
		}
	}
	set_reg16(model, REG_TX_SLOT_REPLY2,
		reg16(model, REG_TX_SLOT_REPLY1));
	set_reg16(model, REG_TX_SLOT_REPLY1, 0);
	uint16_t slot = reg16(model, REG_TX_SLOT_REPLY2);
	uint16_t sent_length = 28;
	uint16_t staged_length = 0;
	uint16_t staged_address = 0;
	int sent_populated = 0;
	dwifi_model_raise_irq(model, WIFI_IRQ_TX_START);
	set_rf_status(model, DWIFI_RF_REPLY_OR_ACK);

	if (slot & UINT16_C(0x8000)) {
		uint32_t address = (uint32_t)(slot & UINT16_C(0x0fff)) << 1;
		uint16_t frame_length = load16(model->packet_ram,
			(address + 10) & (DWIFI_PACKET_RAM_SIZE - 1)) &
			UINT16_C(0x3fff);
		staged_address = (uint16_t)address;
		staged_length = frame_length;
		uint32_t duration = 192 + (uint32_t)frame_length * 4;
		if (duration <= client_time &&
		    address + 12 + frame_length <= DWIFI_PACKET_RAM_SIZE) {
			size_t length = (size_t)12 + frame_length;
			memcpy(tx_frame, model->packet_ram + address, length);
			/* Emulator multiplayer transports carry reply-vs-command as
			 * sideband metadata (melonDS uses MP_SendReply). Our frame-only link
			 * has no such channel, while dswifi stages its reply from a command
			 * template. Canonicalize address 3 on the transmitted copy so the
			 * receiving MAC observes the required MP-reply semantic. */
			if (frame_length >= 22) {
				static const uint8_t reply_flow[6] =
					{3, 9, 0xbf, 0, 0, 0x10};
				memcpy(tx_frame + 12 + 16, reply_flow,
					sizeof(reply_flow));
			}
			apply_tx_sequence(model, tx_frame, frame_length);
			/* The MAC marks the header to say a frame was actually sent. */
			tx_frame[4] = 1;
			model->send_frame(model->send_opaque, tx_frame, length);
			model->tx_frame_count++;
			sent_populated = 1;
			sent_length = frame_length;
			store16(model->packet_ram, address + 4,
				load16(model->packet_ram, address + 4) |
					UINT16_C(0x0001));
			if (model->write_packet)
				model->write_packet(model->packet_opaque,
					(uint16_t)(address + 4),
					load16(model->packet_ram, address + 4));
		}
	}
	if (!sent_populated && !model->suppress_default_reply) {
		memset(mp_ack_frame, 0, sizeof(mp_ack_frame));
		store16(mp_ack_frame, 10, 28);
		mp_ack_frame[8] = 0x14;
		mp_ack_frame[9] = 1;
		uint8_t *wifi = mp_ack_frame + 12;
		store16(wifi, 0, UINT16_C(0x0158));
		store16(wifi, 2, UINT16_C(0x00f0));
		memcpy(wifi + 4, model->registers + REG_BSSID0, 6);
		memcpy(wifi + 10, model->registers + REG_MAC_ADDR0, 6);
		static const uint8_t reply_flow[6] =
			{3, 9, 0xbf, 0, 0, 0x10};
		memcpy(wifi + 16, reply_flow, sizeof(reply_flow));
		apply_tx_sequence(model, mp_ack_frame, 28);
		model->send_frame(model->send_opaque, mp_ack_frame, 12 + 28);
		model->tx_frame_count++;
	}
	model->mp_replies_sent++;
	if (sent_populated)
		model->mp_replies_populated++;
	trace_receive(model, sent_populated ? "mp-reply-populated" :
		(model->suppress_default_reply ? "mp-reply-suppressed" :
			"mp-reply-default"), model->packet_ram + staged_address,
		staged_length, staged_address);
	/* A station reply owns W_TXBusy bit 7 until its on-air transfer ends.  The
	 * guest observes this around IRQ0, and melonDS only reports IRQ1 after that
	 * interval rather than at transport submission time. */
	set_reg16(model, REG_TX_BUSY,
		(uint16_t)(reg16(model, REG_TX_BUSY) | UINT16_C(0x0080)));
	model->mp_reply_active = 1;
	model->mp_reply_tx_deadline_us = model->us_timestamp + 192 +
		(uint64_t)sent_length * 4;
	occupy_medium(model, sent_length, UINT8_C(0x14));
}

static void finish_mp_reply(struct dwifi_model *model)
{
	model->mp_reply_active = 0;
	if (reg16(model, REG_TX_STAT_CNT) & UINT16_C(0x1000)) {
		set_reg16(model, REG_TX_STAT, UINT16_C(0x0401));
		dwifi_model_raise_irq(model, WIFI_IRQ_TX_COMPLETE);
	}
	set_reg16(model, REG_TX_BUSY,
		(uint16_t)(reg16(model, REG_TX_BUSY) & ~UINT16_C(0x0080)));
	set_rf_status(model, DWIFI_RF_IDLE);
}

static void fire_tx(struct dwifi_model *model)
{
	if (!(reg16(model, REG_RX_CNT) & UINT16_C(0x8000)))
		return;
	/* The air is still occupied by a frame this station is transmitting. Leave
	 * every slot armed and let dwifi_model_advance_us() retry once it clears:
	 * hardware defers here, it does not discard. */
	if (model->us_timestamp < model->medium_busy_until_us) {
		model->tx_deferred = 1;
		return;
	}
	model->tx_deferred = 0;
	static const uint16_t slots[] = {
		REG_TX_SLOT_LOC3, REG_TX_SLOT_LOC2, REG_TX_SLOT_CMD, REG_TX_SLOT_LOC1
	};
	static const uint16_t bits[] = {8, 4, 2, 1};
	/* Mario Kart's host answers an authentication frame by setting W_TXREQ bit
	 * 3 and then programming the slot at 0x00a4, which a fixed bit-to-slot map
	 * pairs with a different slot; the reply was silently never sent. Rather
	 * than guess the mapping, arm a slot when the guest enables it and send it
	 * on the next request, which needs no bit assignment and cannot repeat. */
	uint16_t requests = reg16(model, REG_TX_REQ_READ);
	for (size_t index = 0; index < sizeof(slots) / sizeof(slots[0]); ++index) {
		int is_mp_command = 0;
		uint16_t slot = reg16(model, slots[index]);
		if (!requests || !model->tx_slot_armed[index] ||
		    !(slot & UINT16_C(0x8000)))
			continue;
		model->tx_slot_armed[index] = 0;
		set_reg16(model, REG_TX_BUSY,
			(uint16_t)(reg16(model, REG_TX_BUSY) | bits[index]));
		uint32_t address = (uint32_t)(slot & UINT16_C(0x0fff)) << 1;
		uint16_t frame_length = load16(model->packet_ram,
			(address + 10) & (DWIFI_PACKET_RAM_SIZE - 1)) & UINT16_C(0x3fff);
		if (address + 12 + frame_length <= DWIFI_PACKET_RAM_SIZE &&
		    model->send_frame) {
			size_t length = (size_t)12 + frame_length;
			memcpy(tx_frame, model->packet_ram + address, length);
			apply_tx_sequence(model, tx_frame, frame_length);
			set_rf_status(model, DWIFI_RF_TRANSMITTING);
			dwifi_model_raise_irq(model, WIFI_IRQ_TX_START);
			model->send_frame(model->send_opaque, tx_frame, length);
			model->tx_frame_count++;
			occupy_medium(model, frame_length, tx_frame[8]);
			static const uint8_t mp_cmd_flow[6] =
				{3, 9, 0xbf, 0, 0, 0};
			uint16_t frame_control = frame_length >= 24 ?
				load16(tx_frame, 12) : 0;
			is_mp_command = ((frame_control >> 2) & 3) == 2 &&
				mac_equal(tx_frame + 12 + 4, mp_cmd_flow);
			if (is_mp_command) {
				uint32_t airtime = 192 + (uint32_t)frame_length *
					(tx_frame[8] == UINT8_C(0x14) ? 4u : 8u);
				start_mp_cmd(model, address, frame_length, airtime, tx_frame[8],
					slots[index], bits[index]);
			} else {
				complete_tx_header(model, address);
				set_rf_status(model, DWIFI_RF_IDLE);
			}
		}
		if (!is_mp_command) {
			set_reg16(model, REG_TX_BUSY,
				(uint16_t)(reg16(model, REG_TX_BUSY) & ~bits[index]));
			set_reg16(model, slots[index], slot & UINT16_C(0x7fff));
			set_reg16(model, REG_TX_STAT, (uint16_t)(UINT16_C(1) |
				(bits[index] == 4 ? UINT16_C(0x1000) :
				 bits[index] == 8 ? UINT16_C(0x2000) : 0)));
			dwifi_model_raise_irq(model, WIFI_IRQ_TX_COMPLETE);
		}
		return;
	}
}

/* Tracked but not served to the guest: answering reads of W_RFStatus with the
 * modelled state made Mario Kart's host spin on it 8000 times and never reach
 * the point of beaconing, where DraStic's own value lets it proceed. The state
 * machine is kept because the multiplayer transfers are defined in terms of it,
 * but ownership of the register has to wait until the whole radio state,
 * including power management, is modelled. */
static void set_rf_status(struct dwifi_model *model, unsigned status)
{
	static const uint16_t pins[] = {
		0x04, 0x84, 0, 0x46, 0, 0x84, 0x87, 0, 0x46, 0x04
	};
	if (status >= sizeof(pins) / sizeof(pins[0]))
		return;
	set_reg16(model, REG_RF_STATUS, (uint16_t)status);
	set_reg16(model, REG_RF_PINS, pins[status]);
}

/* melonDS ComStatus: a transfer is on the air. The transceiver may signal that
 * it is going down, but does not actually stop until the medium is quiet. */
static int com_status(const struct dwifi_model *model)
{
	return model->rx_active || model->mp_cmd_active ||
		model->mp_ack_active || model->mp_reply_active;
}

/* Port of melonDS Wifi::UpdatePowerStatus. 1 = power on, 0 = no change,
 * -1 = power off.
 *
 * W_PowerForce overrides everything; clearing W_ModeReset bit 0 forces the
 * transceiver off; otherwise power follows IRQ13/IRQ15 or W_PowerState
 * according to the mode selected in W_ModeWEP, with W_PowerDownCtrl deciding
 * how deep a regular power-down goes. */
static void update_power_status(struct dwifi_model *model, int power)
{
	int current = 0;
	if (reg16(model, REG_TRX_POWER) == 1)
		current |= 1;
	if (!(reg16(model, REG_POWER_STATE) & (1 << 9)))
		current |= 2;
	int requested = current;

	if (reg16(model, REG_POWER_FORCE) & (1 << 15)) {
		requested = (reg16(model, REG_POWER_FORCE) & 1) ? 0 : 3;
	} else if (!(reg16(model, REG_MODE_RST) & 1)) {
		requested = 0;
	} else {
		if (power == 0) {
			if ((reg16(model, REG_POWER_STATE) & UINT16_C(0x0202)) ==
				UINT16_C(0x0202))
				power = 1;
			else if ((reg16(model, REG_POWER_STATE) &
				UINT16_C(0x0201)) == UINT16_C(0x0001))
				power = -1;
		}
		/* W_PowerDownCtrl bit 0 inhibits a regular power-down; bit 1 forces at
		 * least a partial wakeup. Partial states are not modelled, as in
		 * melonDS. */
		if (power == -1 && (reg16(model, REG_POWER_DOWN_CTRL) & 1))
			power = 0;
		if (power == 1)
			requested = 3;
		else if (power == -1)
			requested = reg16(model, REG_POWER_DOWN_CTRL) ? 3 : 0;
		else if (reg16(model, REG_POWER_DOWN_CTRL) & (1 << 1))
			requested = 3;
	}

	if (requested == current)
		return;

	if (requested & 1) {
		if (!(current & 1)) {
			set_reg16(model, REG_TRX_POWER, 1);
			set_rf_status(model, DWIFI_RF_IDLE);
		}
	} else {
		/* Signals that the transceiver is about to go down. */
		set_reg16(model, REG_TRX_POWER, 2);
		if (!com_status(model)) {
			set_reg16(model, REG_TRX_POWER, 0);
			set_rf_status(model, DWIFI_RF_POWERED_DOWN);
		}
	}

	if (requested & 2) {
		set_reg16(model, REG_POWER_STATE,
			(uint16_t)(reg16(model, REG_POWER_STATE) | (1 << 8)));
		if (!(current & 2) && model->us_until_power_on == 0) {
			/* The radio is not usable the instant it is asked for: hardware
			 * takes about 2 ms to come up, and the guest is told with IRQ11. */
			model->us_until_power_on = -2048;
			dwifi_model_raise_irq(model, WIFI_IRQ_RF_WAKEUP);
		}
	} else {
		set_reg16(model, REG_POWER_STATE,
			(uint16_t)((reg16(model, REG_POWER_STATE) &
				~UINT16_C(0x0101)) | (1 << 9)));
		model->us_until_power_on = 0;
	}
}

/* Pre-beacon. Auto wakeup works in every power-management mode, unlike auto
 * sleep, so this is not gated on the mode in W_ModeWEP. */
static void raise_pre_beacon_irq(struct dwifi_model *model)
{
	dwifi_model_raise_irq(model, WIFI_IRQ_PRE_BEACON);
	if (reg16(model, REG_POWER_TX) & 1)
		update_power_status(model, 1);
}

/* Post-beacon. Auto power-down happens only in automatic power-saving mode 0.
 * In mode 2 the station does not service IRQ13/IRQ15 at all, so powering down
 * there would strand the transceiver with nothing to wake it. */
static void raise_post_beacon_irq(struct dwifi_model *model)
{
	dwifi_model_raise_irq(model, WIFI_IRQ_POST_BEACON);
	if ((reg16(model, REG_MODE_WEP) & 7) == 0 &&
	    !(reg16(model, REG_POWER_TX) & (1 << 1)))
		update_power_status(model, -1);
}

/* Hardware stamps the outcome back into the transmit header once a frame has
 * gone out, and Nintendo's wireless library checks it. */
static void complete_tx_header(struct dwifi_model *model, uint32_t address)
{
	if (address + 6 > DWIFI_PACKET_RAM_SIZE)
		return;
	store16(model->packet_ram, address, UINT16_C(0x0001));
	uint16_t upper = load16(model->packet_ram, address + 4) & UINT16_C(0x00ff);
	store16(model->packet_ram, address + 4, upper);
	if (model->write_packet) {
		model->write_packet(model->packet_opaque, (uint16_t)address,
			UINT16_C(0x0001));
		model->write_packet(model->packet_opaque, (uint16_t)(address + 4),
			upper);
	}
}

/* Airtime for a frame, from melonDS: a 192 us preamble plus four microseconds
 * per byte at 2 Mbit/s, eight at 1 Mbit/s. The rate byte lives at offset 8 of
 * the transmit header. */
static void occupy_medium(struct dwifi_model *model, uint16_t frame_length,
	uint8_t rate)
{
	uint64_t airtime = 192 + (uint64_t)frame_length *
		(rate == UINT8_C(0x14) ? 4u : 8u);
	model->medium_busy_until_us = model->us_timestamp + airtime;
}

static void fire_beacon(struct dwifi_model *model)
{
	uint16_t slot = reg16(model, REG_TX_SLOT_BEACON);
	if (!(slot & UINT16_C(0x8000)) || !model->send_frame)
		return;
	uint32_t address = (uint32_t)(slot & UINT16_C(0x0fff)) << 1;
	uint16_t frame_length = load16(model->packet_ram,
		(address + 10) & (DWIFI_PACKET_RAM_SIZE - 1)) & UINT16_C(0x3fff);
	if (address + 12 + frame_length <= DWIFI_PACKET_RAM_SIZE) {
		size_t length = 12 + frame_length;
		memcpy(beacon_frame, model->packet_ram + address, length);
		/* Beacon packet RAM contains placeholders for fields supplied by the
		 * MAC at transmit time.  The timestamp is the current US counter; the
		 * sequence control uses and advances W_TXSeqNo unless header generation
		 * is disabled. */
		if (frame_length >= 32) {
			uint64_t timestamp = model->us_counter;
			memcpy(beacon_frame + 12 + 24, &timestamp, sizeof(timestamp));
		}
		apply_tx_sequence(model, beacon_frame, frame_length);
		set_rf_status(model, DWIFI_RF_TRANSMITTING);
		dwifi_model_raise_irq(model, WIFI_IRQ_TX_START);
		model->send_frame(model->send_opaque, beacon_frame, length);
		model->tx_frame_count++;
		/* Deliberately not gated on medium_busy_until_us. The beacon is what
		 * keeps a station associated, hardware reserves the medium for it at
		 * the target beacon transmission time, and delaying it is the beacon
		 * starvation this project has already measured at length. It occupies
		 * the air like any other frame, but it never waits for it. */
		occupy_medium(model, frame_length, beacon_frame[8]);
		complete_tx_header(model, address);
		set_rf_status(model, DWIFI_RF_IDLE);
	}
}

/* The beacon interrupt has three sources, and they are not interchangeable: the
 * beacon-count source is what the gate in W_US_COMPARE bit 0 suppresses, and a
 * forced interrupt does not rearm the beacon counter. Beacon transmission hangs
 * off this interrupt rather than off a free-running timer, so a guest that
 * masks the interrupt stops beaconing, as the hardware does. */
enum dwifi_beacon_irq_source {
	DWIFI_BEACON_IRQ_US_COMPARE = 0,
	DWIFI_BEACON_IRQ_BEACON_COUNT = 1,
	DWIFI_BEACON_IRQ_FORCED = 2
};

static void raise_beacon_irq(struct dwifi_model *model, unsigned source)
{
	if (source != DWIFI_BEACON_IRQ_FORCED)
		set_reg16(model, REG_BEACON_COUNT1,
			reg16(model, REG_BEACON_INTERVAL));
	if (model->beacon_irq_blocked && source == DWIFI_BEACON_IRQ_BEACON_COUNT)
		return;
	if (!(reg16(model, REG_US_COMPARE_CNT) & UINT16_C(1)))
		return;

	dwifi_model_raise_irq(model, WIFI_IRQ_BEACON_SLOT);

	/* Arming the post-beacon countdown opens the window during which a host
	 * stays awake to answer stations trying to connect. */
	set_reg16(model, REG_BEACON_COUNT2, UINT16_C(0xffff));
	set_reg16(model, REG_TX_REQ_READ,
		(uint16_t)(reg16(model, REG_TX_REQ_READ) & UINT16_C(0xfff2)));

	if (reg16(model, REG_TX_SLOT_BEACON) & UINT16_C(0x8000))
		fire_beacon(model);

	if (reg16(model, REG_LISTEN_COUNT) == 0)
		set_reg16(model, REG_LISTEN_COUNT,
			reg16(model, REG_LISTEN_INTERVAL));
	set_reg16(model, REG_LISTEN_COUNT,
		(uint16_t)(reg16(model, REG_LISTEN_COUNT) - 1));
}

/* Hardware-visible work on the DS Wi-Fi millisecond boundary. */
static void advance_ms(struct dwifi_model *model)
{
	uint16_t beacon_count = reg16(model, REG_BEACON_COUNT1);
	if (beacon_count) {
		set_reg16(model, REG_BEACON_COUNT1, --beacon_count);
		if (!beacon_count)
			raise_beacon_irq(model, DWIFI_BEACON_IRQ_BEACON_COUNT);
	}
	if (!reg16(model, REG_BEACON_COUNT1))
		set_reg16(model, REG_BEACON_COUNT1,
			reg16(model, REG_BEACON_INTERVAL));

	uint16_t post_beacon = reg16(model, REG_BEACON_COUNT2);
	if (post_beacon) {
		/* This is the window a host keeps open to hear from stations trying to
		 * join, and it is about to close. The guest measures it in emulated
		 * milliseconds, which can pass in a fraction of that in real time, so
		 * give the link a chance to deliver before giving up on it: an
		 * authentication or association frame arriving now holds the window
		 * open by itself. */
		if (post_beacon == 1 && model->wait_for_peer)
			model->wait_for_peer(model->peer_wait_opaque);
		post_beacon = reg16(model, REG_BEACON_COUNT2);
		set_reg16(model, REG_BEACON_COUNT2, --post_beacon);
		if (!post_beacon)
			raise_post_beacon_irq(model);
	}
}

static void set_timer_halfword(uint64_t *target, uint32_t offset,
	uint32_t first_offset, uint16_t value)
{
	unsigned shift = (unsigned)(offset - first_offset) * 8;
	uint64_t mask = UINT64_C(0xffff) << shift;
	*target = (*target & ~mask) | ((uint64_t)value << shift);
}

void dwifi_model_advance_beacon_wall_us(struct dwifi_model *model,
	uint64_t elapsed_us)
{
	if (!model)
		return;
	model->beacon_wall_remainder_us += elapsed_us;
	while (model->beacon_wall_remainder_us >= 1000) {
		model->beacon_wall_remainder_us -= 1000;
		/* Only the beacon countdown, deliberately. The post-beacon listening
		 * window is not advanced here: it is the guest's own accounting and it
		 * calls back into this barrier, which would recurse. */
		uint16_t beacon_count = reg16(model, REG_BEACON_COUNT1);
		if (beacon_count) {
			set_reg16(model, REG_BEACON_COUNT1, --beacon_count);
			if (!beacon_count) {
				model->beacon_wall_raises++;
				raise_beacon_irq(model, DWIFI_BEACON_IRQ_BEACON_COUNT);
			}
		}
		if (!reg16(model, REG_BEACON_COUNT1))
			set_reg16(model, REG_BEACON_COUNT1,
				reg16(model, REG_BEACON_INTERVAL));
	}
}

void dwifi_model_advance_us(struct dwifi_model *model, uint64_t elapsed_us)
{
	if (!model)
		return;
	/* Serviced at the top of a later call rather than inside the tick loop
	 * below.  The guest runs between two calls into the model, so this is the
	 * earliest point at which a reactively staged reply slot can hold anything.
	 * Deferring it into the loop would still read the slot before the guest had
	 * run and would reproduce the inline behaviour this replaced. */
	if (model->mp_reply_due &&
	    model->us_timestamp >= model->mp_reply_due_us) {
		model->mp_reply_due = 0;
		send_mp_reply(model, model->mp_reply_client_time);
	}
	uint64_t ticks = elapsed_us / WIFI_TIMER_INTERVAL_US;
	uint64_t remainder = elapsed_us % WIFI_TIMER_INTERVAL_US;
	remainder += model->timer_remainder_us;
	ticks += remainder / WIFI_TIMER_INTERVAL_US;
	model->timer_remainder_us = (uint8_t)(remainder % WIFI_TIMER_INTERVAL_US);

	model->advance_calls++;
	while (ticks--) {
		model->tick_count++;
		model->us_timestamp += WIFI_TIMER_INTERVAL_US;
		if (model->rx_active && model->rx_pending_ptr <
		    model->rx_pending_stored_length &&
		    model->us_timestamp >= model->rx_next_halfword_at_us) {
			uint16_t ptr = model->rx_pending_ptr;
			uint16_t value = ptr < model->rx_pending_frame_length ?
				model->rx_pending[12 + ptr] : 0;
			if (ptr + 1 < model->rx_pending_frame_length)
				value |= (uint16_t)model->rx_pending[12 + ptr + 1] << 8;
			uint16_t address =
				(uint16_t)(reg16(model, REG_RX_TX_ADDR) << 1);
			model->rx_halfword_writes++;
			packet_store_ring(model, &address, value);
			set_reg16(model, REG_RX_TX_ADDR, (uint16_t)(address >> 1));
			model->rx_pending_ptr = (uint16_t)(ptr + 2);
			model->rx_next_halfword_at_us += model->rx_halfword_us;
		}
		if (model->rx_active &&
		    model->us_timestamp >= model->rx_complete_at_us) {
			model->rx_active = 0;
			model->rx_completing = 1;
			(void)dwifi_model_receive_frame(model, model->rx_pending,
				model->rx_pending_length);
			model->rx_completing = 0;
		}
		if (model->us_until_power_on < 0) {
			model->us_until_power_on += WIFI_TIMER_INTERVAL_US;
			if (model->us_until_power_on >= 0) {
				model->us_until_power_on = 0;
				set_reg16(model, REG_POWER_STATE, 0);
				set_rf_status(model, DWIFI_RF_IDLE);
				update_power_status(model, 0);
			}
		}
		if (load16(model->registers, REG_US_COUNT_CNT) & 1) {
			model->us_counter += WIFI_TIMER_INTERVAL_US;
			/* The compare is against the counter itself, so it is checked as
			 * the counter moves rather than on the millisecond tick, which
			 * drifts off the counter's 1024 us boundaries and would miss it. */
			/* melonDS raises IRQ15 when the countdown to the next beacon
			 * reaches W_PreBeacon. Both sides are masked to the 8 us timer
			 * granularity, as there is no guarantee of an exact landing. */
			if (load16(model->registers, REG_US_COMPARE_CNT)) {
				uint32_t uspart = (uint32_t)(model->us_counter &
					UINT64_C(0x3ff));
				uint32_t beacon_us =
					((uint32_t)reg16(model, REG_BEACON_COUNT1) << 10) |
					(0x3ffu - uspart);
				if ((beacon_us & ~7u) ==
				    ((uint32_t)reg16(model, REG_PRE_BEACON) & ~7u))
				{
					model->pre_beacon_raises++;
					raise_pre_beacon_irq(model);
				}
			}
			if ((model->us_counter & UINT64_C(0x3f8)) == 0 &&
			    (load16(model->registers, REG_US_COMPARE_CNT) & 1) &&
			    (model->us_counter & ~UINT64_C(0x3ff)) == model->us_compare) {
				model->beacon_irq_blocked = 0;
				raise_beacon_irq(model, DWIFI_BEACON_IRQ_US_COMPARE);
			}
		}
		/* Beacon and post-beacon work is on the millisecond boundary, and the
		 * beacon itself is transmitted from the beacon interrupt rather than
		 * from a timer of the model's own. */
		if (model->mp_cmd_active &&
		    model->us_timestamp >= model->mp_reply_deadline_us) {
			/* Wall-clock transport latency is not part of emulated airtime. Give
			 * the peer a bounded chance exactly where hardware closes the reply
			 * window, then judge the client mask. */
			if (model->wait_for_peer)
				model->wait_for_peer(model->peer_wait_opaque);
			finish_mp_cmd(model);
		}
		if (model->mp_ack_active &&
		    model->us_timestamp >= model->mp_ack_deadline_us)
			finish_mp_ack(model);
		if (model->mp_reply_active &&
		    model->us_timestamp >= model->mp_reply_tx_deadline_us)
			finish_mp_reply(model);
		/* A transmission deferred because the air was busy is retried here.
		 * fire_tx() otherwise runs only from the guest's register writes, so
		 * the frame would simply be dropped rather than delayed. */
		if (model->tx_deferred &&
		    model->us_timestamp >= model->medium_busy_until_us)
			fire_tx(model);
		model->ms_remainder_us += WIFI_TIMER_INTERVAL_US;
		while (model->ms_remainder_us >= 1000) {
			model->ms_remainder_us -= 1000;
			advance_ms(model);
		}
	}
}

uint16_t dwifi_model_read16(struct dwifi_model *model, uint32_t address)
{
	if (!model)
		return 0;
	model->read_count++;

	uint32_t window = window_offset(address);
	if (window >= UINT32_C(0x2000) && window < UINT32_C(0x4000))
		return UINT16_C(0xffff);
	if (window >= PACKET_WINDOW_START && window < PACKET_WINDOW_END) {
		uint32_t offset = (window - PACKET_WINDOW_START) &
			(DWIFI_PACKET_RAM_SIZE - 1);
		offset &= ~UINT32_C(1);
		return load16(model->packet_ram, offset);
	}

	uint32_t offset = register_offset(address);
	if (offset == REG_RX_DATA_READ && window < DWIFI_REGISTER_SIZE) {
		uint16_t read_address = reg16(model, REG_RX_READ_ADDR);
		uint16_t value = load16(model->packet_ram, read_address & UINT16_C(0x1ffe));
		read_address = increment_rx_addr(model, read_address);
		if (read_address == reg16(model, REG_RX_GAP_ADDR))
			read_address = (uint16_t)(read_address +
				(reg16(model, REG_RX_GAP_SIZE) << 1));
		set_reg16(model, REG_RX_READ_ADDR, read_address & UINT16_C(0x1ffe));
		set_reg16(model, REG_RX_DATA_READ, value);
		uint16_t count = reg16(model, REG_RX_BUF_COUNT);
		if (count && --count == 0)
			dwifi_model_raise_irq(model, WIFI_IRQ_RX_COUNT);
		set_reg16(model, REG_RX_BUF_COUNT, count);
		return value;
	}
	if (offset >= REG_US_COUNT0 && offset <= REG_US_COUNT3)
		return timer_halfword(model->us_counter, offset, REG_US_COUNT0);
	if (offset >= REG_US_COMPARE0 && offset <= REG_US_COMPARE3)
		return timer_halfword(model->us_compare, offset, REG_US_COMPARE0);
	switch (offset) {
	case REG_MODE_RST:
		return own_radio_registers ?
			load16(model->registers, REG_MODE_RST) : 0;
	case REG_POWER_STATE:
		return own_radio_registers ?
			load16(model->registers, REG_POWER_STATE) :
			UINT16_C(0x0200);
	case REG_BB_BUSY:
	case REG_RF_BUSY:
		return 0;
	case REG_BB_READ: {
		uint16_t control = load16(model->registers, REG_BB_CNT);
		if ((control & UINT16_C(0x4000)) &&
		    ((control >> 12) & 3) == 2)
			return model->baseband[control & UINT16_C(0x007f)];
		return 0;
	}
	default:
		return load16(model->registers, offset);
	}
}

void dwifi_model_write16(struct dwifi_model *model, uint32_t address,
	uint16_t value)
{
	if (!model)
		return;
	model->write_count++;

	uint32_t window = window_offset(address);
	if (window >= PACKET_WINDOW_START && window < PACKET_WINDOW_END) {
		uint32_t offset = (window - PACKET_WINDOW_START) &
			(DWIFI_PACKET_RAM_SIZE - 1);
		offset &= ~UINT32_C(1);
		store16(model->packet_ram, offset, value);
		return;
	}

	uint32_t offset = register_offset(address);
	/* Transceiver power management, ported from melonDS. Auto power-down from
	 * IRQ13 is deliberately not wired up: this model never raises IRQ15, so a
	 * station that powered down on a beacon would have nothing to wake it. */
	if (offset == REG_MODE_RST) {
		uint16_t previous = reg16(model, REG_MODE_RST);
		set_reg16(model, REG_MODE_RST, value & 1);
		if ((previous & 1) != (value & 1))
			update_power_status(model, 0);
		return;
	}
	if (offset == REG_MODE_WEP) {
		set_reg16(model, REG_MODE_WEP, value & UINT16_C(0x007f));
		if (reg16(model, REG_POWER_TX) & (1 << 1)) {
			if ((value & 7) == 1)
				set_reg16(model, REG_POWER_DOWN_CTRL,
					(uint16_t)(reg16(model, REG_POWER_DOWN_CTRL) |
						(1 << 1)));
			else if ((value & 7) == 2)
				set_reg16(model, REG_POWER_DOWN_CTRL, 3);
			if ((value & 7) != 3)
				set_reg16(model, REG_POWER_STATE,
					(uint16_t)(reg16(model, REG_POWER_STATE) &
						UINT16_C(0x0300)));
			update_power_status(model, 0);
		}
		return;
	}
	if (offset == REG_POWER_TX) {
		set_reg16(model, REG_POWER_TX, value & UINT16_C(0x0003));
		if (value & (1 << 1)) {
			if ((reg16(model, REG_MODE_WEP) & 7) == 1)
				set_reg16(model, REG_POWER_DOWN_CTRL,
					(uint16_t)(reg16(model, REG_POWER_DOWN_CTRL) |
						(1 << 1)));
			else if ((reg16(model, REG_MODE_WEP) & 7) == 2)
				set_reg16(model, REG_POWER_DOWN_CTRL, 3);
			update_power_status(model, 0);
		}
		return;
	}
	if (offset == REG_POWER_STATE) {
		/* Only writable in power-management mode 3. */
		if ((reg16(model, REG_MODE_WEP) & 7) != 3)
			return;
		uint16_t next = (uint16_t)((reg16(model, REG_POWER_STATE) &
			UINT16_C(0x0300)) | (value & UINT16_C(0x0003)));
		if ((next & UINT16_C(0x0300)) == UINT16_C(0x0200))
			next &= (uint16_t)~UINT16_C(0x0001);
		else
			next &= (uint16_t)~UINT16_C(0x0002);
		if (!(next & (1 << 9)))
			next &= (uint16_t)~UINT16_C(0x0100);
		set_reg16(model, REG_POWER_STATE, next);
		update_power_status(model, 0);
		return;
	}
	if (offset == REG_POWER_FORCE) {
		set_reg16(model, REG_POWER_FORCE, value & UINT16_C(0x8001));
		update_power_status(model, 0);
		return;
	}
	if (offset == REG_POWER_DOWN_CTRL) {
		set_reg16(model, REG_POWER_DOWN_CTRL, value & UINT16_C(0x0003));
		if (reg16(model, REG_POWER_TX) & (1 << 1)) {
			if ((reg16(model, REG_MODE_WEP) & 7) == 1)
				set_reg16(model, REG_POWER_DOWN_CTRL,
					(uint16_t)(reg16(model, REG_POWER_DOWN_CTRL) |
						(1 << 1)));
			else if ((reg16(model, REG_MODE_WEP) & 7) == 2)
				set_reg16(model, REG_POWER_DOWN_CTRL, 3);
		}
		update_power_status(model, 0);
		return;
	}
	if (offset == REG_TX_SLOT_BEACON) {
		set_reg16(model, offset, value);
		return;
	}
	if (offset == REG_TX_SLOT_CMD || offset == REG_TX_SLOT_LOC1 ||
	    offset == REG_TX_SLOT_LOC2 || offset == REG_TX_SLOT_LOC3) {
		set_reg16(model, offset, value);
		if (value & UINT16_C(0x8000))
			model->tx_slot_armed[tx_slot_index(offset)] = 1;
		fire_tx(model);
		return;
	}
	if (offset == REG_RX_CNT) {
		if (value & 1)
			set_reg16(model, REG_RX_WRITE_CURSOR,
				reg16(model, REG_RX_WRITE_ADDR));
		if (value & UINT16_C(0x0080)) {
			set_reg16(model, REG_TX_SLOT_REPLY2,
				reg16(model, REG_TX_SLOT_REPLY1));
			set_reg16(model, REG_TX_SLOT_REPLY1, 0);
		}
		set_reg16(model, offset, value & UINT16_C(0xff0e));
		if (value & UINT16_C(0x8000))
			fire_tx(model);
		return;
	}
	if (offset == REG_TX_DATA_WRITE) {
		uint16_t address = reg16(model, REG_TX_WRITE_ADDR) & UINT16_C(0x1ffe);
		store16(model->packet_ram, address, value);
		address = (uint16_t)((address + 2) & UINT16_C(0x1ffe));
		if (address == reg16(model, REG_TX_GAP_ADDR))
			address = (uint16_t)(address + (reg16(model, REG_TX_GAP_SIZE) << 1));
		set_reg16(model, REG_TX_WRITE_ADDR, address & UINT16_C(0x1ffe));
		uint16_t count = reg16(model, REG_TX_BUF_COUNT);
		if (count && --count == 0)
			dwifi_model_raise_irq(model, WIFI_IRQ_TX_COUNT);
		set_reg16(model, REG_TX_BUF_COUNT, count);
		return;
	}
	if (offset == REG_TX_REQ_RESET) {
		set_reg16(model, REG_TX_REQ_READ,
			(uint16_t)(reg16(model, REG_TX_REQ_READ) & ~value));
		return;
	}
	if (offset == REG_TX_REQ_SET) {
		set_reg16(model, REG_TX_REQ_READ,
			(uint16_t)(reg16(model, REG_TX_REQ_READ) | value));
		fire_tx(model);
		return;
	}
	if (offset == REG_TX_SLOT_RESET) {
		if (value & 1) set_reg16(model, REG_TX_SLOT_LOC1,
			reg16(model, REG_TX_SLOT_LOC1) & UINT16_C(0x7fff));
		if (value & 2) set_reg16(model, REG_TX_SLOT_CMD,
			reg16(model, REG_TX_SLOT_CMD) & UINT16_C(0x7fff));
		if (value & 4) set_reg16(model, REG_TX_SLOT_LOC2,
			reg16(model, REG_TX_SLOT_LOC2) & UINT16_C(0x7fff));
		if (value & 8) set_reg16(model, REG_TX_SLOT_LOC3,
			reg16(model, REG_TX_SLOT_LOC3) & UINT16_C(0x7fff));
		if (value & 1) model->tx_slot_armed[3] = 0;
		if (value & 2) model->tx_slot_armed[2] = 0;
		if (value & 4) model->tx_slot_armed[1] = 0;
		if (value & 8) model->tx_slot_armed[0] = 0;
		return;
	}
	if (offset == REG_US_COUNT_CNT) {
		store16(model->registers, offset, value & UINT16_C(1));
		return;
	}
	if (offset == REG_US_COMPARE_CNT) {
		/* The forced interrupt is taken before the new enable bit lands, so a
		 * write of 2 alone still raises it while the control is enabled. */
		if (value & UINT16_C(2))
			raise_beacon_irq(model, DWIFI_BEACON_IRQ_FORCED);
		store16(model->registers, offset, value & UINT16_C(1));
		return;
	}
	if (offset >= REG_US_COUNT0 && offset <= REG_US_COUNT3) {
		set_timer_halfword(&model->us_counter, offset, REG_US_COUNT0, value);
		return;
	}
	if (offset >= REG_US_COMPARE0 && offset <= REG_US_COMPARE3) {
		if (offset == REG_US_COMPARE0) {
			/* Bit 0 is a write-only control, not part of the compare value:
			 * it blocks the beacon-count interrupt until W_US_COUNT reaches
			 * W_US_COMPARE. Mario Kart's host programs 0xd001, so the low bit
			 * must be taken as the gate before the value is masked. */
			model->beacon_irq_blocked = (uint8_t)(value & 1);
			value &= UINT16_C(0xfc00);
		}
		set_timer_halfword(&model->us_compare, offset, REG_US_COMPARE0, value);
		return;
	}
	if (offset == REG_W_IE) {
		store16(model->registers, offset, value);
		update_irq(model);
		return;
	}
	if (offset == REG_W_IF) {
		uint16_t flags = load16(model->registers, offset);
		store16(model->registers, offset, (uint16_t)(flags & ~value));
		update_irq(model);
		return;
	}
	if (offset == REG_W_IF_SET) {
		uint16_t flags = load16(model->registers, REG_W_IF);
		flags |= value & UINT16_C(0xfbff);
		store16(model->registers, REG_W_IF, flags);
		update_irq(model);
		return;
	}
	if (offset == REG_BB_CNT && ((value >> 12) & 3) == 1) {
		uint16_t data = load16(model->registers, REG_BB_WRITE);
		model->baseband[value & UINT16_C(0x007f)] = (uint8_t)data;
	}
	store16(model->registers, offset, value);
}
