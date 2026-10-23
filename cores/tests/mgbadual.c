/* ABI-level host test for the mGBA paired libretro frontend. */
#include "libretro.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_SIZE (32 * 1024)
#define ROM_ENTRY 0xC0
#define DUAL_SUBSYSTEM_ID 0x47424c43u
#define REQUIRED_CAPS 0x3fu

static unsigned videos;
static unsigned audio_frames;
static uint64_t audio_energy;
static unsigned polls;
static unsigned visible_width;
static unsigned visible_height;

static bool environment(unsigned cmd, void* data) {
	(void) data;
	return cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT;
}

static void video(const void* data, unsigned width, unsigned height, size_t pitch) {
	(void) pitch;
	if (data) {
		++videos;
		visible_width = width;
		visible_height = height;
	}
}

static size_t audio(const int16_t* data, size_t frames) {
	size_t i;
	for (i = 0; i < frames * 2; ++i) {
		int sample = data[i];
		audio_energy += sample < 0 ? (uint64_t) -sample : (uint64_t) sample;
	}
	audio_frames += (unsigned) frames;
	return frames;
}

static void poll(void) {
	++polls;
}

static int16_t input(unsigned port, unsigned device, unsigned index, unsigned id) {
	(void) device;
	(void) index;
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
		return port ? (1 << RETRO_DEVICE_ID_JOYPAD_B)
		            : (1 << RETRO_DEVICE_ID_JOYPAD_A);
	}
	return port ? id == RETRO_DEVICE_ID_JOYPAD_B
	            : id == RETRO_DEVICE_ID_JOYPAD_A;
}

static void build_rom(uint8_t* rom) {
	memset(rom, 0, ROM_SIZE);
	uint32_t branch = 0xEA000000 | (((ROM_ENTRY - 8) / 4) & 0x00FFFFFF);
	memcpy(rom, &branch, sizeof(branch));
	rom[0xB2] = 0x96;

	/* Enable PSG channel 1, then select multiplayer and have ID 0 transfer. */
	static const uint32_t entry[] = {
		0xE3A00404,             /* mov r0, #0x04000000 */
		0xE3A01080, 0xE5C01084, /* master sound enable */
		0xE3A01077, 0xE5C01080, /* PSG L/R volumes */
		0xE3A01011, 0xE5C01081, /* route channel 1 to L/R */
		0xE3A01080, 0xE5C01062, /* square-wave duty */
		0xE3A010F0, 0xE5C01063, /* maximum envelope volume */
		0xE3A01000, 0xE5C01064,
		0xE3A01084, 0xE5C01065, /* frequency and restart */
		0xE3A02801, 0xE2522001, 0x1AFFFFFD, /* let it sound briefly */
		0xE3A01000, 0xE5C01084, /* then silence before checkpoint tests */
		0xE2800C01, 0xE3A01000, 0xE1C013B4,
		0xE3A01C20, 0xE1C012B8, 0xE3A02C01, 0xE2522001,
		0x1AFFFFFD, 0xE1D012B8, 0xE2112030, 0x1A000005,
		0xE3811080, 0xE1C012B8, 0xE1D012B8, 0xE3110080,
		0x1AFFFFFC, 0xEAFFFFF6, 0xE1D012B8, 0xEAFFFFFD,
	};
	memcpy(rom + ROM_ENTRY, entry, sizeof(entry));
}

static uint64_t hash_bytes(const void* data, size_t size) {
	const uint8_t* bytes = data;
	uint64_t hash = UINT64_C(0xcbf29ce484222325);
	while (size--) {
		hash ^= *bytes++;
		hash *= UINT64_C(0x100000001b3);
	}
	return hash;
}

#define LOAD(name) do { \
	*(void**) (&name) = dlsym(handle, #name); \
	if (!(name)) { fprintf(stderr, "missing %s\n", #name); return 1; } \
} while (0)

int main(int argc, char** argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: mgbadual /path/mgba_dual_libretro.so\n");
		return 2;
	}
	void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 1;
	}

	void (*retro_set_environment)(retro_environment_t);
	void (*retro_set_video_refresh)(retro_video_refresh_t);
	void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*retro_set_input_poll)(retro_input_poll_t);
	void (*retro_set_input_state)(retro_input_state_t);
	void (*retro_init)(void);
	void (*retro_deinit)(void);
	void (*retro_run)(void);
	void (*retro_unload_game)(void);
	void (*retro_get_system_av_info)(struct retro_system_av_info*);
	bool (*retro_load_game_special)(unsigned, const struct retro_game_info*, size_t);
	unsigned (*retro_dual_get_abi_version)(void);
	uint64_t (*retro_dual_get_capabilities)(void);
	bool (*retro_dual_set_visible_console)(unsigned);
	unsigned (*retro_dual_get_visible_console)(void);
	void* (*retro_dual_get_memory_data)(unsigned, unsigned);
	size_t (*retro_dual_get_memory_size)(unsigned, unsigned);
	bool (*retro_dual_reset_console)(unsigned);
	bool (*retro_dual_is_checkpoint_safe)(void);
	size_t (*retro_dual_serialize_size)(void);
	bool (*retro_dual_serialize)(void*, size_t);
	bool (*retro_dual_unserialize)(const void*, size_t);
	bool (*retro_dual_set_clock_epochs)(uint64_t, uint64_t);

	LOAD(retro_set_environment);
	LOAD(retro_set_video_refresh);
	LOAD(retro_set_audio_sample_batch);
	LOAD(retro_set_input_poll);
	LOAD(retro_set_input_state);
	LOAD(retro_init);
	LOAD(retro_deinit);
	LOAD(retro_run);
	LOAD(retro_unload_game);
	LOAD(retro_get_system_av_info);
	LOAD(retro_load_game_special);
	LOAD(retro_dual_get_abi_version);
	LOAD(retro_dual_get_capabilities);
	LOAD(retro_dual_set_visible_console);
	LOAD(retro_dual_get_visible_console);
	LOAD(retro_dual_get_memory_data);
	LOAD(retro_dual_get_memory_size);
	LOAD(retro_dual_reset_console);
	LOAD(retro_dual_is_checkpoint_safe);
	LOAD(retro_dual_serialize_size);
	LOAD(retro_dual_serialize);
	LOAD(retro_dual_unserialize);
	LOAD(retro_dual_set_clock_epochs);

	if (retro_dual_get_abi_version() != 2 ||
	    (retro_dual_get_capabilities() & REQUIRED_CAPS) != REQUIRED_CAPS) {
		fprintf(stderr, "dual ABI/capabilities mismatch\n");
		return 1;
	}
	retro_set_environment(environment);
	retro_set_video_refresh(video);
	retro_set_audio_sample_batch(audio);
	retro_set_input_poll(poll);
	retro_set_input_state(input);
	retro_init();
	if (!retro_dual_set_clock_epochs(1700000000, 1700003600)) {
		fprintf(stderr, "clock epoch setup failed\n");
		return 1;
	}

	uint8_t* rom_a = malloc(ROM_SIZE);
	uint8_t* rom_b = malloc(ROM_SIZE);
	build_rom(rom_a);
	build_rom(rom_b);
	rom_b[0xA0] = 1; /* different content, still a valid synthetic GBA ROM */
	struct retro_game_info games[2];
	memset(games, 0, sizeof(games));
	games[0].data = rom_a;
	games[0].size = ROM_SIZE;
	games[1].data = rom_b;
	games[1].size = ROM_SIZE;
	if (!retro_load_game_special(DUAL_SUBSYSTEM_ID, games, 2)) {
		fprintf(stderr, "two-content load failed\n");
		return 1;
	}
	free(rom_a);
	free(rom_b);
	struct retro_system_av_info av;
	retro_get_system_av_info(&av);
	if (av.timing.sample_rate != 65536.0) {
		fprintf(stderr, "unexpected fixed audio rate: %.2f\n", av.timing.sample_rate);
		return 1;
	}

	if (!retro_dual_set_visible_console(1) ||
	    retro_dual_get_visible_console() != 1 ||
	    retro_dual_set_visible_console(2)) {
		fprintf(stderr, "visible-console contract failed\n");
		return 1;
	}
	unsigned i;
	for (i = 0; i < 120; ++i) {
		retro_run();
	}
	if (videos != 120 || polls != 120 || visible_width != 240 || visible_height != 160) {
		fprintf(stderr, "frontend callbacks wrong: video=%u poll=%u size=%ux%u\n",
		        videos, polls, visible_width, visible_height);
		return 1;
	}
	if (!audio_frames || !audio_energy) {
		fprintf(stderr, "silent audio callback: frames=%u energy=%llu\n",
		        audio_frames, (unsigned long long) audio_energy);
		return 1;
	}
	if (!retro_dual_get_memory_data(0, RETRO_MEMORY_SAVE_RAM) ||
	    !retro_dual_get_memory_data(1, RETRO_MEMORY_SAVE_RAM) ||
	    !retro_dual_get_memory_size(0, RETRO_MEMORY_SAVE_RAM) ||
	    !retro_dual_get_memory_size(1, RETRO_MEMORY_SAVE_RAM)) {
		fprintf(stderr, "per-console save memory unavailable\n");
		return 1;
	}
	if (!retro_dual_is_checkpoint_safe()) {
		fprintf(stderr, "frame boundary is not checkpoint-safe\n");
		return 1;
	}
	size_t state_size = retro_dual_serialize_size();
	uint8_t* before = malloc(state_size);
	uint8_t* after = malloc(state_size);
	if (!state_size || !before || !after ||
	    !retro_dual_serialize(before, state_size)) {
		fprintf(stderr, "paired checkpoint capture failed\n");
		return 1;
	}
	for (i = 0; i < 5; ++i) {
		retro_run();
	}
	if (!retro_dual_unserialize(before, state_size) ||
	    !retro_dual_serialize(after, state_size) ||
	    memcmp(before, after, state_size)) {
		fprintf(stderr, "paired checkpoint round-trip diverged\n");
		return 1;
	}
	if (!retro_dual_reset_console(0)) {
		fprintf(stderr, "targeted reset contract failed\n");
		return 1;
	}
	retro_run();
	retro_run();
	if (!retro_dual_is_checkpoint_safe() || !retro_dual_reset_console(1)) {
		fprintf(stderr, "console A reset did not recover to a frame boundary\n");
		return 1;
	}
	retro_run();
	retro_run();
	if (!retro_dual_is_checkpoint_safe() || !retro_dual_reset_console(2)) {
		fprintf(stderr, "console B reset did not recover to a frame boundary\n");
		return 1;
	}
	retro_run();
	retro_run();
	if (!retro_dual_is_checkpoint_safe() || retro_dual_reset_console(3)) {
		fprintf(stderr, "paired reset did not recover to a frame boundary\n");
		return 1;
	}

	printf("  ok   ABI v2 exposes all required capabilities\n");
	printf("  ok   120 paired frames presented console B (%u audio frames, energy=%llu)\n",
	       audio_frames, (unsigned long long) audio_energy);
	printf("  ok   per-console memory and visible-console selection work\n");
	printf("  ok   %zu-byte paired checkpoint round-trips exactly (state=%016llx)\n",
	       state_size, (unsigned long long) hash_bytes(before, state_size));
	printf("  ok   clock epochs and targeted resets recover to checkpoint-safe frames\n");

	free(before);
	free(after);
	retro_unload_game();
	retro_deinit();
	dlclose(handle);
	return 0;
}
