/*
 * Stands in for minarch: loads the shim exactly the way ma_core.c Core_open
 * does - same dlsym list, same call order - and checks that what comes back
 * out the far side is the real core's, unaltered.
 *
 * The order matters. minarch calls retro_get_system_info *before* any of the
 * retro_set_* registration functions, which is why the shim loads lazily.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include "libretro.h"

static int failures = 0;

#define CHECK(cond, ...)                                    \
	do {                                                    \
		if (!(cond)) {                                      \
			printf("FAIL: ");                               \
			printf(__VA_ARGS__);                            \
			printf("\n");                                   \
			failures++;                                     \
		}                                                   \
	} while (0)

static int env_calls = 0;
/* HARNESS_OPTIONS_LATER at frame HARNESS_OPTIONS_LATER_AT: the player changes
 * options in the menu, and the next update poll says so. */
static int harness_frame;
static int options_changed_pending;
static const char* harness_options(void) {
	const char* later = getenv("HARNESS_OPTIONS_LATER");
	const char* at = getenv("HARNESS_OPTIONS_LATER_AT");
	if (later && at && harness_frame >= atoi(at)) return later;
	return getenv("HARNESS_OPTIONS");
}
static bool fe_environment(unsigned cmd, void* data) {
	// Keep the trace bounded: a real core queries this constantly, and printing
	// every call would dominate a benchmark.
	if (++env_calls <= 8) printf("fe:environment cmd=%u\n", cmd);

	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:            // gambatte refuses to run without it
		if (data) *(bool*)data = true;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		if (data) *(const char**)data = "/tmp";
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		/* HARNESS_OPTIONS="key=value;key=value": this frontend's core options. */
		struct retro_variable* var = data;
		const char* all = harness_options();
		if (!var || !var->key || !all) return false;
		static char found[16][64];
		static int next;
		size_t klen = strlen(var->key);
		for (const char* p = all; *p; ) {
			const char* end = strchr(p, ';');
			size_t len = end ? (size_t)(end - p) : strlen(p);
			if (len > klen && p[klen] == '=' && !strncmp(p, var->key, klen) &&
			    len - klen - 1 < sizeof(found[0])) {
				char* slot = found[next++ % 16];
				memcpy(slot, p + klen + 1, len - klen - 1);
				slot[len - klen - 1] = '\0';
				var->value = slot;
				return true;
			}
			p += len + (end ? 1 : 0);
		}
		var->value = NULL;
		return false;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		if (data) *(bool*)data = options_changed_pending;
		options_changed_pending = 0;
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
		return true;
	default:
		return false;
	}
}
static int video_frames = 0, audio_batches = 0, overlay_pixels_seen = 0;
static void fe_video_refresh(const void* d, unsigned w, unsigned h, size_t p) {
	video_frames++;
	if (getenv("HARNESS_EXPECT_OVERLAY") && d) {
		const uint16_t* pixels = d;
		for (unsigned y = 0; y < h && !overlay_pixels_seen; y++)
			for (unsigned x = 0; x < w; x++)
				if (pixels[(size_t)y * (p / sizeof(*pixels)) + x]) {
					overlay_pixels_seen = 1;
					break;
				}
	}
	if (video_frames <= 3) printf("fe:video_refresh %ux%u\n", w, h);
}
static void fe_audio_sample(int16_t l, int16_t r)                { printf("fe:audio_sample\n"); }
static size_t fe_audio_sample_batch(const int16_t* d, size_t f) {
	audio_batches++;
	if (audio_batches <= 3) printf("fe:audio_sample_batch %zu\n", f);
	return f;
}
static int input_polls = 0;
static void fe_input_poll(void) {
	input_polls++;
	if (input_polls <= 3) printf("fe:input_poll\n");
}
static int16_t fe_input_state(unsigned p, unsigned d, unsigned i, unsigned id) {
	static int shown = 0;
	if (shown++ < 3) printf("fe:input_state\n");
	// HARNESS_BUTTONS gives each side a distinguishable input, so the test can
	// tell whose input landed on which port.
	static int buttons = -1;
	if (buttons < 0) buttons = getenv("HARNESS_BUTTONS") ? atoi(getenv("HARNESS_BUTTONS")) : 42;
	if (getenv("HARNESS_VARY_INPUT")) {
		static int tick = 0;
		return (int16_t)(buttons + (++tick & 7));
	}
	return (int16_t)buttons;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		printf("usage: %s <shim.so> [frames] [ms_per_frame]\n", argv[0]);
		return 2;
	}
	// Link tests need the loop to run long enough for a peer to connect and
	// packets to cross; the passthrough test just wants a single frame.
	int frames = (argc > 2) ? atoi(argv[2]) : 1;
	int frame_ms = (argc > 3) ? atoi(argv[3]) : 0;

	void* handle = dlopen(argv[1], RTLD_LAZY);
	CHECK(handle != NULL, "dlopen(%s): %s", argv[1], dlerror());
	if (!handle) return 1;

	// Exactly the symbols ma_core.c resolves.
	void     (*init)(void)                                  = dlsym(handle, "retro_init");
	void     (*deinit)(void)                                = dlsym(handle, "retro_deinit");
	void     (*get_system_info)(struct retro_system_info*)  = dlsym(handle, "retro_get_system_info");
	void     (*get_system_av_info)(struct retro_system_av_info*) = dlsym(handle, "retro_get_system_av_info");
	void     (*set_controller_port_device)(unsigned, unsigned) = dlsym(handle, "retro_set_controller_port_device");
	void     (*reset)(void)                                 = dlsym(handle, "retro_reset");
	void     (*run)(void)                                   = dlsym(handle, "retro_run");
	size_t   (*serialize_size)(void)                        = dlsym(handle, "retro_serialize_size");
	bool     (*serialize)(void*, size_t)                    = dlsym(handle, "retro_serialize");
	bool     (*unserialize)(const void*, size_t)            = dlsym(handle, "retro_unserialize");
	void     (*cheat_reset)(void)                           = dlsym(handle, "retro_cheat_reset");
	void     (*cheat_set)(unsigned, bool, const char*)      = dlsym(handle, "retro_cheat_set");
	bool     (*load_game)(const struct retro_game_info*)    = dlsym(handle, "retro_load_game");
	bool     (*load_game_special)(unsigned, const struct retro_game_info*, size_t) = dlsym(handle, "retro_load_game_special");
	void     (*unload_game)(void)                           = dlsym(handle, "retro_unload_game");
	unsigned (*get_region)(void)                            = dlsym(handle, "retro_get_region");
	void*    (*get_memory_data)(unsigned)                   = dlsym(handle, "retro_get_memory_data");
	size_t   (*get_memory_size)(unsigned)                   = dlsym(handle, "retro_get_memory_size");

	void (*set_environment)(retro_environment_t)               = dlsym(handle, "retro_set_environment");
	void (*set_video_refresh)(retro_video_refresh_t)           = dlsym(handle, "retro_set_video_refresh");
	void (*set_audio_sample)(retro_audio_sample_t)             = dlsym(handle, "retro_set_audio_sample");
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t) = dlsym(handle, "retro_set_audio_sample_batch");
	void (*set_input_poll)(retro_input_poll_t)                 = dlsym(handle, "retro_set_input_poll");
	void (*set_input_state)(retro_input_state_t)               = dlsym(handle, "retro_set_input_state");

	CHECK(init && deinit && get_system_info && get_system_av_info && run && load_game &&
	      set_environment && set_video_refresh && set_audio_sample &&
	      set_audio_sample_batch && set_input_poll && set_input_state,
	      "shim is missing entry points minarch requires");

	CHECK(set_controller_port_device && reset && serialize_size && serialize &&
	      unserialize && cheat_reset && cheat_set && load_game_special &&
	      unload_game && get_region && get_memory_data && get_memory_size,
	      "shim is missing entry points minarch resolves but does not hard-require");

	// --- minarch's order starts here -------------------------------------

	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	get_system_info(&info);

	// These feed core.extensions / core.need_fullpath, and minarch strcpys
	// valid_extensions unchecked.
	const char* expected_core_name = getenv("HARNESS_EXPECT_CORE_NAME");
	if (!expected_core_name) expected_core_name = "FakeCore";
	CHECK(info.library_name && strcmp(info.library_name, expected_core_name) == 0,
	      "library_name not forwarded verbatim (got %s)", info.library_name ? info.library_name : "(null)");
	CHECK(info.valid_extensions && strcmp(info.valid_extensions, "fake|bin") == 0,
	      "valid_extensions not forwarded verbatim");
	CHECK(info.need_fullpath == true, "need_fullpath not forwarded");

	set_environment(fe_environment);
	set_video_refresh(fe_video_refresh);
	set_audio_sample(fe_audio_sample);
	set_audio_sample_batch(fe_audio_sample_batch);
	set_input_poll(fe_input_poll);
	set_input_state(fe_input_state);

	init();

	// HARNESS_ROM loads a real ROM so a core can be benchmarked at full tilt.
	struct retro_game_info game;
	memset(&game, 0, sizeof(game));
	game.path = "/fake/rom.bin";
	static const unsigned char fake_rom[] = "netplay-test-rom-v1";
	const char* fake_rom_id = getenv("HARNESS_ROM_ID");
	game.data = fake_rom_id ? (const void*)fake_rom_id : fake_rom;
	game.size = fake_rom_id ? strlen(fake_rom_id) : sizeof(fake_rom) - 1;
	void* rom_buf = NULL;
	const char* rom_path = getenv("HARNESS_ROM");
	if (rom_path) {
		FILE* rf = fopen(rom_path, "rb");
		if (rf) {
			fseek(rf, 0, SEEK_END); long n = ftell(rf); fseek(rf, 0, SEEK_SET);
			rom_buf = malloc(n);
			if (rom_buf && fread(rom_buf, 1, n, rf) == (size_t)n) {
				game.path = rom_path; game.data = rom_buf; game.size = n;
				printf("fe:rom %ld bytes\n", n);
			}
			fclose(rf);
		}
	}
	// HARNESS_LOAD_DELAY_MS is a device that takes its time with the content:
	// a disc image hashes in seconds on one handheld and well over ten on
	// another, while the session is already connected.
	if (getenv("HARNESS_LOAD_DELAY_MS")) usleep(atoi(getenv("HARNESS_LOAD_DELAY_MS")) * 1000);
	CHECK(load_game(&game) == true, "load_game did not forward its return value");

	struct retro_system_av_info av;
	memset(&av, 0, sizeof(av));
	get_system_av_info(&av);
	CHECK(av.geometry.base_width == 240 && av.geometry.base_height == 160,
	      "av_info geometry not forwarded (got %ux%u)", av.geometry.base_width, av.geometry.base_height);
	CHECK(av.timing.sample_rate == 44100.0, "av_info sample_rate not forwarded");

	// HARNESS_STALL_AT / HARNESS_STALL_MS reproduce a menu: the frontend simply
	// stops calling retro_run while everything else stays alive.
	int stall_at = getenv("HARNESS_STALL_AT") ? atoi(getenv("HARNESS_STALL_AT")) : -1;
	int stall_ms = getenv("HARNESS_STALL_MS") ? atoi(getenv("HARNESS_STALL_MS")) : 0;

	struct timeval t0, t1;
	gettimeofday(&t0, NULL);
	for (int i = 0; i < frames; i++) {
		if (getenv("HARNESS_RESET_AT") && i == atoi(getenv("HARNESS_RESET_AT"))) {
			printf("fe:reset_request at=%d\n", i); fflush(stdout);
			reset();
		}
		if (getenv("HARNESS_DIE_AT") && i == atoi(getenv("HARNESS_DIE_AT"))) {
			printf("fe:process_crash at=%d\n", i); fflush(stdout);
			_exit(86);
		}
		if (i == stall_at) {
			printf("fe:stall_begin\n"); fflush(stdout);
			usleep(stall_ms * 1000);
			// Quit while still stalled, so the peer never gets a resume - the
			// device case where a player opens the menu and then exits.
			if (getenv("HARNESS_DIE_IN_STALL")) {
				printf("fe:died_in_stall\n"); fflush(stdout);
				_exit(0);
			}
			printf("fe:stall_end\n"); fflush(stdout);
		}
		harness_frame = i;
		if (getenv("HARNESS_OPTIONS_LATER_AT") && i == atoi(getenv("HARNESS_OPTIONS_LATER_AT"))) {
			printf("fe:options_changed at=%d\n", i); fflush(stdout);
			options_changed_pending = 1;
		}
		run();
		fflush(stdout);
		if (frame_ms) usleep(frame_ms * 1000);
	}

	size_t sz = serialize_size();
	unsigned char buf[16];
	if (getenv("HARNESS_EXPECT_NO_STATES") || getenv("NETPLAY_SESSION")) {
		CHECK(sz == 0, "active session exposed save states (size %zu)", sz);
		CHECK(serialize(buf, sizeof(buf)) == false, "active session allowed a save state");
		CHECK(unserialize(buf, sizeof(buf)) == false, "active session allowed a state load");
	} else {
		CHECK(sz == 16, "serialize_size not forwarded (got %zu)", sz);
		CHECK(serialize(buf, sizeof(buf)) == true, "serialize did not forward");
		CHECK(buf[0] == 0xAB, "serialize did not write the core's bytes through");
		if (getenv("HARNESS_EXPECT_NO_LOAD"))
			CHECK(unserialize(buf, sizeof(buf)) == false,
			      "active session allowed a frontend state load");
		else
			CHECK(unserialize(buf, sizeof(buf)) == true, "unserialize did not forward");
	}

	CHECK(get_region() == RETRO_REGION_NTSC, "get_region not forwarded");
	if (getenv("HARNESS_EXPECT_OVERLAY"))
		CHECK(overlay_pixels_seen, "startup notice was not drawn over the core frame");
	if (getenv("HARNESS_EXPECT_NO_PERSISTENCE")) {
		CHECK(get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0,
		      "shared-screen guest exposed SRAM size");
		CHECK(get_memory_data(RETRO_MEMORY_SAVE_RAM) == NULL,
		      "shared-screen guest exposed SRAM data");
		CHECK(get_memory_size(RETRO_MEMORY_RTC) == 0,
		      "shared-screen guest exposed RTC size");
		CHECK(get_memory_data(RETRO_MEMORY_RTC) == NULL,
		      "shared-screen guest exposed RTC data");
	} else {
		CHECK(get_memory_size(RETRO_MEMORY_SAVE_RAM) == 16, "get_memory_size not forwarded");
		CHECK(get_memory_data(RETRO_MEMORY_SAVE_RAM) != NULL, "get_memory_data not forwarded");
	}

	unload_game();
	deinit();

	gettimeofday(&t1, NULL);
	{
		double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
		if (secs > 0)
			printf("fe:bench %d frames in %.2fs = %.1f fps (%.0f%% of 60fps realtime)\n",
			       frames, secs, frames / secs, 100.0 * (frames / secs) / 60.0);
	}
	printf("fe:totals video=%d audio=%d input=%d\n", video_frames, audio_batches, input_polls);
	printf(failures ? "\nRESULT: %d failure(s)\n" : "\nRESULT: ok (%d failures)\n", failures);
	return failures ? 1 : 0;
}
