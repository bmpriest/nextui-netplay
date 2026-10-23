/*
 * Minimal paired libretro frontend for mGBA's in-process GBA lockstep driver.
 *
 * This is deliberately separate from libretro.c. The stock frontend owns one
 * mCore and provides the full option/peripheral surface; this frontend owns two
 * cores and provides only the deterministic surface needed by Netplay's
 * retro_dual_* ABI. Ordinary single-player launches continue to use the stock
 * core.
 */
#include "libretro.h"
#include "gambatte_dual.h"

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/memory.h>
#include <mgba/internal/gba/savedata.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>
#include <mgba-util/vfs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Sourced from the shared shim/include/gambatte_dual.h (copied alongside this
 * file into the mgba source tree by the Makefile) so the ABI version and
 * capability bits cannot drift from what shim.c's core_has_dual_contract()
 * checks against. */
#define DUAL_ABI_VERSION GAMBATTE_DUAL_ABI_VERSION
#define DUAL_SUBSYSTEM_ID GAMBATTE_DUAL_SUBSYSTEM_ID
#define DUAL_MAGIC 0x4d474244u /* MGBD */
#define DUAL_STATE_VERSION 1u

#define DUAL_CAP_TWO_CONTENTS      GAMBATTE_DUAL_CAP_TWO_CONTENTS
#define DUAL_CAP_CONSOLE_MEMORY    GAMBATTE_DUAL_CAP_CONSOLE_MEMORY
#define DUAL_CAP_VISIBLE_CONSOLE   GAMBATTE_DUAL_CAP_VISIBLE_CONSOLE
#define DUAL_CAP_PAIRED_CHECKPOINT GAMBATTE_DUAL_CAP_PAIRED_CHECKPOINT
#define DUAL_CAP_TARGETED_RESET    GAMBATTE_DUAL_CAP_TARGETED_RESET
#define DUAL_CAP_CLOCK_EPOCHS      GAMBATTE_DUAL_CAP_CLOCK_EPOCHS

#define NCONSOLES 2
#define VIDEO_STRIDE 256
#define VIDEO_HEIGHT 224
#define AUDIO_FRAMES 4096
#define AUDIO_OUTPUT_FRAMES (AUDIO_FRAMES * 2)
#define AUDIO_DEFAULT_RATE 65536u
#define RUN_GUARD 65536u

const char* const projectName = "mGBA Dual";
const char* const projectVersion = "0.11-dev-netdual1";

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

struct DualConsole {
	struct mCore* core;
	struct GBASIOLockstepDriver driver;
	struct mLockstepUser user;
	struct mAudioBuffer resample_buffer;
	struct mAudioResampler resampler;
	unsigned resampler_source_rate;
	mColor* video;
	void* rom_data;
	size_t rom_size;
	uint8_t* savedata;
	int index;
	int player_id;
	bool asleep;
	bool audio_init;
};

struct DualStateHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t state_size[2];
	uint32_t reserved[4];
};

static struct DualConsole consoles[NCONSOLES];
static struct GBASIOLockstepCoordinator coordinator;
static bool coordinator_init;
static bool content_loaded;
static unsigned visible_console;
static uint64_t clock_epochs[NCONSOLES];
static bool clock_epochs_set;
static bool input_bitmasks;
static unsigned audio_output_rate = AUDIO_DEFAULT_RATE;
static bool audio_signal_reported;
static struct mLogger dual_logger;

struct DualProfile {
	uint64_t input_us;
	uint64_t emulate_us;
	uint64_t video_us;
	uint64_t audio_us;
	uint64_t total_us;
	uint32_t emulate_max_us;
	uint32_t total_max_us;
	unsigned frames;
};

static struct DualProfile profile;

static uint64_t monotonic_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u;
}

static void profile_report(void) {
	if (profile.frames < 600) {
		return;
	}
	fprintf(stderr,
	        "mGBA Dual profile: frames=%u input=%lluus emulation+cable=%lluus "
	        "video=%lluus audio=%lluus total=%lluus emu_max=%uus total_max=%uus\n",
	        profile.frames,
	        (unsigned long long) (profile.input_us / profile.frames),
	        (unsigned long long) (profile.emulate_us / profile.frames),
	        (unsigned long long) (profile.video_us / profile.frames),
	        (unsigned long long) (profile.audio_us / profile.frames),
	        (unsigned long long) (profile.total_us / profile.frames),
	        profile.emulate_max_us, profile.total_max_us);
	memset(&profile, 0, sizeof(profile));
}

bool retro_dual_reset_console(unsigned console);

static void dual_log(struct mLogger* logger, int category, enum mLogLevel level,
		const char* format, va_list args) {
	(void) logger;
	(void) category;
	if (level == mLOG_FATAL) {
		fputs("mGBA Dual FATAL: ", stderr);
		vfprintf(stderr, format, args);
		fputc('\n', stderr);
	}
}

static struct DualConsole* console_from_user(struct mLockstepUser* user) {
	return (struct DualConsole*) ((char*) user - offsetof(struct DualConsole, user));
}

static void user_sleep(struct mLockstepUser* user) {
	console_from_user(user)->asleep = true;
}

static void user_wake(struct mLockstepUser* user) {
	console_from_user(user)->asleep = false;
}

static int user_requested_id(struct mLockstepUser* user) {
	return console_from_user(user)->index;
}

static void user_player_id_changed(struct mLockstepUser* user, int id) {
	console_from_user(user)->player_id = id;
}

static void console_clear(struct DualConsole* c) {
	if (c->audio_init) {
		mAudioResamplerDeinit(&c->resampler);
		mAudioBufferDeinit(&c->resample_buffer);
	}
	if (c->core) {
		mCoreConfigDeinit(&c->core->config);
		c->core->deinit(c->core);
	}
	free(c->video);
	free(c->rom_data);
	free(c->savedata);
	memset(c, 0, sizeof(*c));
	c->player_id = -1;
}

static void unload_pair(void) {
	unsigned i;
	for (i = 0; i < NCONSOLES; ++i) {
		console_clear(&consoles[i]);
	}
	if (coordinator_init) {
		GBASIOLockstepCoordinatorDeinit(&coordinator);
		coordinator_init = false;
	}
	content_loaded = false;
}

static struct VFile* open_rom(const struct retro_game_info* game,
		struct DualConsole* c) {
	if (game->data && game->size) {
		c->rom_data = malloc(game->size);
		if (!c->rom_data) {
			return NULL;
		}
		memcpy(c->rom_data, game->data, game->size);
		c->rom_size = game->size;
		return VFileFromMemory(c->rom_data, c->rom_size);
	}
	if (game->path) {
		return VFileOpen(game->path, O_RDONLY);
	}
	return NULL;
}

static bool console_load(struct DualConsole* c,
		const struct retro_game_info* game, int index) {
	memset(c, 0, sizeof(*c));
	c->index = index;
	c->player_id = -1;

	struct VFile* rom = open_rom(game, c);
	if (!rom) {
		return false;
	}
	c->core = mCoreFindVF(rom);
	if (!c->core || c->core->platform(c->core) != mPLATFORM_GBA) {
		rom->close(rom);
		console_clear(c);
		return false;
	}
	mCoreInitConfig(c->core, NULL);
	if (!c->core->init(c->core)) {
		rom->close(rom);
		console_clear(c);
		return false;
	}
	/* Match stock mGBA's libretro defaults. A zero-initialized options struct
	 * sets masterVolume to zero when the GBA config is loaded. */
	struct mCoreOptions opts = {
		.useBios = true,
		.volume = 0x100,
	};
	mCoreConfigLoadDefaults(&c->core->config, &opts);
	mCoreLoadConfig(c->core);
	if (clock_epochs_set) {
		c->core->rtc.override = RTC_FAKE_EPOCH;
		c->core->rtc.value = (int64_t) clock_epochs[index] * 1000;
	}

	c->core->opts.skipBios = true;
	c->video = calloc(VIDEO_STRIDE * VIDEO_HEIGHT, sizeof(*c->video));
	c->savedata = malloc(GBA_SIZE_FLASH1M);
	if (!c->video || !c->savedata) {
		rom->close(rom);
		console_clear(c);
		return false;
	}
	memset(c->savedata, 0xFF, GBA_SIZE_FLASH1M);
	c->core->setVideoBuffer(c->core, c->video, VIDEO_STRIDE);
	c->core->setAudioBufferSize(c->core, AUDIO_FRAMES);
	mAudioBufferInit(&c->resample_buffer, AUDIO_OUTPUT_FRAMES, 2);
	mAudioResamplerInit(&c->resampler, mINTERPOLATOR_SINC);
	mAudioResamplerSetDestination(&c->resampler, &c->resample_buffer,
	                              audio_output_rate);
	c->audio_init = true;
	if (!c->core->loadROM(c->core, rom)) {
		rom->close(rom);
		console_clear(c);
		return false;
	}
	c->core->reset(c->core);

	struct VFile* save = VFileFromMemory(c->savedata, GBA_SIZE_FLASH1M);
	if (!save || !c->core->loadSave(c->core, save)) {
		if (save) {
			save->close(save);
		}
		console_clear(c);
		return false;
	}
	return true;
}

static bool load_pair(const struct retro_game_info* games) {
	unload_pair();
	GBASIOLockstepCoordinatorInit(&coordinator);
	coordinator_init = true;

	unsigned i;
	for (i = 0; i < NCONSOLES; ++i) {
		if (!console_load(&consoles[i], &games[i], (int) i)) {
			unload_pair();
			return false;
		}
		consoles[i].user.sleep = user_sleep;
		consoles[i].user.wake = user_wake;
		consoles[i].user.requestedId = user_requested_id;
		consoles[i].user.playerIdChanged = user_player_id_changed;
		GBASIOLockstepDriverCreate(&consoles[i].driver, &consoles[i].user);
		GBASIOLockstepCoordinatorAttach(&coordinator, &consoles[i].driver);
		struct GBA* gba = consoles[i].core->board;
		GBASIOSetDriver(&gba->sio, &consoles[i].driver.d);
	}
	content_loaded = true;
	return true;
}

static uint32_t read_keys(unsigned port) {
	static const unsigned keymap[] = {
		RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
		RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
		RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_LEFT,
		RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
		RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_L,
	};
	uint32_t keys = 0;
	unsigned i;
	int16_t mask = input_state_cb && input_bitmasks
		? input_state_cb(port, RETRO_DEVICE_JOYPAD, 0,
		                  RETRO_DEVICE_ID_JOYPAD_MASK) : 0;
	for (i = 0; i < sizeof(keymap) / sizeof(*keymap); ++i) {
		if ((input_bitmasks && (mask & (1 << keymap[i]))) ||
		    (!input_bitmasks && input_state_cb &&
		     input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, keymap[i]))) {
			keys |= 1u << i;
		}
	}
	return keys;
}

static void drain_audio(unsigned visible) {
	static int16_t samples[AUDIO_OUTPUT_FRAMES * 2];
	unsigned i;
	for (i = 0; i < NCONSOLES; ++i) {
		struct mAudioBuffer* buffer = consoles[i].core->getAudioBuffer(consoles[i].core);
		if (i == visible) {
			unsigned source_rate = consoles[i].core->audioSampleRate(consoles[i].core);
			if (source_rate != audio_output_rate) {
				/* Reconfiguring the resampler source is not free; only do it when
				 * the core's rate actually changed instead of on every frame. */
				if (source_rate != consoles[i].resampler_source_rate) {
					mAudioResamplerSetSource(&consoles[i].resampler, buffer,
					                         source_rate, true);
					consoles[i].resampler_source_rate = source_rate;
				}
				mAudioResamplerProcess(&consoles[i].resampler);
				buffer = &consoles[i].resample_buffer;
			}
		}
		size_t available;
		while ((available = mAudioBufferAvailable(buffer)) != 0) {
			if (available > AUDIO_OUTPUT_FRAMES) {
				available = AUDIO_OUTPUT_FRAMES;
			}
			size_t produced = mAudioBufferRead(buffer, samples, available);
			if (i == visible && audio_batch_cb && produced) {
				if (!audio_signal_reported) {
					int peak = 0;
					size_t sample;
					for (sample = 0; sample < produced * 2; ++sample) {
						int magnitude = samples[sample] < 0
							? -(int) samples[sample] : samples[sample];
						if (magnitude > peak) {
							peak = magnitude;
						}
					}
					if (peak) {
						fprintf(stderr,
						        "mGBA Dual audio active: output=%u source=%u peak=%d\n",
						        audio_output_rate,
						        consoles[i].core->audioSampleRate(consoles[i].core), peak);
						audio_signal_reported = true;
					}
				}
				audio_batch_cb(samples, produced);
			}
		}
	}
}

static bool run_visible_frame(void) {
	if (!content_loaded) {
		return false;
	}
	uint32_t target[NCONSOLES];
	unsigned i;
	for (i = 0; i < NCONSOLES; ++i) {
		target[i] = consoles[i].core->frameCounter(consoles[i].core) + 1;
	}
	unsigned steps = 0;
	while (steps < RUN_GUARD) {
		if (consoles[0].core->frameCounter(consoles[0].core) >= target[0] &&
		    consoles[1].core->frameCounter(consoles[1].core) >= target[1] &&
		    !coordinator.waiting && !coordinator.transferActive) {
			return true;
		}
		bool ran = false;
		for (i = 0; i < NCONSOLES; ++i) {
			if (consoles[i].asleep) {
				continue;
			}
			consoles[i].core->runLoop(consoles[i].core);
			ran = true;
			++steps;
		}
		if (!ran) {
			return false;
		}
	}
	return false;
}

static size_t save_one(struct DualConsole* c, void* output, size_t capacity) {
	struct VFile* vf = VFileMemChunk(NULL, 0);
	if (!vf || !mCoreSaveStateNamed(c->core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC)) {
		if (vf) {
			vf->close(vf);
		}
		return 0;
	}
	size_t size = (size_t) vf->size(vf);
	if (output) {
		if (size > capacity || vf->seek(vf, 0, SEEK_SET) < 0 ||
		    vf->read(vf, output, size) != (ssize_t) size) {
			vf->close(vf);
			return 0;
		}
	}
	vf->close(vf);
	return size;
}

static bool load_one(struct DualConsole* c, const void* data, size_t size) {
	struct VFile* vf = VFileFromConstMemory(data, size);
	if (!vf) {
		return false;
	}
	bool ok = mCoreLoadStateNamed(c->core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
	vf->close(vf);
	return ok;
}

unsigned retro_api_version(void) {
	return RETRO_API_VERSION;
}

void retro_set_environment(retro_environment_t cb) {
	environ_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb) {
	video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb) {
	(void) cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
	audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb) {
	input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb) {
	input_state_cb = cb;
}

void retro_init(void) {
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
	audio_output_rate = AUDIO_DEFAULT_RATE;
	if (environ_cb) {
		environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
		input_bitmasks = environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);
		unsigned requested_rate = audio_output_rate;
		if (environ_cb(RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE, &requested_rate) &&
		    requested_rate) {
			audio_output_rate = requested_rate;
		}
	}
	audio_signal_reported = false;
	memset(&profile, 0, sizeof(profile));
	dual_logger.log = dual_log;
	mLogSetDefaultLogger(&dual_logger);
}

void retro_deinit(void) {
	unload_pair();
}

void retro_get_system_info(struct retro_system_info* info) {
	memset(info, 0, sizeof(*info));
	info->library_name = projectName;
	info->library_version = projectVersion;
	info->valid_extensions = "gba";
	info->need_fullpath = false;
	info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	memset(info, 0, sizeof(*info));
	info->geometry.base_width = 240;
	info->geometry.base_height = 160;
	info->geometry.max_width = VIDEO_STRIDE;
	info->geometry.max_height = VIDEO_HEIGHT;
	info->geometry.aspect_ratio = 3.0f / 2.0f;
	info->timing.fps = content_loaded
		? consoles[0].core->frequency(consoles[0].core) /
		  (double) consoles[0].core->frameCycles(consoles[0].core)
		: 59.7275;
	info->timing.sample_rate = audio_output_rate;
}

bool retro_load_game(const struct retro_game_info* game) {
	if (!game) {
		return false;
	}
	struct retro_game_info games[2] = { *game, *game };
	return load_pair(games);
}

bool retro_load_game_special(unsigned type, const struct retro_game_info* games,
		size_t count) {
	return type == DUAL_SUBSYSTEM_ID && games && count == NCONSOLES &&
	       load_pair(games);
}

void retro_unload_game(void) {
	unload_pair();
}

void retro_run(void) {
	if (!content_loaded) {
		return;
	}
	uint64_t total_started = monotonic_us();
	if (input_poll_cb) {
		input_poll_cb();
	}
	unsigned i;
	for (i = 0; i < NCONSOLES; ++i) {
		consoles[i].core->setKeys(consoles[i].core, read_keys(i));
	}
	uint64_t input_finished = monotonic_us();
	bool ok = run_visible_frame();
	uint64_t emulate_finished = monotonic_us();
	unsigned width, height;
	consoles[visible_console].core->currentVideoSize(
		consoles[visible_console].core, &width, &height);
	if (video_cb) {
		video_cb(ok ? consoles[visible_console].video : NULL, width, height,
		         VIDEO_STRIDE * sizeof(mColor));
	}
	uint64_t video_finished = monotonic_us();
	drain_audio(visible_console);
	uint64_t audio_finished = monotonic_us();
	uint32_t emulate_us = (uint32_t) (emulate_finished - input_finished);
	uint32_t total_us = (uint32_t) (audio_finished - total_started);
	profile.input_us += input_finished - total_started;
	profile.emulate_us += emulate_us;
	profile.video_us += video_finished - emulate_finished;
	profile.audio_us += audio_finished - video_finished;
	profile.total_us += total_us;
	if (emulate_us > profile.emulate_max_us) profile.emulate_max_us = emulate_us;
	if (total_us > profile.total_max_us) profile.total_max_us = total_us;
	++profile.frames;
	profile_report();
}

void retro_reset(void) {
	retro_dual_reset_console(2);
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
	(void) port;
	(void) device;
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char* code) {
	(void) index;
	(void) enabled;
	(void) code;
}

unsigned retro_get_region(void) {
	return RETRO_REGION_NTSC;
}

unsigned retro_dual_get_abi_version(void) {
	return DUAL_ABI_VERSION;
}

uint64_t retro_dual_get_capabilities(void) {
	return DUAL_CAP_TWO_CONTENTS | DUAL_CAP_CONSOLE_MEMORY |
	       DUAL_CAP_VISIBLE_CONSOLE | DUAL_CAP_PAIRED_CHECKPOINT |
	       DUAL_CAP_TARGETED_RESET | DUAL_CAP_CLOCK_EPOCHS;
}

bool retro_dual_set_visible_console(unsigned console) {
	if (console >= NCONSOLES) {
		return false;
	}
	visible_console = console;
	return true;
}

unsigned retro_dual_get_visible_console(void) {
	return visible_console;
}

void* retro_dual_get_memory_data(unsigned console, unsigned id) {
	if (!content_loaded || console >= NCONSOLES) {
		return NULL;
	}
	struct GBA* gba = consoles[console].core->board;
	switch (id) {
	case RETRO_MEMORY_SAVE_RAM:
		return consoles[console].savedata;
	case RETRO_MEMORY_SYSTEM_RAM:
		return gba->memory.wram;
	case RETRO_MEMORY_VIDEO_RAM:
		return gba->video.vram;
	default:
		return NULL;
	}
}

size_t retro_dual_get_memory_size(unsigned console, unsigned id) {
	if (!content_loaded || console >= NCONSOLES) {
		return 0;
	}
	struct GBA* gba = consoles[console].core->board;
	switch (id) {
	case RETRO_MEMORY_SAVE_RAM:
		return gba->memory.savedata.type == GBA_SAVEDATA_AUTODETECT
			? GBA_SIZE_FLASH1M : GBASavedataSize(&gba->memory.savedata);
	case RETRO_MEMORY_SYSTEM_RAM:
		return GBA_SIZE_EWRAM;
	case RETRO_MEMORY_VIDEO_RAM:
		return GBA_SIZE_VRAM;
	default:
		return 0;
	}
}

void* retro_get_memory_data(unsigned id) {
	return retro_dual_get_memory_data(visible_console, id);
}

size_t retro_get_memory_size(unsigned id) {
	return retro_dual_get_memory_size(visible_console, id);
}

bool retro_dual_reset_console(unsigned console) {
	if (!content_loaded || console > NCONSOLES) {
		return false;
	}
	unsigned first = console == NCONSOLES ? 0 : console;
	unsigned last = console == NCONSOLES ? NCONSOLES : console + 1;
	unsigned i;
	for (i = first; i < last; ++i) {
		consoles[i].core->reset(consoles[i].core);
	}
	return true;
}

bool retro_dual_is_checkpoint_safe(void) {
	return content_loaded && !coordinator.waiting &&
	       !coordinator.transferActive;
}

size_t retro_dual_serialize_size(void) {
	if (!retro_dual_is_checkpoint_safe()) {
		return 0;
	}
	size_t a = save_one(&consoles[0], NULL, 0);
	size_t b = save_one(&consoles[1], NULL, 0);
	return a && b ? sizeof(struct DualStateHeader) + a + b : 0;
}

bool retro_dual_serialize(void* data, size_t size) {
	if (!data || !retro_dual_is_checkpoint_safe()) {
		return false;
	}
	size_t a = save_one(&consoles[0], NULL, 0);
	size_t b = save_one(&consoles[1], NULL, 0);
	if (!a || !b || a > UINT32_MAX || b > UINT32_MAX ||
	    size != sizeof(struct DualStateHeader) + a + b) {
		return false;
	}
	struct DualStateHeader header;
	memset(&header, 0, sizeof(header));
	header.magic = DUAL_MAGIC;
	header.version = DUAL_STATE_VERSION;
	header.state_size[0] = (uint32_t) a;
	header.state_size[1] = (uint32_t) b;
	memcpy(data, &header, sizeof(header));
	uint8_t* out = (uint8_t*) data + sizeof(header);
	return save_one(&consoles[0], out, a) == a &&
	       save_one(&consoles[1], out + a, b) == b;
}

bool retro_dual_unserialize(const void* data, size_t size) {
	if (!data || !content_loaded || size < sizeof(struct DualStateHeader)) {
		return false;
	}
	struct DualStateHeader header;
	memcpy(&header, data, sizeof(header));
	size_t a = header.state_size[0];
	size_t b = header.state_size[1];
	if (header.magic != DUAL_MAGIC || header.version != DUAL_STATE_VERSION ||
	    size != sizeof(header) + a + b) {
		return false;
	}
	const uint8_t* in = (const uint8_t*) data + sizeof(header);
	return load_one(&consoles[0], in, a) &&
	       load_one(&consoles[1], in + a, b);
}

size_t retro_serialize_size(void) {
	return retro_dual_serialize_size();
}

bool retro_serialize(void* data, size_t size) {
	return retro_dual_serialize(data, size);
}

bool retro_unserialize(const void* data, size_t size) {
	return retro_dual_unserialize(data, size);
}

bool retro_dual_set_clock_epochs(uint64_t console_a, uint64_t console_b) {
	uint64_t epochs[2] = { console_a, console_b };
	unsigned i;
	for (i = 0; i < NCONSOLES; ++i) {
		if (epochs[i] > INT64_MAX / 1000u) {
			return false;
		}
		clock_epochs[i] = epochs[i];
		if (content_loaded) {
			consoles[i].core->rtc.override = RTC_FAKE_EPOCH;
			consoles[i].core->rtc.value = (int64_t) epochs[i] * 1000;
		}
	}
	clock_epochs_set = true;
	return true;
}
