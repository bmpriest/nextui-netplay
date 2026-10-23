/*
 * A minimal libretro core, used only to verify that the shim forwards
 * correctly in both directions. Every entry point records that it was reached;
 * retro_run exercises the frontend callbacks so the reverse path is covered too.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libretro.h"
#ifdef FAKE_DUAL
#include "gambatte_dual.h"
#endif

#define HIT(name) printf("core:%s\n", name)

static retro_environment_t        cb_environment;
static retro_video_refresh_t      cb_video_refresh;
static retro_audio_sample_t       cb_audio_sample;
static retro_audio_sample_batch_t cb_audio_sample_batch;
static retro_input_poll_t         cb_input_poll;
static retro_input_state_t        cb_input_state;

static unsigned char state_blob[16];
static unsigned char save_ram[16];
static unsigned char rtc_ram[8];
static unsigned core_runs;
#ifdef FAKE_DUAL
static unsigned dual_visible;
static unsigned char dual_save_ram[2][16];
static unsigned char dual_rtc_ram[2][8];
static unsigned char dual_state[2][16];
#endif

//////////////////////////////////////////////////////////////////////////////
// netpacket - what gpsp/gambatte use for link play
//////////////////////////////////////////////////////////////////////////////

static retro_netpacket_send_t         np_send;
static uint16_t                       np_local_id;
static int                            np_started;
static unsigned                       np_seq;

static retro_netpacket_poll_receive_t np_poll_receive;
static int np_in_receive;

static void np_start(uint16_t client_id, retro_netpacket_send_t send_fn,
                     retro_netpacket_poll_receive_t poll_fn) {
	printf("core:np_start id=%u\n", client_id);
	np_send = send_fn;
	np_poll_receive = poll_fn;
	np_local_id = client_id;
	np_started = 1;
}

static void np_receive(const void* buf, size_t len, uint16_t client_id) {
	printf("core:np_recv from=%u len=%zu data=%.*s\n",
	       client_id, len, (int)len, (const char*)buf);
	/* A core is allowed to poll from inside its own receive handler. The shim
	 * must make that a no-op rather than recursing through this function with
	 * the next packet. */
	if (getenv("FAKE_CORE_POLL_IN_RECEIVE") && np_poll_receive && !np_in_receive) {
		np_in_receive = 1;
		np_poll_receive();
		np_in_receive = 0;
		printf("core:np_reentrant_poll_returned\n");
	}
}

static void np_stop(void) { printf("core:np_stop\n"); np_started = 0; np_send = NULL; }
static void np_poll(void) { }
static bool np_connected(uint16_t id)    { printf("core:np_connected id=%u\n", id); return true; }
static void np_disconnected(uint16_t id) { printf("core:np_disconnected id=%u\n", id); }

static const struct retro_netpacket_callback np_callback = {
	np_start, np_receive, np_stop, np_poll, np_connected, np_disconnected, "faketest1"
};

void retro_set_environment(retro_environment_t cb) {
	HIT("set_environment");
	cb_environment = cb;
	// Real cores probe the frontend from inside set_environment; do the same so
	// the environment path is exercised.
	unsigned quirk = 0;
	cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &quirk);

	// Link-capable cores register here. A stock frontend returns false and the
	// core disables link; the shim answers true instead.
	// FAKE_CORE_NO_NETPACKET models gambatte, which carries its own link traffic
	// over a socket it opens itself and never asks the frontend for netpacket.
	if (getenv("FAKE_CORE_NO_NETPACKET"))
		printf("core:netpacket_skipped\n");
	else if (cb(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE, (void*)&np_callback))
		printf("core:netpacket_accepted\n");
	else
		printf("core:netpacket_refused\n");
}

void retro_set_video_refresh(retro_video_refresh_t cb)           { HIT("set_video_refresh");      cb_video_refresh = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)             { HIT("set_audio_sample");       cb_audio_sample = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { HIT("set_audio_sample_batch"); cb_audio_sample_batch = cb; }
void retro_set_input_poll(retro_input_poll_t cb)                 { HIT("set_input_poll");         cb_input_poll = cb; }
void retro_set_input_state(retro_input_state_t cb)               { HIT("set_input_state");        cb_input_state = cb; }

void retro_init(void) {
	HIT("init");
	memset(state_blob, 0xAB, sizeof(state_blob));
	memset(save_ram, getenv("FAKE_SRAM_BYTE") ? atoi(getenv("FAKE_SRAM_BYTE")) : 0x51,
	       sizeof(save_ram));
	memset(rtc_ram, getenv("FAKE_RTC_BYTE") ? atoi(getenv("FAKE_RTC_BYTE")) : 0x52,
	       sizeof(rtc_ram));
	core_runs = 0;
#ifdef FAKE_DUAL
	memset(dual_state, 0xAB, sizeof(dual_state));
	memset(dual_save_ram, 0, sizeof(dual_save_ram));
	memset(dual_rtc_ram, 0, sizeof(dual_rtc_ram));
	memset(dual_save_ram[dual_visible],
	       getenv("FAKE_SRAM_BYTE") ? atoi(getenv("FAKE_SRAM_BYTE")) : 0x51,
	       sizeof(dual_save_ram[dual_visible]));
	memset(dual_rtc_ram[dual_visible],
	       getenv("FAKE_RTC_BYTE") ? atoi(getenv("FAKE_RTC_BYTE")) : 0x52,
	       sizeof(dual_rtc_ram[dual_visible]));
#endif
	// Report what the frontend (or the shim) gives us for a core option, the way
	// gpSP reads gpsp_serial to decide its link mode.
	struct retro_variable var = { "gpsp_serial", NULL };
	if (cb_environment(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		printf("core:option gpsp_serial=%s\n", var.value);
	else
		printf("core:option gpsp_serial=<unset>\n");
	if (getenv("FAKE_CORE_NAME") && strstr(getenv("FAKE_CORE_NAME"), "PCSX")) {
		const char* keys[] = { "pcsx_rearmed_memcard1", "pcsx_rearmed_memcard2" };
		for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			struct retro_variable pc = { keys[i], NULL };
			if (cb_environment(RETRO_ENVIRONMENT_GET_VARIABLE, &pc) && pc.value)
				printf("core:option %s=%s\n", keys[i], pc.value);
			else
				printf("core:option %s=<unset>\n", keys[i]);
		}
		const char* save_dir = NULL;
		if (cb_environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir)
			printf("core:save_directory=%s\n", save_dir);
	}
}
void retro_deinit(void) { HIT("deinit"); }

unsigned retro_api_version(void) { HIT("api_version"); return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info* info) {
	HIT("get_system_info");
	memset(info, 0, sizeof(*info));
	// The shim picks shared-screen versus link-cable off this name, so tests
	// need to be able to pose as a link core.
	const char* name       = getenv("FAKE_CORE_NAME");
	info->library_name     = (name && name[0]) ? name : "FakeCore";
	info->library_version  = "9.9";
	info->valid_extensions = "fake|bin";
	info->need_fullpath    = true;
	info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	HIT("get_system_av_info");
	memset(info, 0, sizeof(*info));
	info->geometry.base_width   = 240;
	info->geometry.base_height  = 160;
	info->geometry.max_width    = 240;
	info->geometry.max_height   = 160;
	info->geometry.aspect_ratio = 1.5f;
	info->timing.fps            = 59.73;
	info->timing.sample_rate    = 44100.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device) { HIT("set_controller_port_device"); }
void retro_reset(void) {
	HIT("reset");
	memset(state_blob, 0xC3, sizeof(state_blob));
}

void retro_run(void) {
	HIT("run");
	state_blob[1]++;
	const char* corrupt_at = getenv("FAKE_CORE_CORRUPT_AT");
	if (corrupt_at && core_runs == (unsigned)atoi(corrupt_at)) {
		state_blob[2] ^= 0x5A;
		printf("core:state_corrupted run=%u\n", core_runs);
	}
#ifdef FAKE_DUAL
	/* One replica quietly stops matching the other, the way a wall-clock
	 * dependent serial coordinator does on real hardware. Only the paired
	 * state changes; nothing tells the shim. */
	const char* diverge_at = getenv("FAKE_DUAL_DIVERGE_AT");
	if (diverge_at && core_runs == (unsigned)atoi(diverge_at)) {
		dual_state[0][3] ^= 0x5A;
		printf("core:dual_diverged run=%u\n", core_runs);
	}
#endif
	core_runs++;

	// FAKE_CORE_BLOCK_AT/_MS reproduce a link-capable core blocking inside
	// retro_run while it waits on its peer, as gambatte's NetSerial does.
	{
		static int run_count = 0;
		static int block_at = -1, block_ms = 0;
		if (block_at == -2) { /* parsed */ }
		else if (block_at == -1) {
			block_at = getenv("FAKE_CORE_BLOCK_AT") ? atoi(getenv("FAKE_CORE_BLOCK_AT")) : -3;
			block_ms = getenv("FAKE_CORE_BLOCK_MS") ? atoi(getenv("FAKE_CORE_BLOCK_MS")) : 0;
		}
		if (run_count++ == block_at) {
			printf("core:block_begin\n"); fflush(stdout);
			usleep(block_ms * 1000);
			printf("core:block_end\n"); fflush(stdout);
		}
	}

	// Once linked, emit one identifiable packet per frame so the peer's log
	// proves data crossed the wire.
	/* FAKE_CORE_MIDFRAME_POLL models a link core doing a serial exchange inside
	 * one retro_run: put a byte on the wire, then ask to read rather than
	 * returning to the frontend and waiting out the rest of the frame. */
	if (np_started && np_send && getenv("FAKE_CORE_MIDFRAME_POLL")) {
		char msg[32];
		int n = snprintf(msg, sizeof(msg), "mid%u", np_seq++);
		np_send(RETRO_NETPACKET_RELIABLE, msg, (size_t)n, 0xFFFF);
		if (np_poll_receive) {
			printf("core:midframe_poll seq=%u\n", np_seq);
			np_poll_receive();
		}
	}

	// FAKE_CORE_QUIET simulates a core that has registered the interface but is
	// not transmitting - a game sitting in a menu, or waiting on the other
	// player. The peer must keep it alive with heartbeats regardless.
	static int quiet = -1;
	if (quiet < 0) quiet = getenv("FAKE_CORE_QUIET") != NULL;

	if (np_started && np_send && !quiet) {
		char msg[32];
		int n = snprintf(msg, sizeof(msg), "P%u-%u", np_local_id, np_seq++);
		np_send(RETRO_NETPACKET_RELIABLE, msg, (size_t)n, RETRO_NETPACKET_BROADCAST);
	}
	cb_input_poll();
	// The value the frontend returns has to survive the trip back through the
	// shim's input_state wrapper.
	printf("core:input_state=%d\n", (int)cb_input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A));

	// Shared-screen netplay must serve BOTH ports. Stock minarch returns 0 for
	// anything above port 0, so a non-zero p1 can only have come from the peer.
	{
		static int shown = 0;
		static unsigned input_sum = 2166136261u;
		int p0 = cb_input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
		int p1 = cb_input_state(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
		if (shown < 400) printf("core:ports p0=%d p1=%d\n", p0, p1);
		/* FNV over the input stream: identical only if both peers ran the same
		 * frames with the same inputs, in the same order. */
		input_sum ^= (unsigned)p0; input_sum *= 16777619u;
		input_sum ^= (unsigned)p1; input_sum *= 16777619u;
		if (++shown % 100 == 0) printf("core:inputsum frames=%d sum=%08x\n", shown, input_sum);
	}
	if (core_runs <= 3)
		printf("core:persistent sram=%u rtc=%u\n", save_ram[0], rtc_ram[0]);

	static const int16_t frame[240 * 160];
	cb_video_refresh(frame, 240, 160, 240 * sizeof(int16_t));

	static const int16_t audio[64];
	cb_audio_sample_batch(audio, 32);
	cb_audio_sample(0, 0);
}

size_t retro_serialize_size(void) { HIT("serialize_size"); return sizeof(state_blob); }

bool retro_serialize(void* data, size_t size) {
	HIT("serialize");
	if (size < sizeof(state_blob)) return false;
	memcpy(data, state_blob, sizeof(state_blob));
	return true;
}

bool retro_unserialize(const void* data, size_t size) {
	HIT("unserialize");
	if (size < sizeof(state_blob)) return false;
	memcpy(state_blob, data, sizeof(state_blob));
	/* Both the initialized and post-reset machine states are states produced by
	 * this core. Rejecting the latter made the host-authoritative reset test
	 * model a core that could not load its own serialization. */
	return state_blob[0] == 0xAB || state_blob[0] == 0xC3;
}

void retro_cheat_reset(void) { HIT("cheat_reset"); }
void retro_cheat_set(unsigned index, bool enabled, const char* code) { HIT("cheat_set"); }

bool retro_load_game(const struct retro_game_info* game) { HIT("load_game"); return true; }
bool retro_load_game_special(unsigned t, const struct retro_game_info* i, size_t n) { HIT("load_game_special"); return false; }
void retro_unload_game(void) { HIT("unload_game"); }

unsigned retro_get_region(void) { HIT("get_region"); return RETRO_REGION_NTSC; }
void* retro_get_memory_data(unsigned id) {
	HIT("get_memory_data");
#ifdef FAKE_DUAL
	if (id == RETRO_MEMORY_SAVE_RAM) return dual_save_ram[dual_visible];
	if (id == RETRO_MEMORY_RTC) return dual_rtc_ram[dual_visible];
#endif
	if (id == RETRO_MEMORY_SAVE_RAM) return save_ram;
	if (id == RETRO_MEMORY_RTC) return rtc_ram;
	return NULL;
}
size_t retro_get_memory_size(unsigned id) {
	HIT("get_memory_size");
	/* PicoDrive reports no save RAM in some states and 16 KiB in others for
	 * the same game, so one run's checkpoint can disagree with the next. */
	if (id == RETRO_MEMORY_SAVE_RAM) return getenv("FAKE_NO_SAVE_RAM") ? 0 : sizeof(save_ram);
	if (id == RETRO_MEMORY_RTC) return sizeof(rtc_ram);
	return 0;
}

#ifdef FAKE_DUAL
unsigned retro_dual_get_abi_version(void) { return GAMBATTE_DUAL_ABI_VERSION; }
uint64_t retro_dual_get_capabilities(void) {
	return GAMBATTE_DUAL_CAP_TWO_CONTENTS | GAMBATTE_DUAL_CAP_CONSOLE_MEMORY |
	       GAMBATTE_DUAL_CAP_VISIBLE_CONSOLE | GAMBATTE_DUAL_CAP_PAIRED_CHECKPOINT |
	       GAMBATTE_DUAL_CAP_TARGETED_RESET | GAMBATTE_DUAL_CAP_CLOCK_EPOCHS;
}
/* Folded into the paired state, exactly as the real core folds the epochs into
 * its checkpoint header. The two devices' clocks differ, so if the shim ever
 * handed each side its own reading instead of the agreed pair, the states would
 * no longer hash equal and the link test would say so. */
bool retro_dual_set_clock_epochs(uint64_t console_a, uint64_t console_b) {
	for (unsigned i = 0; i < 8; i++) {
		dual_state[0][i] = (unsigned char)(console_a >> (i * 8));
		dual_state[1][i] = (unsigned char)(console_b >> (i * 8));
	}
	return true;
}
bool retro_dual_set_visible_console(unsigned console) {
	if (console > GAMBATTE_DUAL_CONSOLE_B) return false;
	dual_visible = console;
	return true;
}
unsigned retro_dual_get_visible_console(void) { return dual_visible; }
void* retro_dual_get_memory_data(unsigned console, unsigned id) {
	if (console > GAMBATTE_DUAL_CONSOLE_B) return NULL;
	if (id == RETRO_MEMORY_SAVE_RAM) return dual_save_ram[console];
	if (id == RETRO_MEMORY_RTC) return dual_rtc_ram[console];
	return NULL;
}
size_t retro_dual_get_memory_size(unsigned console, unsigned id) {
	if (console > GAMBATTE_DUAL_CONSOLE_B) return 0;
	if (id == RETRO_MEMORY_SAVE_RAM) return sizeof(dual_save_ram[console]);
	if (id == RETRO_MEMORY_RTC) return sizeof(dual_rtc_ram[console]);
	return 0;
}
bool retro_dual_reset_console(unsigned console) {
	if (console > GAMBATTE_DUAL_CONSOLE_B) return false;
	memset(dual_state[console], 0xC3, sizeof(dual_state[console]));
	return true;
}
bool retro_dual_is_checkpoint_safe(void) { return true; }
/* Real gambatte's saveState is not a pure read - CPU::saveState rebases the
 * cycle counter, so serializing changes what the next serialize produces, and
 * stateSize() runs the same path. A fake core that reads cleanly cannot catch a
 * protocol which serializes a different number of times on each device, which
 * is precisely the bug that shipped: the host serialized twice (checkpoint,
 * then hash) and the guest once, so the two replicas differed at frame 0 with
 * nothing emulated. Model the mutation. */
size_t retro_dual_serialize_size(void) {
	dual_state[0][15]++;   /* the size query runs the same path, so it mutates too */
	return sizeof(dual_state);
}

/* Write the bytes, *then* move on. That ordering is the whole hazard: the
 * payload describes the state the device was in, while the device is left
 * somewhere else. A peer that adopts the payload therefore lands where the
 * sender no longer is, and the two disagree from that moment - which is only
 * visible to a protocol that performs the same operations on both sides. */
bool retro_dual_serialize(void* data, size_t size) {
	if (size != sizeof(dual_state)) return false;
	memcpy(data, dual_state, size);
	dual_state[0][14]++;
	return true;
}
bool retro_dual_unserialize(const void* data, size_t size) {
	if (size != sizeof(dual_state)) return false;
	memcpy(dual_state, data, size);
	return true;
}
#endif
