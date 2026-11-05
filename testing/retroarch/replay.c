/* replay <core> <rom> <state> <start> <inputs.txt> <crcs.txt> [end]
 * Loads <state> as the state before frame <start>, runs with the captured
 * inputs (port 0 host, port 1 client) and prints our CRC at every CRC frame
 * the host sent, beside the host's. STATE_VARS="k=v;k=v" sets core options;
 * RR_SPECIAL=1 answers the rollback-netplay savestate context. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "libretro.h"  /* shim/include */
static void* h;
#define SYM(t, n) ((t)dlsym(h, n))
static unsigned fr; static uint32_t in0[20000], in1[20000];
static int special;
static struct { char k[64], v[64]; } vars[64]; static int nvars;
static bool env(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char**)data = "/tmp"; return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data = true; return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable* v = data;
		for (int i = 0; i < nvars; i++) if (!strcmp(vars[i].k, v->key)) { v->value = vars[i].v; return true; }
		return false; }
	case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
		if (!special) return false; *(int*)data = 3; return true;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: *(int*)data = 3 | (special ? 4 : 0); return true;
	default: return false;
	}
}
static void vcb(const void* d, unsigned w, unsigned hh, size_t p) {}
static void acb(int16_t l, int16_t r) {}
static size_t bcb(const int16_t* d, size_t f) { return f; }
static void pcb(void) {}
static int16_t icb(unsigned p, unsigned d, unsigned i, unsigned id) {
	if (d != RETRO_DEVICE_JOYPAD || p > 1) return 0;
	uint32_t w = p ? in1[fr] : in0[fr];
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)w;
	return (w >> id) & 1;
}
static void* slurp(const char* p, long* n) { FILE* f = fopen(p, "rb"); if (!f) return NULL; fseek(f,0,2); *n = ftell(f); fseek(f,0,0); void* b = malloc(*n); fread(b,1,*n,f); fclose(f); return b; }
int main(int argc, char** argv) {
	const char* sv = getenv("STATE_VARS");
	if (sv) { char buf[2048]; snprintf(buf, sizeof buf, "%s", sv); for (char* t = strtok(buf, ";"); t; t = strtok(NULL, ";")) { char* e = strchr(t, '='); if (!e) continue; *e = 0; snprintf(vars[nvars].k, 64, "%s", t); snprintf(vars[nvars].v, 64, "%s", e + 1); nvars++; } }
	special = getenv("RR_SPECIAL") != NULL;
	h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); if (!h) { printf("dlopen %s\n", dlerror()); return 1; }
	SYM(void(*)(retro_environment_t), "retro_set_environment")(env);
	SYM(void(*)(retro_video_refresh_t), "retro_set_video_refresh")(vcb);
	SYM(void(*)(retro_audio_sample_t), "retro_set_audio_sample")(acb);
	SYM(void(*)(retro_audio_sample_batch_t), "retro_set_audio_sample_batch")(bcb);
	SYM(void(*)(retro_input_poll_t), "retro_set_input_poll")(pcb);
	SYM(void(*)(retro_input_state_t), "retro_set_input_state")(icb);
	SYM(void(*)(void), "retro_init")();
	long rn; void* rom = slurp(argv[2], &rn);
	struct retro_game_info g = { argv[2], rom, (size_t)rn, NULL };
	if (!SYM(bool(*)(const struct retro_game_info*), "retro_load_game")(&g)) { printf("load_game failed\n"); return 1; }
	for (unsigned p = 0; p < 2; p++) SYM(void(*)(unsigned, unsigned), "retro_set_controller_port_device")(p, 1);
	size_t (*ssz)(void) = SYM(size_t(*)(void), "retro_serialize_size");
	bool (*ser)(void*, size_t) = SYM(bool(*)(void*, size_t), "retro_serialize");
	bool (*unser)(const void*, size_t) = SYM(bool(*)(const void*, size_t), "retro_unserialize");
	void (*run)(void) = SYM(void(*)(void), "retro_run");
	long sn; void* st = slurp(argv[3], &sn);
	size_t sz = ssz(); printf("serialize_size %zu, state file %ld\n", sz, sn);
	unsigned start = atoi(argv[4]);
	FILE* fi = fopen(argv[5], "r"); unsigned f, a, b; while (fscanf(fi, "%u %u %u", &f, &a, &b) == 3) if (f < 20000) { in0[f] = a; in1[f] = b; } fclose(fi);
	unsigned cf[200], cc[200], nc = 0; FILE* fc = fopen(argv[6], "r"); while (nc < 200 && fscanf(fc, "%u %x", &cf[nc], &cc[nc]) == 2) nc++; fclose(fc);
	unsigned end = argc > 7 ? atoi(argv[7]) : start + 600;
	/* run a few frames from power-on first, like a frontend, then load */
	/* PRE=N: power-on frames before the load (default 2). HIST=state0.bin:
	 * load that at frame 0 and run the captured inputs up to <start> first,
	 * reproducing the host's own history before it loads <state>. */
	unsigned pre = getenv("PRE") ? atoi(getenv("PRE")) : 2;
	for (fr = 0; fr < pre; fr++) run();
	if (getenv("HIST")) {
		long hn; void* hs = slurp(getenv("HIST"), &hn);
		unser(hs, hn);
		for (fr = 0; fr < atoi(argv[4]); fr++) run();
	}
	if (!unser(st, sn)) { printf("unserialize failed\n"); return 1; }
	void* buf = malloc(sz);
	const char* cmp = getenv("CMP"); unsigned cmp_at = getenv("CMP_AT") ? atoi(getenv("CMP_AT")) : 0;
	for (fr = start; fr <= end; fr++) {
		if (cmp && fr == cmp_at) {
			long cn; unsigned char* c = slurp(cmp, &cn);
			ser(buf, sz);
			unsigned d = 0, first = 0; for (size_t i = 0; i < sz && i < (size_t)cn; i++) if (((unsigned char*)buf)[i] != c[i]) { if (!d) first = i; d++; }
			printf("at %u: %u bytes differ from %s (first @%u)\n", fr, d, cmp, first);
			if (d) { int shown = 0; for (size_t i = 0; i < sz && shown < 12; i++) if (((unsigned char*)buf)[i] != c[i]) { printf("  @%zu ours %02x host %02x\n", i, ((unsigned char*)buf)[i], c[i]); shown++; } }
		}
		for (unsigned i = 0; i < nc; i++) if (cf[i] == fr) {
			ser(buf, sz);
			uint32_t c = crc32(0, buf, sz);
			printf("frame %u host %08x ours %08x %s\n", fr, cc[i], c, c == cc[i] ? "MATCH" : "differ");
		}
		run();
	}
	return 0;
}
