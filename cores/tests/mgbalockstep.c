/*
 * Determinism harness for mGBA's GBA lockstep cable.
 *
 * The paired mGBA core will run two consoles and the cable between them inside
 * one process, and two handhelds must each reproduce that pair identically from
 * the same inputs. So the cable's answers are emulated state, exactly as
 * Gambatte's bus was, and the same question applies: given identical initial
 * state and identical inputs, do two runs compute identical results?
 *
 * This asks it without a ROM, a device, or a frontend. Two mCores, one
 * GBASIOLockstepCoordinator, two drivers, and a cooperative scheduler that owns
 * no policy of its own - it runs whichever console the cable says is awake, and
 * stops when the cable puts it to sleep. That is the scheduler the paired core
 * will have, so this is the thing under test as much as the cable is.
 *
 * Two properties are checked:
 *
 *   reproducible  two runs of the same script hash identically, per console.
 *   ordered       the scheduler never deadlocks and never runs a sleeping
 *                 console, and the run actually reaches the cable rather than
 *                 idling past it.
 *
 * The second matters because a harness that never exercises a transfer will
 * pass the first trivially. The built-in ROM programs SIOCNT for multiplayer
 * and starts transfers in a loop; the run asserts the coordinator saw them.
 *
 *   mgbalockstep [runs] [frames]
 *
 * Exit status is 0 only if every run produced identical hashes.
 */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/core/timing.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * libretro.c defines these and we deliberately exclude that translation unit -
 * it owns a single global mCore and the retro_* entry points, which is the very
 * assumption this harness exists to get out from under. The core's serializer
 * still references them, so supply the same values it would have.
 */
const char* const projectName = "mGBA";
const char* const projectVersion = "0.11-dev";

#define NCONSOLES 2
#define ROM_SIZE  (1024 * 32)
#define ROM_ENTRY 0xC0

/* ---------------------------------------------------------------- test ROM */

/*
 * Hand-encoded because no ARM assembler is assumed present; the pak's other
 * core tests need only a C++ compiler and this one should not be the exception.
 * ARM state, entry at 0x080000C0, condition AL throughout:
 *
 *   mov  r0, #0x04000000
 *   add  r0, r0, #0x100        ; r0 = 0x04000100, so I/O reachable by imm8
 *   mov  r1, #0
 *   strh r1, [r0, #0x34]       ; RCNT   = 0       (serial, not GPIO)
 *   mov  r1, #0x2000
 *   strh r1, [r0, #0x28]       ; SIOCNT = multi-play mode
 * loop:
 *   ldrh r1, [r0, #0x28]
 *   orr  r1, r1, #0x80         ; start bit
 *   strh r1, [r0, #0x28]
 * wait:
 *   ldrh r1, [r0, #0x28]
 *   tst  r1, #0x80
 *   bne  wait
 *   b    loop
 */
static const uint32_t kEntryTight[] = {
	0xE3A00404, /* mov  r0, #0x04000000 */
	0xE2800C01, /* add  r0, r0, #0x100  */
	0xE3A01000, /* mov  r1, #0          */
	0xE1C013B4, /* strh r1, [r0, #0x34] ; RCNT = 0 */
	0xE3A01C20, /* mov  r1, #0x2000     */
	0xE1C012B8, /* strh r1, [r0, #0x28] ; SIOCNT = multi */
	0xE1D012B8, /* loop: ldrh r1, [r0, #0x28] */
	0xE3811080, /* orr  r1, r1, #0x80   */
	0xE1C012B8, /* strh r1, [r0, #0x28] ; start */
	0xE1D012B8, /* wait: ldrh r1, [r0, #0x28] */
	0xE3110080, /* tst  r1, #0x80       */
	0x1AFFFFFC, /* bne  wait            */
	0xEAFFFFF8, /* b    loop            */
};

/*
 * Production test program. It selects multiplayer mode, allows the attach
 * handshake to settle, then idles. The harness injects one primary SIOCNT start
 * write per frame after settling. Driving the public MMIO function directly
 * makes transfer coverage deliberate and countable instead of depending on the
 * two CPUs reaching a polling loop at a convenient point in the handshake.
 */
static const uint32_t kEntrySpaced[] = {
	0xE3A00404, /*  0 mov  r0, #0x04000000 */
	0xE2800C01, /*  1 add  r0, r0, #0x100  */
	0xE3A01000, /*  2 mov  r1, #0          */
	0xE1C013B4, /*  3 strh r1, [r0, #0x34] */
	0xE3A01C20, /*  4 mov  r1, #0x2000     */
	0xE1C012B8, /*  5 strh r1, [r0, #0x28] */
	0xE3A02C01, /*  6 mov  r2, #0x100      */
	0xE2522001, /*  7 d1: subs r2, r2, #1  */
	0x1AFFFFFD, /*  8 bne  d1              */
	0xEAFFFFFE, /*  9 idle: b idle         */
};

static void buildTestROM(uint8_t* rom) {
	memset(rom, 0, ROM_SIZE);

	/* b 0xC0 -- also supplies the 0xEA at offset 3 that GBAIsROM looks for. */
	uint32_t branch = 0xEA000000 | (((ROM_ENTRY - 8) / 4) & 0x00FFFFFF);
	memcpy(rom, &branch, sizeof(branch));

	/* GBAIsROM's second signature byte. */
	rom[0xB2] = 0x96;

	if (getenv("MGBA_LOCKSTEP_TIGHT")) {
		memcpy(rom + ROM_ENTRY, kEntryTight, sizeof(kEntryTight));
	} else {
		memcpy(rom + ROM_ENTRY, kEntrySpaced, sizeof(kEntrySpaced));
	}
}

/* ------------------------------------------------------------------- logging */

/*
 * The SIO category is chatty enough at DEBUG to bury the result, and every line
 * of it is a side effect we do not want timing-coupled into the run. Swallow
 * everything by default; MGBA_LOCKSTEP_VERBOSE=1 puts it back when a failing
 * run needs reading.
 */
static unsigned long gFatals;        /* all of them */
static unsigned long gFatalsSettled; /* only those after the attach handshake */
static unsigned long gTransfers;     /* successful primary transfer starts */
static char gFirstFatal[256];

/* Frames 0-1 are reserved for the two-player attach handshake. */
#define SETTLE_FRAMES 2

/* Set by the scheduler so the FATAL handler can say who was executing. */
struct Console;
static struct Console* gRunning;
static int gRunningSleptAt;   /* cycle at which the running console last slept */
static int gRunningCycleNow;  /* filled by the handler */
static void diagnoseFatal(const char* msg);
static int runningSettled(void);

static void quietLog(struct mLogger* logger, int category, enum mLogLevel level,
                     const char* format, va_list args) {
	(void) logger;
	(void) category;

	if (!strcmp(format, "Transfer starting at %08X")) {
		++gTransfers;
	}

	/*
	 * mASSERT_LOG logs at FATAL and returns - it does not abort, with or without
	 * NDEBUG. So mGBA can declare "Multiplayer desynchronized" and the run
	 * carries on regardless. Counting these is the point: two runs that desync
	 * the *same* way still hash identically, so a reproducibility check alone
	 * would pass while the cable was being driven into a state its own author
	 * considers broken. Determinism is not correctness.
	 */
	if (level == mLOG_FATAL) {
		char msg[256];
		va_list copy;
		va_copy(copy, args);
		vsnprintf(msg, sizeof(msg), format, copy);
		va_end(copy);
		if (!gFatals) {
			snprintf(gFirstFatal, sizeof(gFirstFatal), "%s", msg);
		}
		++gFatals;
		if (runningSettled()) {
			++gFatalsSettled;
		}
		if (getenv("MGBA_LOCKSTEP_DIAGNOSE")) {
			diagnoseFatal(msg);
		}
	}

	if (!getenv("MGBA_LOCKSTEP_VERBOSE")) {
		return;
	}
	fprintf(stderr, "%s: ", mLogCategoryName(category));
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
}

static struct mLogger gQuietLogger = { .log = quietLog };

/* ------------------------------------------------------------------ consoles */

struct Console {
	struct mCore* core;
	struct GBASIOLockstepDriver driver;
	struct mLockstepUser user;
	mColor* video;
	uint8_t* rom;
	int index;
	int playerId;
	bool asleep;

	/* observed, for the "did we actually reach the cable" check */
	unsigned long sleeps;
	unsigned long wakes;
};

static struct Console* fromUser(struct mLockstepUser* user) {
	return (struct Console*) ((char*) user - offsetof(struct Console, user));
}

/*
 * The whole policy surface. Serial here: sleeping only records that this
 * console may not run, and the scheduler below honours it. A threaded paired
 * core would block a thread in the same place; nothing else would differ, which
 * is the property that lets a serial Brick and a threaded A30 interoperate.
 */
static void userSleep(struct mLockstepUser* user) {
	struct Console* c = fromUser(user);
	c->asleep = true;
	++c->sleeps;
	if (c == gRunning) {
		struct GBA* gba = (struct GBA*) c->core->board;
		gRunningSleptAt = mTimingCurrentTime(&gba->timing);
	}
}

static void userWake(struct mLockstepUser* user) {
	struct Console* c = fromUser(user);
	c->asleep = false;
	++c->wakes;
}

static int userRequestedId(struct mLockstepUser* user) {
	return fromUser(user)->index;
}

static void userPlayerIdChanged(struct mLockstepUser* user, int id) {
	fromUser(user)->playerId = id;
}

/*
 * Called from the log hook when mGBA declares a desync. The question it answers:
 * was the console that provoked this already asleep, and if so how many cycles
 * had it executed since sleeping? A blocking `sleep` (mLockstepThreadUser) stops
 * the CPU on the spot; a cooperative one returns, and the core keeps running to
 * the next event boundary.
 */
static int runningSettled(void) {
	if (!gRunning) {
		return 0;
	}
	return (int) gRunning->core->frameCounter(gRunning->core) >= SETTLE_FRAMES;
}

static void diagnoseFatal(const char* msg) {
	if (!gRunning) {
		fprintf(stderr, "  [fatal] %s (no console executing)\n", msg);
		return;
	}
	struct GBA* gba = (struct GBA*) gRunning->core->board;
	int now = mTimingCurrentTime(&gba->timing);
	fprintf(stderr, "  [fatal] frame=%u console%i asleep=%s delta=%d cycles :: %s\n",
	        gRunning->core->frameCounter(gRunning->core), gRunning->index,
	        gRunning->asleep ? "YES" : "no", now - gRunningSleptAt, msg);
	gRunningCycleNow = now;
}

static bool consoleInit(struct Console* c, struct GBASIOLockstepCoordinator* coordinator, int index) {
	memset(c, 0, sizeof(*c));
	c->index = index;
	c->playerId = -1;

	c->core = GBACoreCreate();
	if (!c->core) {
		fprintf(stderr, "console %i: GBACoreCreate failed\n", index);
		return false;
	}
	/* Before init, not after: _GBACoreReset reads config, and an uninitialised
	 * config hash table faults rather than returning a default. */
	mCoreInitConfig(c->core, NULL);
	if (!c->core->init(c->core)) {
		fprintf(stderr, "console %i: core init failed\n", index);
		return false;
	}

	unsigned w, h;
	c->core->baseVideoSize(c->core, &w, &h);
	c->video = calloc(w * h, sizeof(mColor));
	c->core->setVideoBuffer(c->core, c->video, w);

	c->rom = malloc(ROM_SIZE);
	buildTestROM(c->rom);
	struct VFile* vf = VFileFromMemory(c->rom, ROM_SIZE);
	if (!c->core->loadROM(c->core, vf)) {
		fprintf(stderr, "console %i: loadROM rejected the test ROM\n", index);
		return false;
	}

	c->core->reset(c->core);

	c->user.sleep = userSleep;
	c->user.wake = userWake;
	c->user.requestedId = userRequestedId;
	c->user.playerIdChanged = userPlayerIdChanged;

	/*
	 * Order matters: Attach only sets driver->coordinator, and GBASIOSetDriver
	 * immediately calls the driver's init -> reset, which registers the player
	 * with that coordinator and schedules the first lockstep event. Attaching
	 * afterwards would reset against a NULL coordinator.
	 */
	GBASIOLockstepDriverCreate(&c->driver, &c->user);
	GBASIOLockstepCoordinatorAttach(coordinator, &c->driver);

	struct GBA* gba = (struct GBA*) c->core->board;
	GBASIOSetDriver(&gba->sio, &c->driver.d);
	return true;
}

static void consoleDeinit(struct Console* c) {
	if (c->core) {
		mCoreConfigDeinit(&c->core->config);
		c->core->deinit(c->core);
	}
	free(c->video);
	free(c->rom);
}

/* -------------------------------------------------------------------- hashing */

static uint64_t fnv1a(const void* data, size_t size) {
	const uint8_t* p = data;
	uint64_t h = 0xCBF29CE484222325ULL;
	size_t i;
	for (i = 0; i < size; ++i) {
		h ^= p[i];
		h *= 0x100000001B3ULL;
	}
	return h;
}

static uint64_t hashConsole(struct Console* c) {
	size_t size = c->core->stateSize(c->core);
	void* state = calloc(1, size);
	uint64_t h = 0;
	if (c->core->saveState(c->core, state)) {
		h = fnv1a(state, size);
	}
	free(state);
	return h;
}

/* ------------------------------------------------------------------ scheduler */

struct RunResult {
	uint64_t hash[NCONSOLES];
	unsigned long steps;
	unsigned long sleeps;
	unsigned long fatals;
	unsigned long fatalsSettled;
	unsigned long transfers;
	bool ok;
};

/*
 * Cooperative, and deliberately dumb: it owns no quantum of its own. It runs a
 * console only while the cable says that console is awake, and the cable ends
 * each slice itself (GBASIOLockstepPlayerSleep zeroes cpu->nextEvent and raises
 * an interrupt, so runLoop returns at the event boundary). runFrame explicitly
 * loops over runLoop until video advances, ignoring that boundary; using it here
 * lets a sleeping console continue. Inventing a quantum here - "advance A one
 * frame, then B one frame" - would be deterministic and *differently*
 * deterministic from a threaded implementation, which is the divergence this
 * harness exists to make visible.
 */
static struct RunResult runPair(int frames, uint32_t* keyScript, int scriptLen) {
	struct RunResult r;
	memset(&r, 0, sizeof(r));

	gFatals = 0;
	gFatalsSettled = 0;
	gTransfers = 0;
	gFirstFatal[0] = '\0';

	struct GBASIOLockstepCoordinator coordinator;
	GBASIOLockstepCoordinatorInit(&coordinator);

	struct Console consoles[NCONSOLES];
	int i;
	for (i = 0; i < NCONSOLES; ++i) {
		if (!consoleInit(&consoles[i], &coordinator, i)) {
			return r;
		}
	}

	unsigned long steps = 0;
	unsigned long guard = (unsigned long) frames * 4096 + 65536;
	uint32_t lastTransferFrame = UINT32_MAX;

	while (steps < guard) {
		/* A libretro call produces the primary's visible frame. The secondary
		 * may legitimately be asleep one frame behind once the cable is idle. */
		if ((int) consoles[0].core->frameCounter(consoles[0].core) >= frames &&
		    !coordinator.waiting && !coordinator.transferActive) {
			r.ok = true;
			break;
		}
		bool ran = false;

		for (i = 0; i < NCONSOLES; ++i) {
			struct Console* c = &consoles[i];
			if ((int) c->core->frameCounter(c->core) >= frames) {
				continue;
			}
			if (c->asleep) {
				continue;
			}

			/* One explicit primary start request per settled frame. This invokes
			 * the same GBASIOWriteSIOCNT path as an emulated MMIO write, while
			 * keeping the stimulus independent of CPU scheduling. */
			uint32_t frame = c->core->frameCounter(c->core);
			if (c->playerId == 0 && frame >= SETTLE_FRAMES && frame + 1 < (uint32_t) frames &&
			    frame != lastTransferFrame && !coordinator.waiting &&
			    !coordinator.transferActive) {
				struct GBA* gba = (struct GBA*) c->core->board;
				if (!(gba->sio.siocnt & 0x0080)) {
					GBASIOWriteSIOCNT(&gba->sio, gba->sio.siocnt | 0x0080);
					lastTransferFrame = frame;
				}
			}
			/* A coordinator callback above may have put the primary to sleep.
			 * Yield immediately; entering the CPU after that point is precisely
			 * the cooperative-scheduler desync this harness guards against. */
			if (c->asleep) {
				continue;
			}

			/* Fixed input, a pure function of the frame index. */
			uint32_t keys = keyScript[c->core->frameCounter(c->core) % scriptLen];
			c->core->setKeys(c->core, keys);

			gRunning = c;
			c->core->runLoop(c->core);
			gRunning = NULL;
			ran = true;
			++steps;
		}

		if (!ran) {
			/* Everyone still short of the target frame is asleep: the cable has
			 * no one to wake and the pair is wedged. */
			fprintf(stderr, "deadlock: all consoles asleep at step %lu; waiting=%08x active=%i",
			        steps, coordinator.waiting, coordinator.transferActive);
			for (i = 0; i < NCONSOLES; ++i) {
				struct GBASIOLockstepPlayer* player = TableLookup(
				        &coordinator.players, consoles[i].driver.lockstepId);
				fprintf(stderr, " c%i(frame=%u user=%i player=%i id=%i)", i,
				        consoles[i].core->frameCounter(consoles[i].core),
				        consoles[i].asleep, player ? player->asleep : -1,
				        consoles[i].playerId);
			}
			fputc('\n', stderr);
			break;
		}
	}

	if (steps >= guard) {
		fprintf(stderr, "guard tripped after %lu steps\n", steps);
	}

	for (i = 0; i < NCONSOLES; ++i) {
		r.hash[i] = hashConsole(&consoles[i]);
		r.sleeps += consoles[i].sleeps;
	}
	r.steps = steps;
	r.fatals = gFatals;
	r.fatalsSettled = gFatalsSettled;
	r.transfers = gTransfers;

	for (i = 0; i < NCONSOLES; ++i) {
		consoleDeinit(&consoles[i]);
	}
	GBASIOLockstepCoordinatorDeinit(&coordinator);
	return r;
}

/* ----------------------------------------------------------------------- main */

int main(int argc, char** argv) {
	mLogSetDefaultLogger(&gQuietLogger);

	int runs = argc > 1 ? atoi(argv[1]) : 3;
	int frames = argc > 2 ? atoi(argv[2]) : 60;
	if (runs < 2) {
		runs = 2;
	}

	/* A fixed, arbitrary-looking input script. Same every run by construction. */
	uint32_t keyScript[] = { 0x0000, 0x0001, 0x0002, 0x0000, 0x0010, 0x0008, 0x0000, 0x0003 };
	int scriptLen = (int) (sizeof(keyScript) / sizeof(*keyScript));

	struct RunResult first;
	memset(&first, 0, sizeof(first));
	int run;
	int failures = 0;

	for (run = 0; run < runs; ++run) {
		struct RunResult r = runPair(frames, keyScript, scriptLen);
		if (!r.ok) {
			fprintf(stderr, "  run %i: did not complete %i frames\n", run, frames);
			++failures;
			continue;
		}

		printf("  run %-2i steps=%-6lu sleeps=%-6lu transfers=%-6lu fatal=%lu(%lu settled) hash0=%016llx hash1=%016llx\n",
		       run, r.steps, r.sleeps, r.transfers, r.fatals, r.fatalsSettled,
		       (unsigned long long) r.hash[0], (unsigned long long) r.hash[1]);

		if (run == 0) {
			first = r;
			if (r.fatals && !getenv("MGBA_LOCKSTEP_ALLOW_FATAL")) {
				fprintf(stderr, "  mGBA logged %lu FATAL lines (%lu after attach); first was:\n    %s\n"
				                "  the pair completed and hashed reproducibly anyway, which is\n"
				                "  the point - a desync both devices reach identically is still\n"
				                "  a desync. Zero FATAL assertions are permitted.\n",
				        r.fatals, r.fatalsSettled, gFirstFatal);
				++failures;
			}
			if (r.sleeps == 0) {
				fprintf(stderr, "  the run never reached the cable: no console ever slept.\n"
				                "  the ROM or the attach is wrong, and a reproducibility\n"
				                "  result from this would be meaningless.\n");
				++failures;
			}
			unsigned long expectedTransfers = frames > SETTLE_FRAMES + 1
			        ? (unsigned long) (frames - SETTLE_FRAMES - 1) : 0;
			if (r.transfers < expectedTransfers) {
				fprintf(stderr, "  only %lu transfers completed in %i frames; expected at least %lu.\n"
				                "  transfer-path coverage is too thin for this result to count.\n",
				        r.transfers, frames, expectedTransfers);
				++failures;
			}
		} else {
			int i;
			for (i = 0; i < NCONSOLES; ++i) {
				if (r.hash[i] != first.hash[i]) {
					fprintf(stderr, "  run %i console %i diverged: %016llx != %016llx\n",
					        run, i, (unsigned long long) r.hash[i],
					        (unsigned long long) first.hash[i]);
					++failures;
				}
			}
			if (r.steps != first.steps) {
				fprintf(stderr, "  run %i took %lu steps, run 0 took %lu:"
				                " the schedule itself is not reproducible\n",
				        run, r.steps, first.steps);
				++failures;
			}
		}
	}

	return failures ? 1 : 0;
}
