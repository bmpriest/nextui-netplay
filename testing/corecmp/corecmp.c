/*
 * corecmp - one libretro core, one rom, a fixed input script, a per-frame trace.
 *
 * Run two builds of a core with the same rom and compare their traces to see
 * whether they compute the same game; save a state from one and load it into
 * the other to see whether they can exchange states. Netplay needs both: the
 * same inputs must give the same game on both devices, and a state sent from
 * one must carry on identically on the other.
 *
 *   corecmp <core.so> <rom> <frames> <trace> [--save-at N <file>] [--load <file> N]
 *
 *   --save-at N f  after frame N, write the state to f
 *   --load f N     load f before the first frame and count frames from N, so
 *                  the input script carries on where the saving run left it
 *
 * Each trace line is "<frame> <ram> <video> <audio> <state> <size>": FNV-1a of
 * the core's system RAM, of the frame's pixels, of its audio samples and of its
 * serialized state, then the state's size. A dup'd frame keeps the last video
 * hash. Inputs come from the frame number alone, so every run sees the same
 * script without recording one.
 *
 * Core options as NAME=VALUE;NAME=VALUE in $CORECMP_VARS; anything unset stays
 * at the core's default, the same for both builds. $SYSDIR is the BIOS
 * directory, $SAVEDIR the save directory (default: a fresh one per run).
 */

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libretro.h"

static void* handle;
#define SYM(t, n) ((t)dlsym(handle, n))

static char sysdir[512] = "/mnt/SDCARD/BIOS";
static char savedir[512];

static char vars_buf[1024];
static struct { const char* key; const char* value; } vars[32];
static int var_count = -1;

static void vars_init(void) {
	var_count = 0;
	const char* src = getenv("CORECMP_VARS");
	if (!src) return;
	snprintf(vars_buf, sizeof(vars_buf), "%s", src);
	char* p = vars_buf;
	while (p && *p && var_count < 32) {
		char* semi = strchr(p, ';');
		if (semi) *semi++ = '\0';
		char* eq = strchr(p, '=');
		if (eq) {
			*eq = '\0';
			vars[var_count].key = p;
			vars[var_count].value = eq + 1;
			var_count++;
		}
		p = semi;
	}
}

static const char* var_lookup(const char* key) {
	if (var_count < 0) vars_init();
	for (int i = 0; i < var_count; i++)
		if (!strcmp(vars[i].key, key)) return vars[i].value;
	return NULL;
}

static unsigned pixel_bytes = 2;   /* 0RGB1555 until the core says otherwise */

static bool env_cb(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		enum retro_pixel_format f = *(const enum retro_pixel_format*)data;
		pixel_bytes = f == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
		return true;
	}
	case RETRO_ENVIRONMENT_SET_VARIABLES:     return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable* v = (struct retro_variable*)data;
		if (!v || !v->key) return false;
		const char* val = var_lookup(v->key);
		if (!val) return false;
		v->value = val;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		if (data) *(bool*)data = false;
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		if (data) *(const char**)data = sysdir;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		if (data) *(const char**)data = savedir;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		if (data) *(bool*)data = true;
		return true;
	default: return false;
	}
}

static uint32_t fnv_add(uint32_t h, const void* p, size_t n) {
	const uint8_t* b = p;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
	return h;
}
#define FNV0 2166136261u

static uint32_t video_hash = FNV0;
static uint32_t audio_hash;

static void video_cb(const void* d, unsigned w, unsigned h, size_t pitch) {
	if (!d) return;                     /* dup: the frame is unchanged */
	uint32_t x = FNV0;
	for (unsigned y = 0; y < h; y++) x = fnv_add(x, (const uint8_t*)d + y * pitch, (size_t)w * pixel_bytes);
	x = fnv_add(x, &w, sizeof(w));
	x = fnv_add(x, &h, sizeof(h));
	video_hash = x;
}
static void audio_cb(int16_t l, int16_t r) {
	int16_t s[2] = { l, r };
	audio_hash = fnv_add(audio_hash, s, sizeof(s));
}
static size_t batch_cb(const int16_t* d, size_t f) {
	audio_hash = fnv_add(audio_hash, d, f * 4);
	return f;
}
static void poll_cb(void) {}

/* The input script. Menus first: START and A pressed briefly, apart, so a title
 * screen and a menu or two are passed. Then a new random-looking pad every six
 * frames for each player, without START or SELECT, which pause or open menus,
 * and without opposite directions together. */
static unsigned frame_no;

static uint16_t pad_for(unsigned port, unsigned f) {
	if (f < 900) {
		unsigned phase = f % 120;
		if (phase < 4) return port == 0 ? 1u << RETRO_DEVICE_ID_JOYPAD_START : 0;
		if (phase >= 60 && phase < 64) return 1u << RETRO_DEVICE_ID_JOYPAD_A;
		return 0;
	}
	uint32_t x = (f / 6) * 2654435761u ^ (port + 1) * 0x9E3779B9u;
	x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; x *= 3266489917u; x ^= x >> 16;
	uint16_t m = 0;
	static const unsigned ids[] = {
		RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_LEFT,
		RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
		RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_L,
		RETRO_DEVICE_ID_JOYPAD_R,
	};
	for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
		if (x & (1u << i)) m |= 1u << ids[i];
	if ((m & (1u << RETRO_DEVICE_ID_JOYPAD_UP)) && (m & (1u << RETRO_DEVICE_ID_JOYPAD_DOWN)))
		m &= ~(1u << RETRO_DEVICE_ID_JOYPAD_DOWN);
	if ((m & (1u << RETRO_DEVICE_ID_JOYPAD_LEFT)) && (m & (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT)))
		m &= ~(1u << RETRO_DEVICE_ID_JOYPAD_RIGHT);
	return m;
}

static int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
	(void)index;
	if (port > 1 || (device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD) return 0;
	uint16_t m = pad_for(port, frame_no);
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)m;
	return id < 16 ? (m >> id) & 1 : 0;
}

static void* read_file(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* b = n > 0 ? malloc((size_t)n) : NULL;
	if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
	fclose(f);
	*size = (size_t)n;
	return b;
}

int main(int argc, char** argv) {
	if (argc < 5) {
		fprintf(stderr, "usage: %s <core.so> <rom> <frames> <trace> [--save-at N file] [--load file N]\n", argv[0]);
		return 2;
	}
	const char* corepath = argv[1];
	const char* rompath  = argv[2];
	unsigned frames = (unsigned)atoi(argv[3]);
	const char* tracepath = argv[4];
	const char* savepath = NULL, *loadpath = NULL;
	unsigned save_at = 0, start = 0;
	for (int i = 5; i < argc; i++) {
		if (!strcmp(argv[i], "--save-at") && i + 2 < argc) { save_at = (unsigned)atoi(argv[++i]); savepath = argv[++i]; }
		else if (!strcmp(argv[i], "--load") && i + 2 < argc) { loadpath = argv[++i]; start = (unsigned)atoi(argv[++i]); }
	}
	if (getenv("SYSDIR")) snprintf(sysdir, sizeof(sysdir), "%s", getenv("SYSDIR"));
	if (getenv("SAVEDIR")) snprintf(savedir, sizeof(savedir), "%s", getenv("SAVEDIR"));
	else {
		snprintf(savedir, sizeof(savedir), "/tmp/corecmp-save-%d", (int)getpid());
		mkdir(savedir, 0755);
	}

	handle = dlopen(corepath, RTLD_NOW | RTLD_LOCAL);
	if (!handle) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

	void (*set_env)(retro_environment_t)          = SYM(void(*)(retro_environment_t), "retro_set_environment");
	void (*set_video)(retro_video_refresh_t)      = SYM(void(*)(retro_video_refresh_t), "retro_set_video_refresh");
	void (*set_audio)(retro_audio_sample_t)       = SYM(void(*)(retro_audio_sample_t), "retro_set_audio_sample");
	void (*set_batch)(retro_audio_sample_batch_t) = SYM(void(*)(retro_audio_sample_batch_t), "retro_set_audio_sample_batch");
	void (*set_poll)(retro_input_poll_t)          = SYM(void(*)(retro_input_poll_t), "retro_set_input_poll");
	void (*set_input)(retro_input_state_t)        = SYM(void(*)(retro_input_state_t), "retro_set_input_state");
	void (*init)(void)                            = SYM(void(*)(void), "retro_init");
	bool (*load_game)(const struct retro_game_info*) = SYM(bool(*)(const struct retro_game_info*), "retro_load_game");
	void (*run)(void)                             = SYM(void(*)(void), "retro_run");
	size_t (*ser_size)(void)                      = SYM(size_t(*)(void), "retro_serialize_size");
	bool (*ser)(void*, size_t)                    = SYM(bool(*)(void*, size_t), "retro_serialize");
	bool (*unser)(const void*, size_t)            = SYM(bool(*)(const void*, size_t), "retro_unserialize");
	void (*get_info)(struct retro_system_info*)   = SYM(void(*)(struct retro_system_info*), "retro_get_system_info");
	void* (*mem_data)(unsigned)                   = SYM(void*(*)(unsigned), "retro_get_memory_data");
	size_t (*mem_size)(unsigned)                  = SYM(size_t(*)(unsigned), "retro_get_memory_size");
	if (!set_env || !init || !load_game || !run || !ser_size || !ser || !unser || !get_info) {
		fprintf(stderr, "core is missing required entry points\n");
		return 1;
	}

	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	get_info(&info);
	printf("core     : %s %s\n", info.library_name ? info.library_name : "?",
	       info.library_version ? info.library_version : "?");

	set_env(env_cb);
	init();
	if (set_video) set_video(video_cb);
	if (set_audio) set_audio(audio_cb);
	if (set_batch) set_batch(batch_cb);
	if (set_poll)  set_poll(poll_cb);
	if (set_input) set_input(input_cb);

	struct retro_game_info game;
	memset(&game, 0, sizeof(game));
	game.path = rompath;
	void* rom_data = NULL;
	if (!info.need_fullpath) {
		rom_data = read_file(rompath, &game.size);
		if (!rom_data) { fprintf(stderr, "cannot read %s\n", rompath); return 1; }
		game.data = rom_data;
	}
	if (!load_game(&game)) { fprintf(stderr, "retro_load_game failed for %s\n", rompath); return 1; }

	size_t sz = ser_size();
	printf("state    : %zu bytes\n", sz);
	uint8_t* state = sz ? malloc(sz) : NULL;

	if (loadpath) {
		size_t n = 0;
		void* blob = read_file(loadpath, &n);
		if (!blob) { fprintf(stderr, "cannot read %s\n", loadpath); return 1; }
		bool ok = unser(blob, n);
		printf("load     : %s (%zu bytes into a %zu-byte state) %s\n", loadpath, n, sz, ok ? "accepted" : "REJECTED");
		free(blob);
		if (!ok) return 3;
		sz = ser_size();
		state = realloc(state, sz);
	}

	FILE* trace = fopen(tracepath, "w");
	if (!trace) { fprintf(stderr, "cannot write %s\n", tracepath); return 1; }
	size_t ram_size = mem_size ? mem_size(RETRO_MEMORY_SYSTEM_RAM) : 0;

	for (frame_no = start; frame_no < frames; frame_no++) {
		audio_hash = FNV0;
		run();
		uint32_t ram = FNV0;
		void* ram_data = ram_size && mem_data ? mem_data(RETRO_MEMORY_SYSTEM_RAM) : NULL;
		if (ram_data) ram = fnv_add(ram, ram_data, ram_size);
		uint32_t st = 0;
		size_t now = ser_size();
		if (now > sz) { sz = now; state = realloc(state, sz); }
		if (now && ser(state, now)) st = fnv_add(FNV0, state, now);
		fprintf(trace, "%u %08x %08x %08x %08x %zu\n", frame_no + 1, ram, video_hash, audio_hash, st, now);
		if (savepath && frame_no + 1 == save_at) {
			FILE* f = fopen(savepath, "wb");
			if (!f || fwrite(state, 1, now, f) != now) { fprintf(stderr, "cannot write %s\n", savepath); return 1; }
			fclose(f);
			printf("saved    : %s at frame %u (%zu bytes)\n", savepath, save_at, now);
		}
	}
	fclose(trace);
	printf("ran      : frames %u-%u, ram %zu bytes\n", start + 1, frames, ram_size);
	return 0;
}
