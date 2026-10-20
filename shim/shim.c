/*
 * netplay shim - a libretro core that wraps another libretro core.
 *
 * minarch loads this instead of the real core; we dlopen the real one and
 * forward every entry point to it. Sitting between the frontend and the core
 * gives us the seams netplay needs without patching minarch:
 *
 *   - retro_set_environment  we answer SET_NETPACKET_INTERFACE ourselves, so
 *                            GB/GBA link works on a stock frontend
 *   - retro_set_input_state  port 1 can be served from the network
 *   - retro_run              frame advance can be gated on the peer
 *   - retro_(un)serialize    hide frontend save states during sessions while
 *                            retaining protocol-owned state synchronization
 *
 * With no session armed this is a pure passthrough and the game behaves
 * exactly as it would without us.
 *
 * IMPORTANT: this .so must be installed under the real core's filename.
 * minarch derives core.name from basename(core_path) truncated at the last
 * underscore (ma_core.c Core_getName), and core.name feeds both config_dir
 * and states_dir. Loading us as "netplay_shim_libretro.so" would relocate the
 * user's save states. The launcher copies us to <realcore>_libretro.so.
 */

#include <dlfcn.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gnu/libc-version.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "libretro.h"
#include "gambatte_dual.h"
#include "romscan.h"
#include <zlib.h>
#include "netlink.h"
#include "overlay.h"
#include "sha256.h"

#define SHIM_ENV_REAL_CORE "NETPLAY_REAL_CORE"
#define SHIM_ENV_SESSION   "NETPLAY_SESSION"
#define SHIM_ENV_NOTICE    "NETPLAY_CORE_NOTICE"
#define SHIM_ENV_NO_DUAL   "NETPLAY_DUAL_DISABLE"
/* Written when instanced link cannot be used for this pairing; the launcher
 * relaunches once with the network-serial core. See request_serial_fallback. */
#define SHIM_ENV_FALLBACK  "NETPLAY_SERIAL_FALLBACK"
#define STARTUP_NOTICE_MS  2000

/* Bound on packets handed to the core per frame. Without a cap, a peer that
 * has raced ahead can starve the frame. */
#define MAX_PACKETS_PER_FRAME 64

/* Frames of input delay for shared-screen netplay. Each frame buys ~16ms of
 * tolerance at the cost of the same in input lag. Measured RTT here averages
 * ~35ms but spikes past 200ms, and every spike that outruns the window becomes
 * a visible stall - so this is tunable per session via `input_delay=N`. */
#define INPUT_DELAY_DEFAULT 6

/* How often the two sides compare state. A divergence is otherwise silent. */
#define HASH_INTERVAL 300

/* How often to report pacing, in advanced frames (~10s of game time). */
#define PACING_INTERVAL 600

/* Consecutive stalled frames before the "waiting" overlay appears. A lockstep
 * session misses its deadline by a frame or two constantly, and drawing the
 * message for each of those turns a barely-perceptible hitch into flashing
 * text across the whole screen. Below this the previous frame simply stays up,
 * which is what a dropped frame should look like. ~200ms at 60fps. */
#define STALL_OVERLAY_FRAMES 12

/* Recovery is a bounded transaction. The frame counter jumps beyond every
 * input that could have been queued before the barrier, so a delayed pre-reset
 * packet cannot be mistaken for input on the recovered timeline. */
#define RECOVERY_TIMEOUT_MS 10000
#define RECOVERY_FRAME_JUMP 512
#define PEER_WAIT_TIMEOUT_MS 30000
/* How long one call into a link core may take before the session identifies it
 * as blocked. Queue silence is deliberately irrelevant: a Gen 3 trade can be
 * quiet while a player reads a menu, and an empty receive queue says nothing
 * about whether retro_run returned. Tunable while the threshold is still a
 * judgement call rather than a measurement. */
#define LINK_STARVE_TIMEOUT_MS 5000
#define MAX_DESYNC_RECOVERIES 3
#define DESYNC_WINDOW_MS 60000

//////////////////////////////////////////////////////////////////////////////
// logging - minarch redirects the emulator's stderr to $LOGS_PATH/<TAG>.txt
//////////////////////////////////////////////////////////////////////////////

/* Wall-clock prefix, "HH:MM:SS.mmm".
 *
 * Wall clock rather than elapsed because the hard bugs here are two-device
 * stories - a host freezing while the client sits in (waiting...), a watchdog
 * counting strikes while the app believes the session ended. Matching stamps
 * across both handhelds' logs and the watchdog's own log is worth more than
 * saving a subtraction, and these devices keep time across power-off.
 *
 * Milliseconds because the interesting gaps are sub-second: the handshake, the
 * state transfer, the dlopen probe.
 *
 * localtime_r is ~222ns against ~1425ns for the write it accompanies, and it is
 * called only when the second changes - so the common case is one gettimeofday.
 */
static void log_stamp(char* out, int len) {
	struct timeval tv;
	gettimeofday(&tv, NULL);

	static time_t cached_sec = 0;
	static char   cached_hms[32] = "00:00:00";
	if (tv.tv_sec != cached_sec) {
		struct tm tm;
		localtime_r(&tv.tv_sec, &tm);
		strftime(cached_hms, sizeof(cached_hms), "%H:%M:%S", &tm);
		cached_sec = tv.tv_sec;
	}
	snprintf(out, len, "%s.%03d", cached_hms, (int)(tv.tv_usec / 1000));
}

static void shim_log(const char* fmt, ...) {
	char ts[48];
	log_stamp(ts, sizeof(ts));

	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[%s] [netplay-shim] ", ts);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fflush(stderr);
}

//////////////////////////////////////////////////////////////////////////////
// the wrapped core
//////////////////////////////////////////////////////////////////////////////

typedef struct {
	void* handle;

	void     (*init)(void);
	void     (*deinit)(void);
	unsigned (*api_version)(void);
	void     (*get_system_info)(struct retro_system_info*);
	void     (*get_system_av_info)(struct retro_system_av_info*);
	void     (*set_environment)(retro_environment_t);
	void     (*set_video_refresh)(retro_video_refresh_t);
	void     (*set_audio_sample)(retro_audio_sample_t);
	void     (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void     (*set_input_poll)(retro_input_poll_t);
	void     (*set_input_state)(retro_input_state_t);
	void     (*set_controller_port_device)(unsigned, unsigned);
	void     (*reset)(void);
	void     (*run)(void);
	size_t   (*serialize_size)(void);
	bool     (*serialize)(void*, size_t);
	bool     (*unserialize)(const void*, size_t);
	void     (*cheat_reset)(void);
	void     (*cheat_set)(unsigned, bool, const char*);
	bool     (*load_game)(const struct retro_game_info*);
	bool     (*load_game_special)(unsigned, const struct retro_game_info*, size_t);
	void     (*unload_game)(void);
	unsigned (*get_region)(void);
	void*    (*get_memory_data)(unsigned);
	size_t   (*get_memory_size)(unsigned);

	/* Optional bmpriest/gambatte-libretro dual-instance ABI. */
	unsigned (*dual_get_abi_version)(void);
	uint64_t (*dual_get_capabilities)(void);
	bool     (*dual_set_visible_console)(unsigned);
	unsigned (*dual_get_visible_console)(void);
	void*    (*dual_get_memory_data)(unsigned, unsigned);
	size_t   (*dual_get_memory_size)(unsigned, unsigned);
	bool     (*dual_reset_console)(unsigned);
	bool     (*dual_is_checkpoint_safe)(void);
	size_t   (*dual_serialize_size)(void);
	bool     (*dual_serialize)(void*, size_t);
	bool     (*dual_unserialize)(const void*, size_t);
	bool     (*dual_set_clock_epochs)(uint64_t, uint64_t);
} Core;

static Core core;

/* Gambatte instanced link: one paired core (gambatte_dual_libretro.so, dual ABI
 * v1) holds both logical consoles and an in-process serial coordinator. Only
 * delayed, frame-indexed controller inputs cross Wi-Fi. */
static int dual_link_mode;
static unsigned dual_local_console;
static unsigned dual_peer_console;
static int dual_bootstrapped;
static int dual_identity_sent;
static int dual_identity_checked;
static int dual_verdict_sent;
static int dual_verdict_agreed;
/* Which peer cartridge the pair is currently built from, "" meaning our own
 * loaded into both consoles. Compared rather than assumed, because a peer that
 * reconnects may have relaunched a different game. */
static char dual_content_rom[512];
static int dual_content_built;
static int dual_state_sent;
static int dual_state_loaded;
static int dual_checkpoint_sent;
static int dual_checkpoint_loaded;
static int dual_ready_sent;
static int dual_failed;
static char dual_error[128];
static uint32_t dual_frame;
static uint32_t dual_generation;
#define DUAL_READY_EPOCH 0x4455414Cu /* DUAL */
static uint32_t dual_primary_buttons;
static uint32_t dual_shadow_buttons;
#define DUAL_INPUT_RESET 0x80000000u
static int dual_reset_pending;
static struct timeval dual_wait_since;
/* Paired-state agreement. Both replicas hold the same two consoles, so both can
 * hash them - there is no authority here the way there is in shared screen, and
 * nothing to recover from either: an in-process cable has no resync protocol.
 * The value is telling the players immediately instead of leaving them to
 * discover it by watching each other's console do something it never did. */
/* Taken once and reused, at the same protocol point on both devices.
 *
 * The paired state is the only thing the two replicas can be compared by, so
 * everything here is arranged so both devices perform the same operations in
 * the same order. That discipline was worth keeping even after the reason for
 * needing it turned out to be a core bug: gambatte serialized uninitialised
 * SaveState members for any non-Sachen cartridge, which made saveState() vary
 * per call and per process. Fixed upstream; this stays because a protocol that
 * only works when state operations are free is one bad assumption from
 * silently diverging again. */
static size_t dual_state_size;
static void* dual_hash_buf;
static size_t dual_hash_buf_len;
static uint32_t dual_hash_skips;
static uint64_t dual_pair_run_us;
static uint32_t dual_pair_run_max_us;

/* Linked cartridges do not have to match: Red links to Blue, Seasons to Ages.
 * What instanced play does require is that each handheld can run *both*
 * consoles locally, because neither ROM ever crosses the network. When the
 * peer's cartridge is not installed here, the session is demoted to ordinary
 * network serial - see request_serial_fallback(). */
static char dual_local_rom_path[512];
static uint32_t dual_local_rom_size;
/* CRC32 of the same bytes, so a peer with a zipped library can find this
 * cartridge without decompressing every candidate it holds. */
static uint32_t dual_local_rom_crc32;
/* This device's clock as declared to the peer. Kept rather than re-read, so
 * that the epochs both devices install are the ones they exchanged and not two
 * later readings that would no longer be the same pair. */
static uint64_t dual_local_wall_clock;
static char dual_peer_rom_path[512];
/* Set when the peer's cartridge was found inside an archive: the reload has to
 * inflate it rather than read the file, and the frontend must not be handed the
 * .zip path as if it were a ROM. */
static RomScanEntry dual_peer_rom_zip;
static int dual_peer_rom_zipped;
static int dual_peer_rom_found;
static int dual_same_rom;
static int dual_demoted;
/* Our own cartridge, kept because retro_game_info.data belongs to the frontend
 * and a linked pair has to be handed to the core again as two contents. */
static void* dual_own_rom;
static size_t dual_own_rom_len;

static bool hash_content_path(Sha256* hash, const char* path, unsigned depth);
static uint32_t timeval_delta_us(const struct timeval* from, const struct timeval* to);
static long elapsed_ms(const struct timeval* since);
static long peer_wait_timeout_ms(void);
static void recovery_fail(const char* why);
static bool failure_input_tick(void);
static void failure_overlay_message(char* out, size_t len);

static bool read_whole_file(const char* path, void** out, size_t* out_len) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	bool ok = fseek(f, 0, SEEK_END) == 0;
	long len = ok ? ftell(f) : -1;
	ok = ok && len > 0 && len <= 8L * 1024L * 1024L && fseek(f, 0, SEEK_SET) == 0;
	void* buf = ok ? malloc((size_t)len) : NULL;
	ok = buf && fread(buf, 1, (size_t)len, f) == (size_t)len;
	fclose(f);
	if (!ok) { free(buf); return false; }
	*out = buf;
	*out_len = (size_t)len;
	return true;
}

// Callbacks the frontend handed us. We pass our own wrappers to the real core
// and forward through these, so we can intercept in either direction.
static retro_environment_t        fe_environment;
static retro_video_refresh_t      fe_video_refresh;
static retro_audio_sample_t       fe_audio_sample;
static retro_audio_sample_batch_t fe_audio_sample_batch;
static retro_input_poll_t         fe_input_poll;
static retro_input_state_t        fe_input_state;

// Shared-screen netplay: one instance each, same game, inputs synced. Distinct
// from link play, where the cores exchange their own serial/RFU traffic.
static int      netplay_mode = 0;
static int      input_delay = INPUT_DELAY_DEFAULT;
static int      netplay_synced = 0;
static uint32_t netplay_frame = 0;
static uint32_t timeline_start_frame = 0;
static uint32_t frame_buttons[2];      // [0] = host's, [1] = client's
static uint32_t local_inputs[256];     // our own, kept for the delay window
static uint32_t last_scheduled;        // highest frame we have already sent
static int      input_scheduled;

typedef enum {
	RECOVERY_IDLE = 0,
	RECOVERY_CLIENT_WAIT_BEGIN,
	RECOVERY_CLIENT_WAIT_STATE,
	RECOVERY_CLIENT_WAIT_COMMIT,
	RECOVERY_HOST_WAIT_ACK,
} RecoveryPhase;

static RecoveryPhase recovery_phase;
static uint32_t recovery_epoch;
static uint32_t recovery_resume_frame;
static unsigned recovery_count;
static int recovery_failed;
static char recovery_error[96];
static struct timeval recovery_started;
static uint32_t connection_generation;
static int connection_identity_sent;
static int connection_identity_checked;
static int connection_sync_pending;
static int netplay_ever_synced;
static int solo_mode;
static struct timeval peer_missing_since;
static struct timeval input_stall_since;
static struct timeval desync_window_started;
static unsigned desync_recoveries;
static uint32_t failure_buttons_previous;
typedef enum {
	FAILURE_WAIT = 0,
	FAILURE_SOLO,
	FAILURE_EXIT,
	FAILURE_CHOICE_COUNT,
} FailureChoice;
static FailureChoice failure_choice = FAILURE_SOLO;
static int exit_requested;
static int host_reset_pending;
static int recovery_promote_checkpoint;
static NetLinkRecoveryKind recovery_kind = NETLINK_RECOVERY_SYNC;

static uint8_t rom_sha256[32];
static int rom_hash_ready;
static char session_id[65];
static char checkpoint_path[192];

static void* checkpoint_candidate;
static size_t checkpoint_candidate_len;
static uint32_t checkpoint_candidate_frame;
static uint32_t checkpoint_candidate_hash;

#define HASH_HISTORY 8
typedef struct {
	uint32_t frame;
	uint32_t hash;
	int valid;
} StateHash;
static StateHash own_hashes[HASH_HISTORY];
static StateHash pending_peer_hashes[HASH_HISTORY];

// Pacing stats. Without these a "laggy" session is unattributable: from the
// outside a core that cannot hit 60fps and a peer whose inputs arrive late look
// identical - both just advance slowly. Frames-per-second answers the first,
// stall share answers the second.
static uint32_t stat_frames;      // advanced since the last report
static uint32_t stat_stalls;      // frames spent waiting for peer input
static uint32_t stat_stall_max;   // longest single wait, in frames
static uint32_t stall_run;        // current consecutive stall length
static struct timeval stat_since;

// No session file -> pure passthrough. Read once at load; the launcher decides
// whether a session is armed before minarch ever starts.
static int session_active = 0;
/* Core-managed auxiliary saves (memory cards, high scores, etc.) bypass
 * RETRO_MEMORY_SAVE_RAM. A shared-screen guest sees a fresh process-local
 * directory in /tmp so those files can function during play but never touch
 * the user's persistent save tree. */
static char guest_save_dir[256];
static char startup_notice[96];
static struct timeval startup_notice_started;
static unsigned startup_notice_frame;

//////////////////////////////////////////////////////////////////////////////
// CPU frequency
//
// A paired session emulates two consoles per device, and on the A30 that was
// being done at 648MHz of an available 1344MHz. NextUI's default CPU Speed is
// "Auto", which on my282 means the conservative governor with up_threshold=80
// over a 10ms window - and a frame-paced emulator never looks busy to that. It
// computes for two or three milliseconds and then waits for the next frame, so
// measured load sits near 12-20% and the governor stays at its floor
// indefinitely. Under four full-load spinners the same device ramps to 1344MHz
// in about two seconds, so the headroom is real; nothing in a well-behaved
// emulator ever asks for it.
//
// So the frequency is pinned here rather than left to a heuristic that is
// measuring the wrong thing. Doing it through the frontend's own core option
// would not work: minarch applies minarch_cpu_speed after the core is loaded,
// so anything set earlier is overwritten. This runs from the frame loop, after
// that has happened, and re-asserts periodically in case a menu round trip
// puts it back.
//
// Only for an armed session, and restored at teardown - a device left pinned
// after play would quietly cost battery for the rest of the day.
//////////////////////////////////////////////////////////////////////////////

#define CPU_MAX_POLICIES 8

static struct {
	char path[160];
	char governor[32];
	char min_freq[32];
} cpu_saved[CPU_MAX_POLICIES];
static int cpu_saved_count;
static int cpu_pin_failed;

static const char* cpufreq_root(void) {
	const char* root = getenv("NETPLAY_CPUFREQ_ROOT");
	return root && root[0] ? root : "/sys/devices/system/cpu";
}

static bool cpu_policy_path(char* out, size_t out_size,
                            const char* policy, const char* leaf) {
	size_t base_len = strnlen(policy, sizeof(cpu_saved[0].path));
	size_t leaf_len = strlen(leaf);
	if (base_len == sizeof(cpu_saved[0].path) || base_len + 1 + leaf_len + 1 > out_size)
		return false;
	memcpy(out, policy, base_len);
	out[base_len] = '/';
	memcpy(out + base_len + 1, leaf, leaf_len + 1);
	return true;
}

static bool read_line_file(const char* path, char* out, size_t len) {
	FILE* f = fopen(path, "r");
	if (!f) return false;
	bool ok = fgets(out, (int)len, f) != NULL;
	fclose(f);
	if (!ok) return false;
	char* nl = strpbrk(out, "\r\n");
	if (nl) *nl = '\0';
	return out[0] != '\0';
}

static bool write_line_file(const char* path, const char* value) {
	/* These are commonly 0444 even for root, which is why NextUI's own
	 * PLAT_setCPUSpeed widens them before writing. */
	struct stat info;
	if (stat(path, &info) == 0 && !(info.st_mode & S_IWUSR))
		chmod(path, (info.st_mode & 0777) | S_IWUSR);
	FILE* f = fopen(path, "w");
	if (!f) return false;
	bool ok = fputs(value, f) >= 0;
	if (fclose(f) != 0) ok = false;
	return ok;
}

/* Pin every policy to its own maximum. Idempotent: safe to call repeatedly,
 * and only reports once. */
static void cpu_pin_performance(void) {
	if (cpu_pin_failed) return;

	const bool first = cpu_saved_count == 0;
	int pinned = 0;
	for (int i = 0; i < CPU_MAX_POLICIES; i++) {
		char dir[128], path[192], value[32], maxfreq[32];
		snprintf(dir, sizeof(dir), "%s/cpu%d/cpufreq", cpufreq_root(), i);
		snprintf(path, sizeof(path), "%s/scaling_governor", dir);
		if (!read_line_file(path, value, sizeof(value))) continue;

		if (first) {
			snprintf(cpu_saved[cpu_saved_count].path, sizeof(cpu_saved[0].path), "%s", dir);
			snprintf(cpu_saved[cpu_saved_count].governor,
			         sizeof(cpu_saved[0].governor), "%s", value);
			char minpath[192];
			snprintf(minpath, sizeof(minpath), "%s/scaling_min_freq", dir);
			if (!read_line_file(minpath, cpu_saved[cpu_saved_count].min_freq,
			                    sizeof(cpu_saved[0].min_freq)))
				cpu_saved[cpu_saved_count].min_freq[0] = '\0';
			cpu_saved_count++;
		}

		if (!strcmp(value, "performance")) { pinned++; continue; }

		/* Raise the floor as well as the governor. On a kernel that refuses an
		 * unknown governor the floor alone still buys most of the difference. */
		char maxpath[192], minpath[192];
		snprintf(maxpath, sizeof(maxpath), "%s/scaling_max_freq", dir);
		snprintf(minpath, sizeof(minpath), "%s/scaling_min_freq", dir);
		if (read_line_file(maxpath, maxfreq, sizeof(maxfreq)))
			write_line_file(minpath, maxfreq);
		if (write_line_file(path, "performance")) pinned++;
	}

	if (first) {
		if (!cpu_saved_count) {
			cpu_pin_failed = 1;
			shim_log("no cpufreq policies under %s; leaving CPU scaling alone\n",
			         cpufreq_root());
			return;
		}
		char now[32] = "?";
		char path[192];
		snprintf(path, sizeof(path), "%s/scaling_governor", cpu_saved[0].path);
		read_line_file(path, now, sizeof(now));
		shim_log("pinned %d of %d CPU policies to performance (was '%s', now '%s')\n",
		         pinned, cpu_saved_count, cpu_saved[0].governor, now);
	}
}

static void cpu_restore(void) {
	/* Keep the array bound explicit here. cpu_saved_count is populated by the
	 * bounded discovery loop, but stating that invariant in this consumer also
	 * prevents a corrupted count from walking adjacent saved paths. */
	int restore_count = cpu_saved_count;
	if (restore_count > CPU_MAX_POLICIES) restore_count = CPU_MAX_POLICIES;
	for (int i = 0; i < restore_count; i++) {
		char path[192];
		if (cpu_policy_path(path, sizeof(path), cpu_saved[i].path, "scaling_governor"))
			write_line_file(path, cpu_saved[i].governor);
		if (cpu_saved[i].min_freq[0]) {
			if (cpu_policy_path(path, sizeof(path), cpu_saved[i].path, "scaling_min_freq"))
				write_line_file(path, cpu_saved[i].min_freq);
		}
	}
	if (restore_count)
		shim_log("restored CPU scaling to '%s'\n", cpu_saved[0].governor);
	cpu_saved_count = 0;
}

static void show_runtime_notice(const char* message) {
	snprintf(startup_notice, sizeof(startup_notice), "%s", message ? message : "");
	startup_notice_started.tv_sec = startup_notice_started.tv_usec = 0;
	startup_notice_frame = 0;
}

// Core options forced by the session, as `option.<key>=<value>` lines. gpSP's
// link emulation is per-game (gpsp_serial: mul_poke, mul_aw1, mul_aw2, rfu) and
// gambatte encodes a peer IP as twelve single-digit options, so the cap is not
// merely generous - it has to hold a whole address plus mode and port.
// does nothing on "auto" for a game it does not recognise, so a link session has
// to be able to pin it - and both peers must agree.
#define MAX_OPTION_OVERRIDES 24
static struct { char key[64]; char value[64]; } option_override[MAX_OPTION_OVERRIDES];
static int option_override_count = 0;
static int option_update_pending = 0;

// The core's netpacket interface, if it registered one. This is the whole point
// of the shim for link play: the frontend never sees the request, so GB/GBA
// link works on a completely stock minarch.
// Last frame the core produced, kept so a paused session has something to show.
// Skipping core.run() means video_refresh is never called, and minarch simply
// re-presents whatever was on screen - a freeze indistinguishable from a crash.
static void*    last_frame;
static size_t   last_frame_cap;
static unsigned last_frame_w, last_frame_h;
static size_t   last_frame_pitch;
static unsigned pixel_format = RETRO_PIXEL_FORMAT_RGB565;

// Audio geometry, for feeding silence while paused rather than letting the
// frontend's buffer run dry and crackle.
static double av_fps = 60.0, av_sample_rate = 44100.0;

// Video geometry from the same call. Needed because a pause can happen before
// the core has ever produced a frame, which is the normal case at the start of
// a shared-screen session - there is no last frame to copy, so the status
// message has to be drawn into a buffer we make ourselves.
static unsigned av_base_width, av_base_height;

static struct retro_netpacket_callback core_netpacket;
static int  have_netpacket = 0;   // core registered an interface
static int  netpacket_started = 0; // we have called its start()
static int contains_ci(const char* hay, const char* needle);

// The session file is read twice on purpose: netlink takes the transport keys,
// this takes the option overrides. Keeping them separate beats threading core
// option concerns through the network layer.
static void load_option_overrides(const char* session_path) {
	FILE* f = fopen(session_path, "r");
	if (!f) return;
	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	core.get_system_info(&info);
	bool gambatte = info.library_name && contains_ci(info.library_name, "gambatte");

	char line[256];
	while (fgets(line, sizeof(line), f) && option_override_count < MAX_OPTION_OVERRIDES) {
		char* nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';
		if (strncmp(line, "option.", 7) != 0) continue;

		char* eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';

		const char* k = line + 7;
		const char* v = eq + 1;
		/* The app arms one generic session before a game is selected. Preserve its
		 * Gambatte fields for GB, but do not apply or log them for every unrelated
		 * shared-screen core launched under that session. */
		if (!strncmp(k, "gambatte_", 9) && !gambatte) continue;
		if (strlen(k) >= sizeof(option_override[0].key) ||
		    strlen(v) >= sizeof(option_override[0].value)) {
			shim_log("ignoring over-long option override '%s'\n", k);
			continue;
		}
		strcpy(option_override[option_override_count].key, k);
		strcpy(option_override[option_override_count].value, v);
		shim_log("forcing core option %s=%s\n",
		         option_override[option_override_count].key,
		         option_override[option_override_count].value);
		option_override_count++;
	}
	fclose(f);

	if (option_override_count) option_update_pending = 1;
}

static void load_session_id(const char* path) {
	session_id[0] = checkpoint_path[0] = '\0';
	FILE* f = fopen(path, "r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char* end = strpbrk(line, "\r\n");
		if (end) *end = '\0';
		if (strncmp(line, "session_id=", 11)) continue;
		const char* value = line + 11;
		size_t n = strlen(value);
		if (!n || n >= sizeof(session_id)) break;
		bool safe = true;
		for (size_t i = 0; i < n; i++)
			if (!((value[i] >= '0' && value[i] <= '9') ||
			      (value[i] >= 'a' && value[i] <= 'f') ||
			      (value[i] >= 'A' && value[i] <= 'F'))) safe = false;
		if (safe) snprintf(session_id, sizeof(session_id), "%s", value);
		break;
	}
	fclose(f);
	if (session_id[0])
		snprintf(checkpoint_path, sizeof(checkpoint_path),
		         "/tmp/netplay-host-%s.state", session_id);
}

// Mode is normally derived from the core (see core_wants_link). An explicit
// mode= line still wins, which keeps the session file authoritative for testing
// and leaves a way out if the core list is ever wrong.
//
// Returns 1 for shared screen, 0 for link cable, -1 when the session says
// nothing and the core should decide.
static int session_mode_override(const char* path) {
	FILE* f = fopen(path, "r");
	if (!f) return -1;
	char line[256];
	int v = -1;
	while (fgets(line, sizeof(line), f)) {
		char* nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';
		if (!strcmp(line, "mode=netplay"))    v = 1;
		else if (!strcmp(line, "mode=link"))  v = 0;
	}
	fclose(f);
	return v;
}

// Case-insensitive substring, ASCII only. strcasestr is a GNU extension and
// these cores get built against several libcs.
static int contains_ci(const char* hay, const char* needle) {
	if (!hay || !needle || !*needle) return 0;
	for (; *hay; hay++) {
		const char* h = hay;
		const char* n = needle;
		while (*h && *n) {
			int a = (*h >= 'A' && *h <= 'Z') ? *h + 32 : *h;
			int b = (*n >= 'A' && *n <= 'Z') ? *n + 32 : *n;
			if (a != b) break;
			h++; n++;
		}
		if (!*n) return 1;
	}
	return 0;
}

// Cores that emulate a real cable carry their own serial/RFU traffic over the
// netpacket interface, so they get the link path; everything else gets
// shared-screen netplay, which needs only serialize/unserialize and therefore
// works for any core.
//
// Matched on library_name rather than the file name: the pak stages cores under
// cores/override/<platform>/, and the name the core reports is the thing that
// actually identifies it.
static int core_wants_link(void) {
	static const char* LINK_CORES[] = { "gambatte", "gpsp", "mgba dual" };

	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	core.get_system_info(&info);
	if (!info.library_name) return 0;

	for (size_t i = 0; i < sizeof(LINK_CORES) / sizeof(LINK_CORES[0]); i++) {
		if (contains_ci(info.library_name, LINK_CORES[i])) return 1;
	}
	return 0;
}

static int core_is_gambatte(void) {
	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	core.get_system_info(&info);
	return info.library_name && contains_ci(info.library_name, "gambatte");
}

static int core_is_mgba(void) {
	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	core.get_system_info(&info);
	return info.library_name && contains_ci(info.library_name, "mgba");
}

/*
 * What to answer mGBA when it asks the frontend for a target audio rate.
 *
 * Default is 0: decline, and let mGBA keep its own 65536. That is measured, not
 * assumed - see below - and this knob exists so the measurement can be redone.
 *
 * The idea was that mGBA upsamples GBA's native 32768Hz to 65536 for nothing,
 * since minarch resamples again to 48000 for the device. Answering 32768 makes
 * libretro.c take its `coreSampleRate == targetSampleRate` branch and skip its
 * resampler entirely.
 *
 * Measured on an A30 at 1344MHz, Mario Kart Super Circuit, process-wide:
 *
 *   target 65536 (declining, mGBA's default)   12.63 ms/frame
 *   target 32768                               14.64 ms/frame
 *
 * Backwards, and the reason is instructive. 32768 is the GBA's *reset* value;
 * games write SOUNDBIAS and most then run at 65536 or higher. So mGBA's default
 * already matches what a real game produces and its resampler is already being
 * skipped - answering 32768 does not remove a conversion, it *adds* one, and the
 * ~2ms delta is mGBA's resampler being switched on rather than off. minarch has
 * no same-rate short-circuit either (api.c resample_audio runs unconditionally),
 * so there was never a second pass to collapse.
 *
 * The +49% reported for `audio_out_rate = 32768` elsewhere is a RetroArch
 * number, and RetroArch's audio path is not this one.
 *
 * A game that genuinely runs at 32768 would want that answered instead, and
 * would save the ~2ms. We cannot know which at load time - the environment call
 * lands before the game writes SOUNDBIAS - and mGBA fixes the rate once. Hence
 * the knob rather than a guess.
 */
#define MGBA_TARGET_SAMPLE_RATE 0

static unsigned mgba_target_sample_rate(void) {
	const char* env = getenv("NETPLAY_MGBA_SAMPLE_RATE");
	if (env && *env) {
		long v = strtol(env, NULL, 10);
		if (v >= 0) return (unsigned) v;
	}
	return MGBA_TARGET_SAMPLE_RATE;
}

static int session_instanced_core(const char* path, const char* core_name) {
	FILE* f = fopen(path, "r");
	if (!f) return 0;
	char line[256];
	char key[64];
	snprintf(key, sizeof(key), "instanced_%s=", core_name);
	size_t key_len = strlen(key);
	int enabled = 0;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, key, key_len)) enabled = atoi(line + key_len) != 0;
	}
	fclose(f);
	return enabled;
}

static bool core_has_dual_contract(void) {
	const uint64_t required = GAMBATTE_DUAL_CAP_TWO_CONTENTS |
	                          GAMBATTE_DUAL_CAP_CONSOLE_MEMORY |
	                          GAMBATTE_DUAL_CAP_VISIBLE_CONSOLE |
	                          GAMBATTE_DUAL_CAP_PAIRED_CHECKPOINT |
	                          GAMBATTE_DUAL_CAP_CLOCK_EPOCHS;
	if (!core.dual_get_abi_version || !core.dual_get_capabilities ||
	    !core.dual_set_visible_console || !core.dual_get_memory_data ||
	    !core.dual_get_memory_size || !core.dual_is_checkpoint_safe ||
	    !core.dual_serialize_size || !core.dual_serialize ||
	    !core.dual_unserialize || !core.dual_set_clock_epochs)
		return false;
	return core.dual_get_abi_version() == GAMBATTE_DUAL_ABI_VERSION &&
	       (core.dual_get_capabilities() & required) == required;
}

// Core options that a session cannot work without, applied unless the session
// file already sets them.
//
// pcsx_rearmed's drc_thread compiles recompiler blocks on a worker thread. Until
// a block is ready the emulation thread runs the interpreter instead, and the
// two paths do not agree on cycle timing - so the serialized state depends on
// when the compiler thread happened to finish. Measured: five runs of the same
// binary, same ROM, same frame count produced five different state hashes, and
// disabling this one option made all five identical. Two devices running the
// *same* build would desync, which no amount of pinning or core-shipping could
// have fixed.
//
// Only ever consulted for shared-screen sessions; link-cable cores carry their
// own traffic and never compare state.
static const struct {
	const char* core;
	const char* key;
	const char* value;
	bool session_may_override;
}
REQUIRED_OPTIONS[] = {
	{ "pcsx-rearmed", "pcsx_rearmed_drc_thread", "disabled", true },
	/* PCSX otherwise writes its second, core-managed card behind the frontend's
	 * back. Shared-screen sessions deliberately expose only card 1 through the
	 * libretro save-memory buffer, which lets the host own persistence and lets
	 * the guest use an in-memory copy without touching its filesystem. */
	{ "pcsx-rearmed", "pcsx_rearmed_memcard1", "libretro", false },
	{ "pcsx-rearmed", "pcsx_rearmed_memcard2", "none", false },
};

static void apply_required_options(void) {
	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	core.get_system_info(&info);
	if (!info.library_name) return;

	for (size_t i = 0; i < sizeof(REQUIRED_OPTIONS) / sizeof(REQUIRED_OPTIONS[0]); i++) {
		if (!contains_ci(info.library_name, REQUIRED_OPTIONS[i].core)) continue;

		// The session file wins. Someone testing a theory about this option
		// should not have it silently overwritten.
		int already = -1;
		for (int j = 0; j < option_override_count; j++) {
			if (!strcmp(option_override[j].key, REQUIRED_OPTIONS[i].key)) { already = j; break; }
		}
		if (already >= 0 && REQUIRED_OPTIONS[i].session_may_override) {
			shim_log("session already sets %s - leaving it alone\n", REQUIRED_OPTIONS[i].key);
			continue;
		}
		if (already >= 0) {
			snprintf(option_override[already].value,
			         sizeof(option_override[already].value), "%s", REQUIRED_OPTIONS[i].value);
			shim_log("forcing %s=%s (required for host-only save persistence)\n",
			         REQUIRED_OPTIONS[i].key, REQUIRED_OPTIONS[i].value);
			option_update_pending = 1;
			continue;
		}
		if (option_override_count >= MAX_OPTION_OVERRIDES) {
			shim_log("WARNING: no room to force %s - session may desync\n", REQUIRED_OPTIONS[i].key);
			return;
		}
		snprintf(option_override[option_override_count].key,
		         sizeof(option_override[0].key), "%s", REQUIRED_OPTIONS[i].key);
		snprintf(option_override[option_override_count].value,
		         sizeof(option_override[0].value), "%s", REQUIRED_OPTIONS[i].value);
		shim_log("forcing %s=%s (required for shared-screen netplay in %s)\n",
		         REQUIRED_OPTIONS[i].key, REQUIRED_OPTIONS[i].value, info.library_name);
		option_override_count++;
		option_update_pending = 1;
	}
}

// Both peers must agree on the delay, so it is written by the app into both
// session files rather than negotiated.
/* input_delay=N pins the value; input_delay=auto (or an absent line) leaves it
 * to be derived from the measured round trip during the handshake. A pinned
 * value is still only this device's proposal - see delay negotiation below,
 * where both sides adopt the higher of the two. Agreement matters more than
 * either side's preference, and a pin that could break agreement would be a
 * desync rather than an override. */
static int input_delay_pinned;

static int session_input_delay(const char* path) {
	FILE* f = fopen(path, "r");
	if (!f) return INPUT_DELAY_DEFAULT;
	char line[256];
	int v = INPUT_DELAY_DEFAULT;
	while (fgets(line, sizeof(line), f)) {
		int n;
		if (sscanf(line, "input_delay=%d", &n) == 1 && n >= 1 && n <= 20) {
			v = n;
			input_delay_pinned = 1;
		} else if (!strncmp(line, "input_delay_auto=1", 18)) {
			/* Negotiate, but keep the number above as the fallback for a link
			 * that never answers a probe - the app picks it from the transport
			 * actually in use, which is better than any constant here. */
			input_delay_pinned = 0;
		}
	}
	fclose(f);
	return v;
}

/* Frames of delay the measured link actually needs: one frame per frame of the
 * worst observed round trip, plus one for jitter.
 *
 * From the maximum rather than the median, because the window has to survive
 * the spikes. Checked against the measurements in app/netsetup.h, which were
 * taken on these two devices and hand-tuned into NS_INPUT_DELAY_ADHOC/WIFI:
 *
 *   ad hoc, max 21.5ms   -> 3 frames   (hand-tuned value: 3)
 *   via AP,  max 118ms   -> 9 frames   (hand-tuned value: 10)
 *   via AP,  max 300ms   -> 19 frames
 *
 * Sizing from the median would have proposed 2 and 4 for those same links, and
 * the 3-over-AP case is already recorded there as 33-47fps with 63-74% of
 * frames stalled. Returns 0 when nothing has been measured yet. */
static int rtt_proposed_delay(void) {
	uint32_t max_us = 0, median_us = 0;
	unsigned samples = 0;
	if (!NetLink_rttStats(&median_us, &max_us, &samples)) return 0;
	long frames = ((long)max_us + 16700 - 1) / 16700;
	long delay = frames + 1;
	if (delay < 2) delay = 2;
	if (delay > 20) delay = 20;
	shim_log("link rtt over %u samples: median %u.%ums, max %u.%ums -> delay %ld\n",
	         samples, median_us / 1000, (median_us % 1000) / 100,
	         max_us / 1000, (max_us % 1000) / 100, delay);
	return (int)delay;
}

/* How long to let probes accumulate before proposing. At one probe per 100ms
 * this is around ten samples, which is enough for a maximum to have seen a
 * spike, and short enough to disappear behind the bootstrap overlay. */
#define RTT_SETTLE_MS 1000
/* ...or until this many have arrived, whichever is first. A maximum needs more
 * than one sample to mean anything; three keeps startup well under a second. */
#define RTT_MIN_SAMPLES 3

static bool rtt_settled(const struct timeval* since) {
	unsigned samples = 0;
	NetLink_rttStats(NULL, NULL, &samples);
	return samples >= RTT_MIN_SAMPLES || elapsed_ms(since) >= RTT_SETTLE_MS;
}

/* Both sides adopt the higher proposal. Order-independent and needs no extra
 * round trip, which is what preserves the exact-agreement invariant the
 * timeline priming depends on: whatever order the two identities cross in, both
 * arrive at the same number. */
static void adopt_peer_delay(uint32_t peer_delay, const char* who) {
	if (peer_delay < 1 || peer_delay > 20) return;
	if ((uint32_t)input_delay >= peer_delay) return;
	shim_log("%s proposed input delay %u; adopting it over our %d\n",
	         who, peer_delay, input_delay);
	input_delay = (int)peer_delay;
}

/* The app's transport-specific value is a measured safety floor: ordinary
 * Wi-Fi needs substantially more jitter tolerance than ad hoc. A few quiet
 * startup probes can justify raising that floor, but cannot prove that the
 * later link will never spike above it. */
static void adopt_measured_delay(int proposed) {
	if (proposed > input_delay) {
		shim_log("measured link raises input delay to %d (was %d)\n",
		         proposed, input_delay);
		input_delay = proposed;
	} else if (proposed > 0 && proposed < input_delay) {
		shim_log("measured link suggests input delay %d; keeping transport floor %d\n",
		         proposed, input_delay);
	}
}

static const char* find_option_override(const char* key) {
	for (int i = 0; i < option_override_count; i++) {
		if (!strcmp(option_override[i].key, key)) return option_override[i].value;
	}
	return NULL;
}

//////////////////////////////////////////////////////////////////////////////
// loading
//////////////////////////////////////////////////////////////////////////////

static const char* resolve_core(Core* target) {
	const char* missing = NULL;
#define RESOLVE_TO(field, name)                                        \
	do {                                                                \
		target->field = dlsym(target->handle, name);                     \
		if (!target->field) missing = missing ? missing : name;         \
	} while (0)
	RESOLVE_TO(init,                       "retro_init");
	RESOLVE_TO(deinit,                     "retro_deinit");
	RESOLVE_TO(api_version,                "retro_api_version");
	RESOLVE_TO(get_system_info,            "retro_get_system_info");
	RESOLVE_TO(get_system_av_info,         "retro_get_system_av_info");
	RESOLVE_TO(set_environment,            "retro_set_environment");
	RESOLVE_TO(set_video_refresh,          "retro_set_video_refresh");
	RESOLVE_TO(set_audio_sample,           "retro_set_audio_sample");
	RESOLVE_TO(set_audio_sample_batch,     "retro_set_audio_sample_batch");
	RESOLVE_TO(set_input_poll,             "retro_set_input_poll");
	RESOLVE_TO(set_input_state,            "retro_set_input_state");
	RESOLVE_TO(set_controller_port_device, "retro_set_controller_port_device");
	RESOLVE_TO(reset,                      "retro_reset");
	RESOLVE_TO(run,                        "retro_run");
	RESOLVE_TO(serialize_size,             "retro_serialize_size");
	RESOLVE_TO(serialize,                  "retro_serialize");
	RESOLVE_TO(unserialize,                "retro_unserialize");
	RESOLVE_TO(cheat_reset,                "retro_cheat_reset");
	RESOLVE_TO(cheat_set,                  "retro_cheat_set");
	RESOLVE_TO(load_game,                  "retro_load_game");
	RESOLVE_TO(unload_game,                "retro_unload_game");
	RESOLVE_TO(get_region,                 "retro_get_region");
	RESOLVE_TO(get_memory_data,            "retro_get_memory_data");
	RESOLVE_TO(get_memory_size,            "retro_get_memory_size");
	target->load_game_special = dlsym(target->handle, "retro_load_game_special");
	target->dual_get_abi_version = dlsym(target->handle, "retro_dual_get_abi_version");
	target->dual_get_capabilities = dlsym(target->handle, "retro_dual_get_capabilities");
	target->dual_set_visible_console = dlsym(target->handle, "retro_dual_set_visible_console");
	target->dual_get_visible_console = dlsym(target->handle, "retro_dual_get_visible_console");
	target->dual_get_memory_data = dlsym(target->handle, "retro_dual_get_memory_data");
	target->dual_get_memory_size = dlsym(target->handle, "retro_dual_get_memory_size");
	target->dual_reset_console = dlsym(target->handle, "retro_dual_reset_console");
	target->dual_is_checkpoint_safe = dlsym(target->handle, "retro_dual_is_checkpoint_safe");
	target->dual_serialize_size = dlsym(target->handle, "retro_dual_serialize_size");
	target->dual_serialize = dlsym(target->handle, "retro_dual_serialize");
	target->dual_unserialize = dlsym(target->handle, "retro_dual_unserialize");
	target->dual_set_clock_epochs = dlsym(target->handle, "retro_dual_set_clock_epochs");
#undef RESOLVE_TO
	return missing;
}

// Resolved lazily: minarch calls retro_get_system_info before it calls any of
// the retro_set_* registration functions, so there is no single safe place to
// do this up front. Every exported entry point calls this first.
//
// Failure is fatal by design. There is no useful degraded mode - and returning
// a zeroed retro_system_info would segfault minarch, which strcpys
// valid_extensions unchecked. Stock minarch exits when a core won't load; we
// match that so the log says what happened.
/* Frozen executable-sharing implementation. The setting remains visible, but
 * unauthenticated peer binaries must not reach dlopen. Compatibility selection
 * is now performed by the setup app using local, packaged core manifests. */
#if 0
/* CRC32 of a file, plus its ELF machine type. Enough to say whether two builds
 * are the same and whether the other one could even load here.
 *
 * Deliberately a file hash rather than library_name/library_version: the core
 * has to be identified *before* dlopen, because the whole point is to decide
 * which file to open. */
static bool fingerprint_core(const char* path, NetLinkCoreId* out) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;

	static uint32_t table[256];
	static int table_built = 0;
	if (!table_built) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		table_built = 1;
	}

	uint32_t crc = 0xFFFFFFFFu;
	size_t total = 0;
	unsigned char buf[16384];
	size_t n;
	uint16_t machine = 0;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		if (total == 0 && n >= 20 && !memcmp(buf, "\177ELF", 4))
			machine = (uint16_t)(buf[18] | (buf[19] << 8));
		for (size_t i = 0; i < n; i++) crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
		total += n;
	}
	fclose(f);

	out->crc = crc ^ 0xFFFFFFFFu;
	out->size = (uint32_t)total;
	out->machine = machine;
	out->can_send = 0;
	return total > 0;
}

/* Highest GLIBC_x.y a file requires, as major*1000+minor.
 *
 * Scans for the literal strings rather than walking the version-needs section:
 * they live in .dynstr either way, and a byte scan cannot be fooled by a
 * malformed section header. Verified against readelf on real builds - tg5050
 * cores come back 2.33, every other platform 2.17. */
static uint32_t glibc_floor(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return 0;

	uint32_t best = 0;
	char win[512];
	size_t fill = 0;
	size_t n;
	/* Overlapping window, so a match spanning two reads is not missed. */
	while ((n = fread(win + fill, 1, sizeof(win) - fill - 1, f)) > 0) {
		size_t have = fill + n;
		win[have] = '\0';
		for (size_t i = 0; i + 10 < have; i++) {
			if (memcmp(win + i, "GLIBC_", 6)) continue;
			unsigned maj = 0, min = 0;
			if (sscanf(win + i + 6, "%u.%u", &maj, &min) == 2) {
				uint32_t v = maj * 1000 + min;
				if (v > best) best = v;
			}
		}
		if (have > 32) {
			memmove(win, win + have - 32, 32);
			fill = 32;
		} else fill = have;
	}
	fclose(f);
	return best;
}

/* What this device provides. gnu_get_libc_version is glibc-specific, which is
 * fine - these platforms are all glibc. */
static uint32_t runtime_glibc(void) {
	const char* v = gnu_get_libc_version();
	unsigned maj = 0, min = 0;
	if (v && sscanf(v, "%u.%u", &maj, &min) == 2) return maj * 1000 + min;
	return 0;
}

/* library_version without loading a ROM: retro_get_system_info is callable
 * before retro_init, so a core can be identified for a few milliseconds of
 * dlopen rather than a full launch. */
static void probe_core_version(const char* path, char* out, int len) {
	out[0] = '\0';
	void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!h) return;
	void (*gsi)(struct retro_system_info*) = dlsym(h, "retro_get_system_info");
	if (gsi) {
		struct retro_system_info i;
		memset(&i, 0, sizeof(i));
		gsi(&i);
		if (i.library_version) snprintf(out, len, "%s", i.library_version);
	}
	dlclose(h);
}

/* Does the session permit handing our core to a peer? Written by the app from
 * its settings; absent means the default, which is on. */
static int session_share_cores(const char* path) {
	FILE* f = fopen(path, "r");
	if (!f) return 1;
	char line[256];
	int v = 1;
	while (fgets(line, sizeof(line), f)) {
		int n;
		if (sscanf(line, " share_cores = %d", &n) == 1) v = n;
	}
	fclose(f);
	return v;
}

/* Production waits long enough for a peer coming up over WiFi. Host tests that
 * deliberately run one side can shorten this without baking test timing into
 * the protocol. Values outside a useful range are ignored. */
static int negotiate_timeout_ms(void) {
	const char* value = getenv("NETPLAY_NEGOTIATE_TIMEOUT_MS");
	if (!value || !value[0]) return 15000;
	char* end = NULL;
	long ms = strtol(value, &end, 10);
	return end && *end == '\0' && ms >= 50 && ms <= 60000 ? (int)ms : 15000;
}

/* Where an adopted core is written. /tmp is RAM on these devices, which is the
 * right place: it is fast, and a borrowed core must not outlive the session or
 * quietly become what this device runs from then on. */
#define ADOPTED_CORE_PATH "/tmp/netplay_adopted_core.so"

/* Wait for the handshake, compare builds, and if they differ take the host's.
 *
 * Bounded, and every failure falls back to our own core rather than refusing to
 * start: a session that plays with a desync risk is more use than one that will
 * not launch. Returns the path to dlopen.
 */
/* A core the app already staged for this session, if any.
 *
 * This is the fast path the negotiation exists to avoid needing: when the app
 * has exchanged manifests at arm time and fetched the peer's build while the
 * user was still browsing, the launch has nothing to wait for. Named for the
 * core we were asked to wrap, so one staged directory serves every system. */
static const char* staged_core(const char* path) {
	const char* base = strrchr(path, '/');
	base = base ? base + 1 : path;

	const char* pak = getenv("NETPLAY_PAK");
	if (!pak || !pak[0]) return NULL;

	static char staged[512];
	snprintf(staged, sizeof(staged), "%s/cores/staged/%s", pak, base);

	struct stat st;
	if (stat(staged, &st) != 0 || st.st_size <= 0) return NULL;

	void* probe = dlopen(staged, RTLD_NOW | RTLD_LOCAL);
	if (!probe) {
		shim_log("staged core will not load (%s) - ignoring it\n", dlerror());
		return NULL;
	}
	dlclose(probe);
	shim_log("using the core staged for this session: %s\n", staged);
	return staged;
}

static const char* negotiate_core(const char* path, const char* session) {
	/* Checked before anything is exchanged: if the app already settled this,
	 * there is nothing to negotiate and no reason to wait for a handshake. */
	const char* pre = staged_core(path);
	if (pre) {
		NetLink_start();
		return pre;
	}

	NetLinkCoreId mine;
	if (!fingerprint_core(path, &mine)) return path;
	mine.can_send = session_share_cores(session) ? 1 : 0;
	mine.core_glibc = glibc_floor(path);
	mine.runtime_glibc = runtime_glibc();
	probe_core_version(path, mine.version, sizeof(mine.version));
	NetLink_setCoreId(&mine);

	if (!NetLink_start()) return path;

	/* The handshake runs on the netlink thread. Bounded wait - this is on the
	 * path to the first frame, so it must not be able to hang a launch. */
	NetLinkCoreId theirs;
	int waited = 0;
	int negotiate_ms = negotiate_timeout_ms();
	while (!NetLink_peerCoreId(&theirs) && waited < negotiate_ms) {
		usleep(100 * 1000);
		waited += 100;
	}
	if (!NetLink_peerCoreId(&theirs)) {
		shim_log("peer core identity never arrived - using ours\n");
		return path;
	}

	if (theirs.crc == mine.crc && theirs.size == mine.size) {
		shim_log("core builds match (crc=%08x)\n", mine.crc);
		return path;
	}

	/* Same source revision is enough. Different platforms build the same commit
	 * into different files, and those emulate identically - transferring
	 * between them would be megabytes spent to arrive back where we started. */
	if (mine.version[0] && !strcmp(mine.version, theirs.version)) {
		shim_log("different build, same revision '%s' - no transfer needed\n", mine.version);
		return path;
	}

	shim_log("core mismatch: ours crc=%08x '%s', theirs crc=%08x '%s'\n",
	         mine.crc, mine.version, theirs.crc, theirs.version);

	/* Architecture is the one difference no transfer can bridge. */
	if (theirs.machine != mine.machine) {
		shim_log("peer is a different architecture (%u vs %u) - cannot share cores; "
		         "shared screen will desync unless the builds happen to agree\n",
		         theirs.machine, mine.machine);
		return path;
	}

	/* Who donates is a loadability question before it is an authority question.
	 * A tg5050-built core needs glibc 2.33 where every other platform provides
	 * 2.17, so host-wins would hand over a file the peer cannot open. */
	bool we_could_load_theirs   = theirs.core_glibc <= mine.runtime_glibc;
	bool they_could_load_ours   = mine.core_glibc <= theirs.runtime_glibc;

	if (!we_could_load_theirs && !they_could_load_ours) {
		shim_log("neither core is loadable on the other device "
		         "(theirs needs %u.%u, we have %u.%u; ours needs %u.%u, they have %u.%u)\n",
		         theirs.core_glibc / 1000, theirs.core_glibc % 1000,
		         mine.runtime_glibc / 1000, mine.runtime_glibc % 1000,
		         mine.core_glibc / 1000, mine.core_glibc % 1000,
		         theirs.runtime_glibc / 1000, theirs.runtime_glibc % 1000);
		return path;
	}

	/* Default is host-donates, overridden only when loadability says otherwise. */
	bool we_donate = (NetLink_getRole() == NETLINK_ROLE_HOST);
	if (!we_could_load_theirs && they_could_load_ours) {
		if (!we_donate) shim_log("we cannot load the host's core - offering ours instead\n");
		we_donate = true;
	} else if (!they_could_load_ours && we_could_load_theirs) {
		if (we_donate) shim_log("peer cannot load our core - taking theirs instead\n");
		we_donate = false;
	}

	if (we_donate) {
		if (!mine.can_send) {
			shim_log("core sharing disabled here - peer keeps its own build\n");
			return path;
		}
		FILE* f = fopen(path, "rb");
		if (!f) return path;
		void* buf = malloc(mine.size);
		if (!buf) { fclose(f); return path; }
		size_t got = fread(buf, 1, mine.size, f);
		fclose(f);
		if (got == mine.size && NetLink_sendCore(buf, got))
			shim_log("sent our core to the peer (%zu bytes)\n", got);
		else
			shim_log("could not send our core\n");
		free(buf);
		return path;
	}

	if (!theirs.can_send) {
		shim_log("peer is not sharing its core - keeping ours\n");
		return path;
	}

	void* data = NULL;
	size_t len = 0;
	waited = 0;
	while (!NetLink_takeCore(&data, &len) && waited < 60000) {
		usleep(100 * 1000);
		waited += 100;
	}
	if (!data) {
		shim_log("peer's core never arrived - using ours\n");
		return path;
	}

	FILE* out = fopen(ADOPTED_CORE_PATH, "wb");
	if (!out) { free(data); return path; }
	size_t wrote = fwrite(data, 1, len, out);
	fclose(out);
	free(data);
	if (wrote != len) {
		shim_log("could not write the adopted core (%zu of %zu) - using ours\n", wrote, len);
		remove(ADOPTED_CORE_PATH);
		return path;
	}

	/* Prove it loads before committing. A core that fails dlopen here would
	 * otherwise take the whole launch down, and our own core still works. */
	void* probe = dlopen(ADOPTED_CORE_PATH, RTLD_NOW | RTLD_LOCAL);
	if (!probe) {
		shim_log("adopted core will not load (%s) - using ours\n", dlerror());
		remove(ADOPTED_CORE_PATH);
		return path;
	}
	dlclose(probe);

	shim_log("adopted the peer's core (%zu bytes)\n", len);
	return ADOPTED_CORE_PATH;
}
#endif

static void ensure_loaded(void) {
	if (core.handle) return;

	const char* path = getenv(SHIM_ENV_REAL_CORE);
	if (!path || !path[0]) {
		shim_log("FATAL: " SHIM_ENV_REAL_CORE " is unset - nothing to wrap\n");
		exit(EXIT_FAILURE);
	}

	const char* session = getenv(SHIM_ENV_SESSION);
	session_active = (session && session[0] && NetLink_configure(session));
	if (session_active) NetLink_start();
	/* Four different reasons the running core is not the installed one, and the
	 * player is owed the difference. "Compatibility" means the two devices'
	 * builds disagreed and both fell back to a packaged one; the link cores are
	 * substituted because NextUI builds them without the networking this pak
	 * needs at all, which is an implementation detail rather than a fallback;
	 * and the paired core is a different implementation again. */
	const char* notice = getenv(SHIM_ENV_NOTICE);
	if (session_active && notice) {
		if (!strcmp(notice, "compatibility"))
			snprintf(startup_notice, sizeof(startup_notice),
			         "Starting with compatibility core...");
		else if (!strcmp(notice, "netlink"))
			snprintf(startup_notice, sizeof(startup_notice),
			         "Starting with net-enabled core...");
		else if (!strcmp(notice, "paired"))
			snprintf(startup_notice, sizeof(startup_notice),
			         "Starting with dual-instance core...");
		else if (!strcmp(notice, "serial-fallback"))
			snprintf(startup_notice, sizeof(startup_notice),
			         "Cartridges not paired locally. Using link cable...");
		else if (!strcmp(notice, "mismatch"))
			snprintf(startup_notice, sizeof(startup_notice),
			         "Core builds differ. Desyncs may occur...");
		if (startup_notice[0]) shim_log("startup notice: %s\n", startup_notice);
	}

	// RTLD_LOCAL keeps the real core's retro_* symbols out of the global
	// namespace, where they would collide with the ones we export.
	core.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!core.handle) {
		shim_log("FATAL: dlopen(%s) failed: %s\n", path, dlerror());
		exit(EXIT_FAILURE);
	}

	const char* missing = resolve_core(&core);

	if (missing) {
		shim_log("FATAL: %s is missing %s\n", path, missing);
		exit(EXIT_FAILURE);
	}

	if (session_active) {
		load_option_overrides(session);
		load_session_id(session);

		// get_system_info is resolved above, so the core can be asked what it is
		// before anything else happens. Both sides run the same ROM, so they
		// reach the same answer without having to negotiate it.
		int override = session_mode_override(session);
		netplay_mode = (override >= 0) ? override : !core_wants_link();

		struct retro_system_info info;
		memset(&info, 0, sizeof(info));
		core.get_system_info(&info);
		shim_log("mode=%s for %s (%s)\n",
		         netplay_mode ? "shared-screen" : "link-cable",
		         info.library_name ? info.library_name : "unknown core",
		         override >= 0 ? "from session" : "from core");

		input_delay = session_input_delay(session);
		/* NETPLAY_DUAL_DISABLE is set by the launcher when a previous process of
		 * this game demoted itself: the paired core is not even staged, so this
		 * is belt and braces against relaunching into the same negotiation. */
		const char* dual_disabled = getenv(SHIM_ENV_NO_DUAL);
		bool dual_core_enabled =
			(core_is_gambatte() && session_instanced_core(session, "gambatte")) ||
			(core_is_mgba() && session_instanced_core(session, "mgba"));
		bool dual_requested = !netplay_mode && dual_core_enabled &&
		                      !(dual_disabled && dual_disabled[0] == '1');
		dual_link_mode = dual_requested && core_has_dual_contract();
		if (dual_link_mode) {
			dual_local_console = NetLink_getRole() == NETLINK_ROLE_HOST
			                   ? GAMBATTE_DUAL_CONSOLE_A : GAMBATTE_DUAL_CONSOLE_B;
			dual_peer_console = dual_local_console == GAMBATTE_DUAL_CONSOLE_A
			                  ? GAMBATTE_DUAL_CONSOLE_B : GAMBATTE_DUAL_CONSOLE_A;
			if (!core.dual_set_visible_console(dual_local_console)) {
				dual_link_mode = 0;
				shim_log("dual contract rejected visible-console selection; using network serial\n");
			} else {
				shim_log("paired-core dual ABI v%u enabled (console %c visible); Wi-Fi carries inputs only\n",
				         core.dual_get_abi_version(), dual_local_console ? 'B' : 'A');
			}
		} else if (dual_requested) {
			shim_log("paired core/ABI unavailable; using network serial fallback\n");
		}

		// After the mode is known: only shared screen compares state, and only
		// then does a nondeterministic core matter.
		if (netplay_mode) apply_required_options();
		if (netplay_mode && NetLink_getRole() == NETLINK_ROLE_CLIENT) {
			snprintf(guest_save_dir, sizeof(guest_save_dir),
			         "/tmp/netplay-guest-%s%ld",
			         session_id[0] ? session_id : "process-", (long)getpid());
			if (mkdir(guest_save_dir, 0700) != 0 && errno != EEXIST) {
				shim_log("WARNING: cannot create volatile guest save directory %s: %s\n",
				         guest_save_dir, strerror(errno));
				guest_save_dir[0] = '\0';
			} else {
				shim_log("guest core-managed saves redirected to %s\n", guest_save_dir);
			}
		}
	}

	shim_log("wrapping %s (session=%s)\n", path, session_active ? "armed" : "none");

	// The transport starts before the core is opened so link-capable cores see a
	// ready netpacket interface during their own initialization.
}

//////////////////////////////////////////////////////////////////////////////
// callback wrappers
//
// Pure forwards for now. These exist so the interception path is exercised
// from the first build: if a game plays identically through these, the seams
// are sound and netplay logic can be added behind them.
//////////////////////////////////////////////////////////////////////////////

// Bridges the core's send to the wire. Handed to the core in start().
static void shim_netpacket_send(int flags, const void* buf, size_t len, uint16_t client_id) {
	NetLink_send(flags, buf, len, client_id);
}

/* Packets are read off the socket by the netlink worker the moment they land,
 * but they only reach the core through here. Delivering them once per frame is
 * the wrong clock for a serial exchange: a byte that arrived 1ms into a frame
 * waits out the remaining 15ms before the core can see it, and a Gen 1 party
 * trade is hundreds of sequential transactions each paying that toll.
 *
 * The budget is per frame rather than per call, and shared with the frame-top
 * drain, so a core that polls in a tight loop cannot spend the frame delivering
 * packets and never return to the frontend. */
static int netpacket_budget;
static void link_note_delivery(void);

static void deliver_packets(void) {
	uint8_t buf[NETLINK_MAX_PACKET];
	size_t len;
	while (netpacket_budget > 0 && NetLink_popPacket(buf, sizeof(buf), &len)) {
		netpacket_budget--;
		link_note_delivery();
		core_netpacket.receive(buf, len, NetLink_remoteClientId());
	}
}

/* The core asking to read mid-frame, which is the entire point of the libretro
 * netpacket poll_receive callback.
 *
 * Re-entrancy: a core is permitted to call this from inside its own receive
 * handler, and delivering from there would recurse through the same handler
 * with the next packet. One depth counter makes the nested call a no-op, which
 * is correct rather than merely safe - the outer loop delivers that packet as
 * soon as the handler returns. */
static void shim_netpacket_poll_receive(void) {
	static int in_delivery;
	if (!netpacket_started || in_delivery) return;
	in_delivery = 1;
	deliver_packets();
	in_delivery = 0;
}

static const char* dual_core_option(const char* key);

static bool shim_environment(unsigned cmd, void* data) {
	if (cmd == RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE) {
		// Only claim to support this when we can actually back it. With no
		// session armed we forward to the frontend, which says no - so a
		// disarmed launch leaves the core in exactly the state it would be in
		// without the shim, rather than waiting on a session that never starts.
		if (!session_active) {
			return fe_environment ? fe_environment(cmd, data) : false;
		}

		// Answer this ourselves instead of forwarding. A stock minarch returns
		// false here, which makes the core disable link support entirely.
		if (!data) {
			memset(&core_netpacket, 0, sizeof(core_netpacket));
			have_netpacket = 0;
			shim_log("core withdrew its netpacket interface\n");
			return true;
		}

		const struct retro_netpacket_callback* cb = data;
		if (!cb->start || !cb->receive) {
			shim_log("core offered an unusable netpacket interface\n");
			return false;
		}

		core_netpacket = *cb;
		have_netpacket = 1;
		shim_log("core registered netpacket interface (protocol=%s)\n",
		         cb->protocol_version ? cb->protocol_version : "core version");
		return true;
	}

	// Answer forced options ourselves. This replaces the patched minarch's
	// minarch_setCoreOptionValue / forceCoreOptionUpdate: the core simply never
	// sees the frontend's value for a key we are pinning.
	// Track the format so the paused frame can be dimmed correctly.
	if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT && data) {
		pixel_format = *(const enum retro_pixel_format*)data;
	}

	if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE && session_active && data) {
		struct retro_variable* var = data;
		if (dual_link_mode && var->key) {
			const char* pinned = dual_core_option(var->key);
			if (pinned) { var->value = pinned; return true; }
		}
		const char* forced = var->key ? find_option_override(var->key) : NULL;
		if (forced) {
			var->value = forced;
			return true;
		}
	}

	if (cmd == RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY && guest_save_dir[0] && data) {
		*(const char**)data = guest_save_dir;
		return true;
	}

	if (cmd == RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE && data && core_is_mgba()) {
		unsigned rate = mgba_target_sample_rate();
		if (rate) {
			*(unsigned*)data = rate;
			shim_log("answering mGBA target sample rate: %u\n", rate);
			return true;
		}
	}

	if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE && session_active && data) {
		bool fe_says = fe_environment ? fe_environment(cmd, data) : false;
		if (option_update_pending) {
			option_update_pending = 0;
			*(bool*)data = true;
			return true;
		}
		return fe_says;
	}

	return fe_environment ? fe_environment(cmd, data) : false;
}

/* The paired core owns both consoles and the cable between them, so Gambatte's
 * ordinary network Game Link must stay switched off inside it.
 *
 * This is not cosmetic. check_variables() still runs the stock handler in a
 * NETPLAY_DUAL_INSTANCE build, and it acts on whatever this returns before the
 * wrapper reattaches its private bus: "Network Server" would bind a real
 * GameLink listener on the session's own Wi-Fi, and the "Local Server"/"Local
 * Client" values the two-copy implementation used would open a loopback
 * listener on every device plus a connect that can only ever be refused. The
 * session file always carries gambatte_gb_link_* for the serial path, so an
 * override here is required, not merely tidy. "Not Connected" takes the
 * handler's default branch: gb_net_serial.stop() and setSerialIO(NULL), which
 * the wrapper then replaces with the in-process coordinator. */
static const char* dual_core_option(const char* key) {
	if (!strcmp(key, "gambatte_gb_link_mode")) return "Not Connected";
	return NULL;
}

#if 0
/* ---------------------------------------------------------------------------
 * Disabled: the two-copy ("shadow core") instanced link.
 *
 * The first instanced implementation copied gambatte_libretro.so to a second
 * path so dlopen would give it its own file-scope globals, drove it from a
 * persistent worker thread, and linked the two copies with Gambatte's own
 * GameLink over loopback TCP. It established and sustained sessions correctly,
 * but the clock-owning console's synchronous SerialIO::send() held an A30/Brick
 * pair to 22-23fps whichever device owned the clock (docs/multi-instance.md).
 *
 * The paired core replaced it: gambatte_dual_libretro.so holds both consoles
 * and an in-memory serial coordinator, and one retro_run advances the pair.
 * These functions are kept, disabled, because the shape is what a future core
 * that cannot host two consoles itself would need again, and because the
 * measurements above are only meaningful next to the code that produced them.
 *
 * Reading this later: it was gated on `dual_link_mode && !dual_core_mode`.
 * Those two flags were always equal by the time this was retired, which is what
 * made the whole path unreachable; `dual_core_mode` is now gone and
 * `dual_link_mode` alone means "paired core in use".
 * ------------------------------------------------------------------------- */

static Core shadow_core;
static char shadow_core_path[256];
static char shadow_save_dir[256];
/* The hidden core has to run concurrently with the visible core because either
 * side of Gambatte's local serial link may block waiting for the other. Keep
 * one worker alive for the game: creating and joining a pthread for every
 * emulated frame was measurable as a larger cost than Gambatte itself on the
 * A30. */
static pthread_t shadow_worker_thread;
static pthread_mutex_t shadow_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t shadow_worker_request = PTHREAD_COND_INITIALIZER;
static pthread_cond_t shadow_worker_complete = PTHREAD_COND_INITIALIZER;
static int shadow_worker_started;
static int shadow_worker_pending;
static int shadow_worker_done;
static int shadow_worker_stop;
static uint32_t shadow_last_run_us;
static uint64_t dual_shadow_run_us;
static uint32_t dual_shadow_run_max_us;
static uint64_t dual_visible_run_us;
static uint32_t dual_visible_run_max_us;

static const char* dual_link_option(const char* key, bool shadow) {
	static const char* digits = "127000000001";
	static const char digit_values[10][2] = {
		"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"
	};
	bool primary_server = NetLink_getRole() == NETLINK_ROLE_HOST;
	bool server = shadow ? !primary_server : primary_server;
	if (!strcmp(key, "gambatte_gb_link_mode"))
		return server ? "Local Server" : "Local Client";
	if (!strcmp(key, "gambatte_gb_link_network_port")) return "56401";
	const char* prefix = "gambatte_gb_link_network_server_ip_";
	if (!strncmp(key, prefix, strlen(prefix))) {
		int n = atoi(key + strlen(prefix));
		if (n >= 1 && n <= 12) return digit_values[digits[n - 1] - '0'];
	}
	return NULL;
}

static bool shadow_environment(unsigned cmd, void* data) {
	/* MinArch's environment callback is not documented as thread-safe. The
	 * hidden core runs concurrently only after startup, so do not let its
	 * per-frame option poll enter the frontend beside the visible core. Session
	 * options are immutable for this process. */
	if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE && data) {
		*(bool*)data = false;
		return true;
	}
	if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE && data) {
		struct retro_variable* var = data;
		const char* local = var->key ? dual_link_option(var->key, true) : NULL;
		if (local) { var->value = local; return true; }
		const char* forced = var->key ? find_option_override(var->key) : NULL;
		if (forced) { var->value = forced; return true; }
	}
	/* The hidden console must never open core-managed files in the visible
	 * console's save directory. Its serialized state is supplied by the peer. */
	if ((cmd == RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY ||
	     cmd == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY) && data && shadow_save_dir[0]) {
		*(const char**)data = shadow_save_dir;
		return true;
	}
	if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) return true;
	return fe_environment ? fe_environment(cmd, data) : false;
}

static void shadow_video_refresh(const void* data, unsigned w, unsigned h, size_t pitch) {
	(void)data; (void)w; (void)h; (void)pitch;
}
static void shadow_audio_sample(int16_t left, int16_t right) { (void)left; (void)right; }
static size_t shadow_audio_batch(const int16_t* data, size_t frames) { (void)data; return frames; }
static void shadow_input_poll(void) {}
static int16_t shadow_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
	if (port || device != RETRO_DEVICE_JOYPAD || index) return 0;
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)dual_shadow_buttons;
	return (dual_shadow_buttons >> id) & 1;
}

static bool copy_core_file(const char* from, const char* to) {
	FILE* in = fopen(from, "rb");
	if (!in) return false;
	FILE* out = fopen(to, "wb");
	if (!out) { fclose(in); return false; }
	char buf[32768];
	size_t n;
	bool ok = true;
	while ((n = fread(buf, 1, sizeof(buf), in)) != 0)
		if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
	if (ferror(in)) ok = false;
	if (fclose(out) != 0) ok = false;
	fclose(in);
	if (!ok) remove(to);
	return ok;
}

static bool prepare_shadow_core(void) {
	if (!dual_link_mode || shadow_core.handle) return shadow_core.handle != NULL;
	const char* source = getenv(SHIM_ENV_REAL_CORE);
	if (!source || !source[0]) return false;
	snprintf(shadow_core_path, sizeof(shadow_core_path),
	         "/tmp/netplay-gambatte-shadow-%ld.so", (long)getpid());
	if (!copy_core_file(source, shadow_core_path)) {
		snprintf(dual_error, sizeof(dual_error), "Could not stage second Gambatte instance.");
		return false;
	}
	shadow_core.handle = dlopen(shadow_core_path, RTLD_NOW | RTLD_LOCAL);
	if (!shadow_core.handle) {
		shim_log("second Gambatte dlopen failed: %s\n", dlerror());
		snprintf(dual_error, sizeof(dual_error), "Could not load second Gambatte instance.");
		remove(shadow_core_path);
		return false;
	}
	const char* missing = resolve_core(&shadow_core);
	if (missing) {
		shim_log("second Gambatte is missing %s\n", missing);
		snprintf(dual_error, sizeof(dual_error), "Second Gambatte build is incomplete.");
		dlclose(shadow_core.handle);
		memset(&shadow_core, 0, sizeof(shadow_core));
		remove(shadow_core_path);
		return false;
	}
	shadow_core.set_environment(shadow_environment);
	shadow_core.set_video_refresh(shadow_video_refresh);
	shadow_core.set_audio_sample(shadow_audio_sample);
	shadow_core.set_audio_sample_batch(shadow_audio_batch);
	shadow_core.set_input_poll(shadow_input_poll);
	shadow_core.set_input_state(shadow_input_state);
	shadow_core.init();
	shim_log("second Gambatte instance loaded from %s\n", shadow_core_path);
	return true;
}

static void* shadow_worker_main(void* unused) {
	(void)unused;
	pthread_mutex_lock(&shadow_worker_mutex);
	for (;;) {
		while (!shadow_worker_pending && !shadow_worker_stop)
			pthread_cond_wait(&shadow_worker_request, &shadow_worker_mutex);
		if (shadow_worker_stop) break;
		shadow_worker_pending = 0;
		pthread_mutex_unlock(&shadow_worker_mutex);

		struct timeval started, finished;
		gettimeofday(&started, NULL);
		shadow_core.run();
		gettimeofday(&finished, NULL);
		uint32_t elapsed = timeval_delta_us(&started, &finished);

		pthread_mutex_lock(&shadow_worker_mutex);
		shadow_last_run_us = elapsed;
		shadow_worker_done = 1;
		pthread_cond_signal(&shadow_worker_complete);
	}
	pthread_mutex_unlock(&shadow_worker_mutex);
	return NULL;
}

static bool start_shadow_worker(void) {
	if (shadow_worker_started) return true;
	pthread_mutex_lock(&shadow_worker_mutex);
	shadow_worker_pending = shadow_worker_done = shadow_worker_stop = 0;
	int rc = pthread_create(&shadow_worker_thread, NULL, shadow_worker_main, NULL);
	if (!rc) shadow_worker_started = 1;
	pthread_mutex_unlock(&shadow_worker_mutex);
	if (rc) dual_fail("Could not start the hidden Gambatte worker.");
	else shim_log("persistent hidden-core worker started\n");
	return rc == 0;
}

static void stop_shadow_worker(void) {
	if (!shadow_worker_started) return;
	pthread_mutex_lock(&shadow_worker_mutex);
	shadow_worker_stop = 1;
	pthread_cond_signal(&shadow_worker_request);
	pthread_mutex_unlock(&shadow_worker_mutex);
	pthread_join(shadow_worker_thread, NULL);
	pthread_mutex_lock(&shadow_worker_mutex);
	shadow_worker_started = 0;
	shadow_worker_pending = shadow_worker_done = shadow_worker_stop = 0;
	pthread_mutex_unlock(&shadow_worker_mutex);
}

/* Ran the visible core on this thread and the hidden core on the worker, then
 * joined. Concurrency was mandatory: whichever console owned the link clock
 * blocked inside GB::runFor() until its peer answered. */
static bool run_shadow_pair(void) {
	if (!start_shadow_worker()) return false;
	struct timeval pair_started, visible_started, visible_finished, pair_finished;
	gettimeofday(&pair_started, NULL);

	pthread_mutex_lock(&shadow_worker_mutex);
	shadow_worker_done = 0;
	shadow_worker_pending = 1;
	pthread_cond_signal(&shadow_worker_request);
	pthread_mutex_unlock(&shadow_worker_mutex);

	NetLink_setCoreRunning(true);
	gettimeofday(&visible_started, NULL);
	core.run();
	gettimeofday(&visible_finished, NULL);

	pthread_mutex_lock(&shadow_worker_mutex);
	while (!shadow_worker_done)
		pthread_cond_wait(&shadow_worker_complete, &shadow_worker_mutex);
	uint32_t shadow_us = shadow_last_run_us;
	pthread_mutex_unlock(&shadow_worker_mutex);
	gettimeofday(&pair_finished, NULL);
	NetLink_setCoreRunning(false);

	uint32_t visible_us = timeval_delta_us(&visible_started, &visible_finished);
	uint32_t pair_us = timeval_delta_us(&pair_started, &pair_finished);
	dual_visible_run_us += visible_us;
	dual_shadow_run_us += shadow_us;
	dual_pair_run_us += pair_us;
	if (visible_us > dual_visible_run_max_us) dual_visible_run_max_us = visible_us;
	if (shadow_us > dual_shadow_run_max_us) dual_shadow_run_max_us = shadow_us;
	if (pair_us > dual_pair_run_max_us) dual_pair_run_max_us = pair_us;
	return true;
}
#endif /* two-copy instanced link */

// Bring both sides to a bit-identical starting point. The host ships its state;
// the client adopts it. Without this the two simulations differ from frame one
// and every synced input afterwards is meaningless.
// Identify the emulator, not the game. Two devices can run the same ROM under
// genuinely different cores.
static uint32_t core_identity(void) {
	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	core.get_system_info(&info);

	uint32_t h = 2166136261u;
	const char* parts[2] = { info.library_name, info.library_version };
	for (int i = 0; i < 2; i++)
		for (const char* p = parts[i]; p && *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }

	/* Serialize size differs between builds far more often than the version
	 * string does, so fold it in.
	 *
	 * Not for instanced link. There the state covers *both* consoles, so the
	 * size depends on which two cartridges are loaded - and identity is
	 * exchanged before the pair is rebuilt with one cartridge each, when the two
	 * devices still hold their own game twice. Two handhelds carrying Red and
	 * Blue would read as different builds. The paired ABI version answers the
	 * same question without depending on content. */
	size_t sz = dual_link_mode ? (size_t)core.dual_get_abi_version()
	                           : core.serialize_size();
	h ^= (uint32_t)sz;
	h *= 16777619u;

	/* Log the inputs, not just the hash. A single number tells you two builds
	 * disagree but not which field disagrees - and "same version string, still
	 * a mismatch" is exactly the case worth telling apart, because a differing
	 * serialize_size means the two sides cannot exchange state at all, while a
	 * differing version is only a build to line up. */
	shim_log("core identity: name='%s' version='%s' %s=%zu -> %08x\n",
	         info.library_name ? info.library_name : "?",
	         info.library_version ? info.library_version : "?",
	         dual_link_mode ? "paired_abi" : "serialize_size", sz, h);
	return h;
}

#define AUTHORITATIVE_MAGIC 0x4E505356u /* NPSV */
#define AUTHORITATIVE_VERSION 1u

typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint32_t version;
	uint32_t state_size;
	uint32_t sram_size;
	uint32_t rtc_size;
} AuthoritativeHeader;

static uint32_t hash_bytes(const void* data, size_t len);

static bool core_memory_sizes(Core* target, uint32_t* sram_size, uint32_t* rtc_size) {
	size_t sram = target->get_memory_size(RETRO_MEMORY_SAVE_RAM);
	size_t rtc = target->get_memory_size(RETRO_MEMORY_RTC);
	if (sram > UINT32_MAX || rtc > UINT32_MAX) return false;
	*sram_size = (uint32_t)sram;
	*rtc_size = (uint32_t)rtc;
	return true;
}

static bool persistent_memory_sizes(uint32_t* sram_size, uint32_t* rtc_size) {
	return core_memory_sizes(&core, sram_size, rtc_size);
}

#define DUAL_MEMORY_MAGIC 0x47424d45u /* GBME */
typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint32_t sram_size;
	uint32_t rtc_size;
} DualMemoryHeader;

static bool dual_memory_sizes(unsigned console, uint32_t* sram_size, uint32_t* rtc_size) {
	size_t sram = core.dual_get_memory_size(console, RETRO_MEMORY_SAVE_RAM);
	size_t rtc = core.dual_get_memory_size(console, RETRO_MEMORY_RTC);
	if (sram > UINT32_MAX || rtc > UINT32_MAX) return false;
	*sram_size = (uint32_t)sram;
	*rtc_size = (uint32_t)rtc;
	return true;
}

static bool capture_dual_memory(unsigned console, void** out, size_t* out_len) {
	uint32_t sram_size = 0, rtc_size = 0;
	if (!dual_memory_sizes(console, &sram_size, &rtc_size)) return false;
	size_t total = sizeof(DualMemoryHeader) + (size_t)sram_size + rtc_size;
	if (total > NETLINK_MAX_STATE) return false;
	void* sram = sram_size ? core.dual_get_memory_data(console, RETRO_MEMORY_SAVE_RAM) : NULL;
	void* rtc = rtc_size ? core.dual_get_memory_data(console, RETRO_MEMORY_RTC) : NULL;
	if ((sram_size && !sram) || (rtc_size && !rtc)) return false;
	uint8_t* data = malloc(total);
	if (!data) return false;
	DualMemoryHeader h = { htonl(DUAL_MEMORY_MAGIC), htonl(sram_size), htonl(rtc_size) };
	memcpy(data, &h, sizeof(h));
	if (sram_size) memcpy(data + sizeof(h), sram, sram_size);
	if (rtc_size) memcpy(data + sizeof(h) + sram_size, rtc, rtc_size);
	*out = data;
	*out_len = total;
	return true;
}

static bool apply_dual_memory(unsigned console, const void* input, size_t len) {
	if (!input || len < sizeof(DualMemoryHeader)) return false;
	DualMemoryHeader wire;
	memcpy(&wire, input, sizeof(wire));
	uint32_t magic = ntohl(wire.magic);
	uint32_t sram_size = ntohl(wire.sram_size);
	uint32_t rtc_size = ntohl(wire.rtc_size);
	uint32_t local_sram = 0, local_rtc = 0;
	if (magic != DUAL_MEMORY_MAGIC ||
	    len != sizeof(wire) + (size_t)sram_size + rtc_size ||
	    !dual_memory_sizes(console, &local_sram, &local_rtc) ||
	    sram_size != local_sram || rtc_size != local_rtc)
		return false;
	void* sram = sram_size ? core.dual_get_memory_data(console, RETRO_MEMORY_SAVE_RAM) : NULL;
	void* rtc = rtc_size ? core.dual_get_memory_data(console, RETRO_MEMORY_RTC) : NULL;
	if ((sram_size && !sram) || (rtc_size && !rtc)) return false;
	const uint8_t* data = input;
	if (sram_size) memcpy(sram, data + sizeof(wire), sram_size);
	if (rtc_size) memcpy(rtc, data + sizeof(wire) + sram_size, rtc_size);
	return true;
}

static bool parse_authoritative_state(const void* data, size_t len,
		AuthoritativeHeader* parsed) {
	if (!data || len < sizeof(AuthoritativeHeader)) return false;
	AuthoritativeHeader wire;
	memcpy(&wire, data, sizeof(wire));
	AuthoritativeHeader h = {
		.magic = ntohl(wire.magic),
		.version = ntohl(wire.version),
		.state_size = ntohl(wire.state_size),
		.sram_size = ntohl(wire.sram_size),
		.rtc_size = ntohl(wire.rtc_size),
	};
	uint64_t payload64 = (uint64_t)h.state_size + h.sram_size + h.rtc_size;
	if (h.magic != AUTHORITATIVE_MAGIC || h.version != AUTHORITATIVE_VERSION ||
	    payload64 > NETLINK_MAX_STATE - sizeof(AuthoritativeHeader) ||
	    len != sizeof(AuthoritativeHeader) + (size_t)payload64)
		return false;
	if (parsed) *parsed = h;
	return true;
}

/* Package the core state together with the raw persistent-memory regions. The
 * files that minarch used to populate those regions (.sav, .srm, compressed or
 * otherwise) never enter the protocol. */
static bool capture_core_state(Core* target, void** out, size_t* out_len, uint32_t* out_hash) {
	size_t state_size = target->serialize_size();
	uint32_t sram_size, rtc_size;
	if (!state_size || state_size > UINT32_MAX ||
	    !core_memory_sizes(target, &sram_size, &rtc_size)) return false;
	uint64_t payload64 = (uint64_t)state_size + sram_size + rtc_size;
	if (payload64 > NETLINK_MAX_STATE - sizeof(AuthoritativeHeader)) return false;
	size_t payload = (size_t)payload64;

	void* sram = sram_size ? target->get_memory_data(RETRO_MEMORY_SAVE_RAM) : NULL;
	void* rtc = rtc_size ? target->get_memory_data(RETRO_MEMORY_RTC) : NULL;
	if ((sram_size && !sram) || (rtc_size && !rtc)) return false;

	size_t total = sizeof(AuthoritativeHeader) + payload;
	uint8_t* buf = malloc(total);
	if (!buf) return false;
	AuthoritativeHeader wire = {
		.magic = htonl(AUTHORITATIVE_MAGIC),
		.version = htonl(AUTHORITATIVE_VERSION),
		.state_size = htonl((uint32_t)state_size),
		.sram_size = htonl(sram_size),
		.rtc_size = htonl(rtc_size),
	};
	memcpy(buf, &wire, sizeof(wire));
	uint8_t* state = buf + sizeof(wire);
	memset(state, 0, state_size);
	if (!target->serialize(state, state_size)) { free(buf); return false; }
	if (sram_size) memcpy(state + state_size, sram, sram_size);
	if (rtc_size) memcpy(state + state_size + sram_size, rtc, rtc_size);

	*out = buf;
	*out_len = total;
	if (out_hash) *out_hash = hash_bytes(buf, total);
	return true;
}

static bool capture_authoritative_state(void** out, size_t* out_len, uint32_t* out_hash) {
	return capture_core_state(&core, out, out_len, out_hash);
}

static bool apply_core_state(Core* target, const void* data, size_t len) {
	AuthoritativeHeader h;
	uint32_t local_sram = 0, local_rtc = 0;
	if (!parse_authoritative_state(data, len, &h)) {
		shim_log("state rejected: malformed header\n");
		return false;
	}
	if (h.state_size != target->serialize_size() ||
	    !core_memory_sizes(target, &local_sram, &local_rtc) ||
	    h.sram_size != local_sram || h.rtc_size != local_rtc) {
		shim_log("state rejected: sizes state %u/%zu sram %u/%u rtc %u/%u (state/local)\n",
		         h.state_size, target->serialize_size(), h.sram_size, local_sram,
		         h.rtc_size, local_rtc);
		return false;
	}

	const uint8_t* state = (const uint8_t*)data + sizeof(AuthoritativeHeader);
	if (!target->unserialize(state, h.state_size)) {
		shim_log("state rejected: core unserialize failed (%u bytes)\n", h.state_size);
		return false;
	}
	void* sram = h.sram_size ? target->get_memory_data(RETRO_MEMORY_SAVE_RAM) : NULL;
	void* rtc = h.rtc_size ? target->get_memory_data(RETRO_MEMORY_RTC) : NULL;
	if ((h.sram_size && !sram) || (h.rtc_size && !rtc)) return false;
	if (h.sram_size) memcpy(sram, state + h.state_size, h.sram_size);
	if (h.rtc_size) memcpy(rtc, state + h.state_size + h.sram_size, h.rtc_size);
	return true;
}

static bool apply_authoritative_state(const void* data, size_t len) {
	return apply_core_state(&core, data, len);
}

#define CHECKPOINT_MAGIC 0x4E50434Bu /* NPCK */
typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t core_identity;
	uint32_t state_size;
	uint32_t frame;
	uint32_t state_hash;
	uint8_t rom_sha256[32];
} CheckpointHeader;

static uint32_t hash_bytes(const void* data, size_t len) {
	uint32_t h = 2166136261u;
	const uint8_t* p = data;
	for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 16777619u; }
	return h;
}

static void set_checkpoint_candidate(void* data, size_t len, uint32_t frame, uint32_t hash) {
	free(checkpoint_candidate);
	checkpoint_candidate = data;
	checkpoint_candidate_len = len;
	checkpoint_candidate_frame = frame;
	checkpoint_candidate_hash = hash;
}

static bool promote_checkpoint(void) {
	if (!checkpoint_path[0] || !checkpoint_candidate || !checkpoint_candidate_len) return false;
	char tmp[224];
	snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", checkpoint_path, (long)getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) return false;
	CheckpointHeader h = {
		.magic = CHECKPOINT_MAGIC, .version = 2,
		.core_identity = core_identity(),
		.state_size = (uint32_t)checkpoint_candidate_len,
		.frame = checkpoint_candidate_frame,
		.state_hash = checkpoint_candidate_hash,
	};
	memcpy(h.rom_sha256, rom_sha256, sizeof(h.rom_sha256));
	const uint8_t* parts[2] = { (const uint8_t*)&h, checkpoint_candidate };
	size_t lengths[2] = { sizeof(h), checkpoint_candidate_len };
	bool ok = true;
	for (int part = 0; part < 2 && ok; part++) {
		const uint8_t* p = parts[part];
		size_t left = lengths[part];
		while (left) {
			ssize_t n = write(fd, p, left);
			if (n > 0) { p += n; left -= (size_t)n; }
			else if (n < 0 && errno == EINTR) continue;
			else { ok = false; break; }
		}
	}
	if (ok) ok = fsync(fd) == 0;
	if (close(fd) != 0) ok = false;
	if (ok) ok = rename(tmp, checkpoint_path) == 0;
	if (!ok) unlink(tmp);
	if (ok) shim_log("confirmed host checkpoint promoted at frame %u\n", h.frame);
	else shim_log("could not persist confirmed host checkpoint: %s\n", strerror(errno));
	return ok;
}

static bool load_checkpoint(void** data, size_t* len, uint32_t* frame) {
	if (!checkpoint_path[0]) return false;
	FILE* f = fopen(checkpoint_path, "rb");
	if (!f) return false;
	CheckpointHeader h;
	bool ok = fread(&h, 1, sizeof(h), f) == sizeof(h) &&
	          h.magic == CHECKPOINT_MAGIC && h.version == 2 &&
	          h.core_identity == core_identity() &&
	          h.state_size <= NETLINK_MAX_STATE &&
	          !memcmp(h.rom_sha256, rom_sha256, sizeof(h.rom_sha256));
	void* buf = ok ? malloc(h.state_size) : NULL;
	if (!buf || fread(buf, 1, h.state_size, f) != h.state_size) ok = false;
	if (ok && (!parse_authoritative_state(buf, h.state_size, NULL) ||
	           hash_bytes(buf, h.state_size) != h.state_hash)) ok = false;
	if (fgetc(f) != EOF) ok = false;
	fclose(f);
	if (!ok) { free(buf); shim_log("ignored invalid or stale host checkpoint\n"); return false; }
	*data = buf; *len = h.state_size; *frame = h.frame;
	shim_log("restored last confirmed host checkpoint from frame %u\n", h.frame);
	return true;
}

static void reset_netplay_timeline(uint32_t frame);
static void recovery_clock_start(void);

static bool identity_matches(const NetLinkSessionIdentity* peer) {
	uint32_t mine = core_identity();
	size_t state_size = core.serialize_size();
	uint32_t sram_size = 0, rtc_size = 0;
	if (!persistent_memory_sizes(&sram_size, &rtc_size)) return false;
	if (memcmp(peer->rom_sha256, rom_sha256, 32)) {
		shim_log("refusing peer: ROM content hashes differ\n");
		return false;
	}
	if (peer->mode != 1) {
		shim_log("refusing peer: mode differs (peer %u, ours 1)\n", peer->mode);
		return false;
	}
	adopt_peer_delay(peer->input_delay, "peer");
	if (peer->core_identity != mine || peer->state_size != state_size ||
	    peer->sram_size != sram_size || peer->rtc_size != rtc_size) {
		shim_log("refusing peer: core/state/persistent-memory identity differs "
		         "(peer %08x/%u/%u/%u, ours %08x/%zu/%u/%u)\n",
		         peer->core_identity, peer->state_size, peer->sram_size, peer->rtc_size,
		         mine, state_size, sram_size, rtc_size);
		return false;
	}
	return true;
}

static void recovery_fail(const char* message) {
	recovery_failed = 1;
	snprintf(recovery_error, sizeof(recovery_error), "%s", message);
	failure_choice = FAILURE_SOLO;
	failure_buttons_previous = 0;
	shim_log("%s\n", message);
}

/* Every TCP generation is a new synchronization barrier. This covers both a
 * guest relaunch (the live host donates its current state) and a host relaunch
 * (the new host first loads its last peer-confirmed checkpoint). */
static void netplay_handshake(void) {
	if (!NetLink_isConnected() || !rom_hash_ready) return;
	uint32_t generation = NetLink_connectionGeneration();
	if (generation != connection_generation) {
		connection_generation = generation;
		connection_identity_sent = connection_identity_checked = 0;
		connection_sync_pending = 1;
		netplay_synced = 0;
		recovery_failed = 0;
		recovery_error[0] = '\0';
		recovery_kind = NETLINK_RECOVERY_SYNC;
		recovery_promote_checkpoint = 0;
		host_reset_pending = 0;
		recovery_clock_start();
		recovery_phase = NetLink_getRole() == NETLINK_ROLE_CLIENT
		               ? RECOVERY_CLIENT_WAIT_BEGIN : RECOVERY_IDLE;
		NetLink_resetSync();
		shim_log("connection %u requires an authoritative sync\n", generation);
	}
	/* A failed barrier stays failed until the next connection; retrying it
	 * every frame only repeats the same failure. */
	if (recovery_failed) return;

	if (!connection_identity_sent) {
		/* Same settle-then-propose as the instanced path; recovery_started is
		 * set at the generation change, so it dates the connection. */
		if (!input_delay_pinned) {
			if (!rtt_settled(&recovery_started)) return;
			adopt_measured_delay(rtt_proposed_delay());
		}
		NetLinkSessionIdentity mine;
		memset(&mine, 0, sizeof(mine));
		memcpy(mine.rom_sha256, rom_sha256, 32);
		mine.mode = 1;
		mine.input_delay = (uint32_t)input_delay;
		mine.core_identity = core_identity();
		mine.state_size = (uint32_t)core.serialize_size();
		mine.wall_clock_utc = (uint64_t)time(NULL);
		if (!persistent_memory_sizes(&mine.sram_size, &mine.rtc_size)) {
			recovery_fail("Could not inspect local save memory. Exit the game.");
			return;
		}
		connection_identity_sent = NetLink_sendSessionIdentity(&mine);
	}

	if (!connection_identity_checked) {
		NetLinkSessionIdentity peer;
		if (!NetLink_takeSessionIdentity(&peer)) return;
		if (!identity_matches(&peer)) {
			recovery_fail("Peer game or core is incompatible. Exit the game.");
			return;
		}
		connection_identity_checked = 1;
		shim_log("ROM and core identity match peer; agreed input delay %d\n",
		         input_delay);
	}

	if (!connection_sync_pending || NetLink_getRole() != NETLINK_ROLE_HOST ||
	    recovery_phase != RECOVERY_IDLE) return;

	size_t sz = 0;
	void* buf = NULL;
	uint32_t source_frame = 0;
	bool restored = false;
	if (!netplay_ever_synced)
		restored = load_checkpoint(&buf, &sz, &source_frame);
	if (!restored) {
		if (!capture_authoritative_state(&buf, &sz, NULL)) {
			recovery_fail("Host could not capture synchronization state.");
			return;
		}
		source_frame = netplay_ever_synced ? netplay_frame : 0;
	}
	if (restored && !apply_authoritative_state(buf, sz)) {
		/* The checkpoint only resumes a relaunched host where it left off.
		 * One this core cannot load must not cost the session: drop it and
		 * synchronize from the state the host is actually running. */
		free(buf);
		buf = NULL;
		restored = false;
		unlink(checkpoint_path);
		shim_log("discarded unusable host checkpoint; syncing live state\n");
		if (!capture_authoritative_state(&buf, &sz, NULL)) {
			recovery_fail("Host could not capture synchronization state.");
			return;
		}
		source_frame = netplay_ever_synced ? netplay_frame : 0;
	}
	if (!restored && !apply_authoritative_state(buf, sz)) {
		free(buf);
		recovery_fail("Host could not adopt synchronization state.");
		return;
	}

	recovery_epoch++;
	if (!recovery_epoch) recovery_epoch++;
	recovery_resume_frame = (restored || netplay_ever_synced)
	                      ? source_frame + RECOVERY_FRAME_JUMP : 0;
	recovery_kind = NETLINK_RECOVERY_SYNC;
	if (!NetLink_beginResync(recovery_epoch, recovery_resume_frame, recovery_kind) ||
	    !NetLink_sendState(NETLINK_STATE_AUTHORITATIVE, buf, sz)) {
		free(buf); return;
	}
	set_checkpoint_candidate(buf, sz, recovery_resume_frame, hash_bytes(buf, sz));
	reset_netplay_timeline(recovery_resume_frame);
	recovery_phase = RECOVERY_HOST_WAIT_ACK;
	recovery_clock_start();
	shim_log("authoritative %s state sent for connection %u; resume frame %u\n",
	         restored ? "checkpoint" : (netplay_ever_synced ? "live" : "initial"),
	         generation, recovery_resume_frame);
}

static void reset_netplay_timeline(uint32_t frame) {
	netplay_frame = frame;
	timeline_start_frame = frame;
	memset(local_inputs, 0, sizeof(local_inputs));
	memset(frame_buttons, 0, sizeof(frame_buttons));
	last_scheduled = 0;
	input_scheduled = 0;
	stall_run = 0;
	input_stall_since.tv_sec = input_stall_since.tv_usec = 0;
	stat_frames = stat_stalls = stat_stall_max = 0;
	memset(&stat_since, 0, sizeof(stat_since));
	memset(own_hashes, 0, sizeof(own_hashes));
	memset(pending_peer_hashes, 0, sizeof(pending_peer_hashes));
	NetLink_resetTimeline();
}

static void recovery_clock_start(void) {
	gettimeofday(&recovery_started, NULL);
}

static long elapsed_ms(const struct timeval* since) {
	if (!since->tv_sec) return 0;
	struct timeval now;
	gettimeofday(&now, NULL);
	return (now.tv_sec - since->tv_sec) * 1000L
	     + (now.tv_usec - since->tv_usec) / 1000L;
}

static long peer_wait_timeout_ms(void) {
	static long timeout = -1;
	if (timeout >= 0) return timeout;
	timeout = PEER_WAIT_TIMEOUT_MS;
	const char* value = getenv("NETPLAY_PEER_TIMEOUT_MS");
	if (value) {
		long requested = strtol(value, NULL, 10);
		if (requested >= 100 && requested <= 300000) timeout = requested;
	}
	return timeout;
}

static bool recovery_timed_out(void) {
	if (!recovery_started.tv_sec) return false;
	return elapsed_ms(&recovery_started) > RECOVERY_TIMEOUT_MS;
}

static bool desync_recovery_allowed(void) {
	if (!desync_window_started.tv_sec || elapsed_ms(&desync_window_started) > DESYNC_WINDOW_MS) {
		gettimeofday(&desync_window_started, NULL);
		desync_recoveries = 0;
	}
	if (desync_recoveries >= MAX_DESYNC_RECOVERIES) return false;
	desync_recoveries++;
	return true;
}

/* Failed shared-screen sessions remain interactive through the ordinary
 * libretro joypad callback. Continuing solo changes only this emulator process:
 * the durable session and save/persistence restrictions remain in force. */
static void dual_reset_bootstrap(void);

static void retry_failed_session(void) {
	NetLink_stop();
	NetLink_resetSync();
	if (dual_link_mode) {
		/* The peer's replica of our console cannot be trusted across a break, so
		 * a retry is a fresh pairing rather than a resumed timeline. */
		dual_reset_bootstrap();
		dual_generation = 0;
	}
	connection_generation = 0;
	connection_identity_sent = connection_identity_checked = 0;
	connection_sync_pending = 1;
	netplay_synced = 0;
	recovery_phase = NetLink_getRole() == NETLINK_ROLE_CLIENT
	               ? RECOVERY_CLIENT_WAIT_BEGIN : RECOVERY_IDLE;
	peer_missing_since.tv_sec = peer_missing_since.tv_usec = 0;
	input_stall_since.tv_sec = input_stall_since.tv_usec = 0;
	recovery_failed = 0;
	recovery_error[0] = '\0';
	recovery_clock_start();
	if (!NetLink_start()) recovery_fail("Could not restart the network session.");
	else shim_log("user chose to wait and retry the peer connection\n");
}

static bool failure_input_tick(void) {
	if (exit_requested) return false;
	uint32_t buttons = fe_input_state
	                 ? (uint16_t)fe_input_state(0, RETRO_DEVICE_JOYPAD, 0,
	                                            RETRO_DEVICE_ID_JOYPAD_MASK)
	                 : 0;
	uint32_t a = 1u << RETRO_DEVICE_ID_JOYPAD_A;
	uint32_t b = 1u << RETRO_DEVICE_ID_JOYPAD_B;
	uint32_t left = 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
	uint32_t right = 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
	uint32_t pressed = buttons & ~failure_buttons_previous;
	failure_buttons_previous = buttons;

	if (pressed & left)
		failure_choice = (FailureChoice)((failure_choice + FAILURE_CHOICE_COUNT - 1) % FAILURE_CHOICE_COUNT);
	if (pressed & right)
		failure_choice = (FailureChoice)((failure_choice + 1) % FAILURE_CHOICE_COUNT);
	if (!(pressed & (a | b))) return false;

	/* B is the quick, conservative action: keep waiting. A confirms the
	 * highlighted choice. */
	FailureChoice choice = (pressed & b) ? FAILURE_WAIT : failure_choice;
	if (choice == FAILURE_WAIT) {
		retry_failed_session();
		return true;
	}
	if (choice == FAILURE_EXIT) {
		NetLink_stop();
		exit_requested = 1;
		shim_log("user requested exit from the netplay failure overlay\n");
		if (fe_environment) fe_environment(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
		return true;
	}

	NetLink_stop();
	solo_mode = 1;
	netplay_synced = 0;
	recovery_failed = 0;
	recovery_error[0] = '\0';
	recovery_phase = RECOVERY_IDLE;
	shim_log("user continued this game solo; durable session remains armed\n");
	return true;
}

static void failure_overlay_message(char* out, size_t len) {
	if (exit_requested) {
		snprintf(out, len, "Exiting game...");
		return;
	}
	const char* choice = failure_choice == FAILURE_WAIT ? "Wait and retry"
	                   : failure_choice == FAILURE_EXIT ? "Exit game"
	                   : "Continue solo";
	snprintf(out, len, "%s Choice: %s. Left Right, A select. B waits.",
	         recovery_error[0] ? recovery_error : "Synchronization failed.", choice);
}

/* Advance an authoritative-state recovery transaction at a frame boundary.
 * True means the core must remain paused for this retro_run call. */
static bool netplay_recovery_tick(void) {
	if (recovery_failed) return true;

	if (connection_sync_pending && !connection_identity_checked && recovery_timed_out()) {
		recovery_fail("Peer identity handshake timed out.");
		return true;
	}

	if (recovery_phase != RECOVERY_IDLE && recovery_timed_out()) {
		recovery_fail("Authoritative synchronization timed out.");
		return true;
	}

	if (NetLink_getRole() == NETLINK_ROLE_HOST) {
		if (recovery_phase == RECOVERY_IDLE && host_reset_pending) {
			host_reset_pending = 0;
			/* Reset only the authority, then serialize that exact post-reset
			 * machine state. The guest never calls core.reset(); adopting this
			 * bundle is the reset, and ACK/COMMIT keeps both timelines paused
			 * until it has succeeded. */
			core.reset();
			size_t sz = 0;
			void* buf = NULL;
			if (!capture_authoritative_state(&buf, &sz, NULL)) {
				recovery_fail("Host could not capture reset state.");
				return true;
			}

			recovery_epoch++;
			if (!recovery_epoch) recovery_epoch++;
			recovery_resume_frame = netplay_frame + RECOVERY_FRAME_JUMP;
			recovery_kind = NETLINK_RECOVERY_RESET;
			recovery_clock_start();

			bool sent = NetLink_beginResync(recovery_epoch, recovery_resume_frame,
			                                recovery_kind) &&
			            NetLink_sendState(NETLINK_STATE_AUTHORITATIVE, buf, sz);
			bool adopted = apply_authoritative_state(buf, sz);
			if (!sent || !adopted) {
				free(buf);
				recovery_fail("Host could not send or adopt reset state.");
				return true;
			}

			set_checkpoint_candidate(buf, sz, recovery_resume_frame, hash_bytes(buf, sz));
			recovery_promote_checkpoint = 1;
			reset_netplay_timeline(recovery_resume_frame);
			recovery_phase = RECOVERY_HOST_WAIT_ACK;
			shim_log("authoritative reset %u sent; resume frame %u\n",
			         recovery_epoch, recovery_resume_frame);
			return true;
		}

		uint32_t mismatch_frame;
		if (recovery_phase == RECOVERY_IDLE &&
		    NetLink_takeResyncRequest(&mismatch_frame)) {
			size_t sz = 0;
			void* buf = NULL;
			if (!capture_authoritative_state(&buf, &sz, NULL)) {
				recovery_fail("Host could not create authoritative recovery state.");
				return true;
			}

			recovery_epoch++;
			if (!recovery_epoch) recovery_epoch++;
			recovery_resume_frame = netplay_frame + RECOVERY_FRAME_JUMP;
			recovery_clock_start();

			recovery_kind = NETLINK_RECOVERY_SYNC;
			bool sent = NetLink_beginResync(recovery_epoch, recovery_resume_frame,
			                                recovery_kind) &&
			            NetLink_sendState(NETLINK_STATE_AUTHORITATIVE, buf, sz);
			bool adopted = apply_authoritative_state(buf, sz);
			free(buf);

			if (!sent || !adopted) {
				recovery_fail("Host could not send or adopt recovery state.");
				return true;
			}

			reset_netplay_timeline(recovery_resume_frame);
			recovery_phase = RECOVERY_HOST_WAIT_ACK;
			shim_log("authoritative resync %u sent after mismatch at frame %u; "
			         "resume frame %u\n", recovery_epoch, mismatch_frame,
			         recovery_resume_frame);
			return true;
		}

		if (recovery_phase == RECOVERY_HOST_WAIT_ACK) {
			uint32_t epoch;
			bool loaded;
			if (NetLink_takeResyncAck(&epoch, &loaded) && epoch == recovery_epoch) {
				if (!loaded || !NetLink_commitResync(epoch)) {
					recovery_fail("Guest could not commit authoritative recovery.");
					return true;
				}
				if (connection_sync_pending) {
					promote_checkpoint();
					netplay_synced = netplay_ever_synced = 1;
					connection_sync_pending = 0;
				}
				if (recovery_promote_checkpoint) {
					promote_checkpoint();
					recovery_promote_checkpoint = 0;
				}
				recovery_phase = RECOVERY_IDLE;
				recovery_started.tv_sec = recovery_started.tv_usec = 0;
				recovery_count++;
				shim_log("authoritative %s %u committed (%u %s this session)\n",
				         recovery_kind == NETLINK_RECOVERY_RESET ? "reset" : "resync",
				         epoch, recovery_count, recovery_count == 1 ? "recovery" : "recoveries");
				recovery_kind = NETLINK_RECOVERY_SYNC;
			}
			return true;
		}
		return false;
	}

	/* A normal desync puts the guest in WAIT_BEGIN after it asks for help. A
	 * host reset is deliberately unsolicited, so an idle guest must also honor
	 * the authority's BEGIN and pause before advancing another core frame. */
	if (recovery_phase == RECOVERY_CLIENT_WAIT_BEGIN ||
	    recovery_phase == RECOVERY_IDLE) {
		uint32_t epoch, frame;
		NetLinkRecoveryKind kind;
		if (NetLink_takeResyncBegin(&epoch, &frame, &kind)) {
			recovery_epoch = epoch;
			recovery_resume_frame = frame;
			recovery_kind = kind;
			recovery_phase = RECOVERY_CLIENT_WAIT_STATE;
			shim_log("authoritative %s %u beginning; resume frame %u\n",
			         kind == NETLINK_RECOVERY_RESET ? "reset" : "resync", epoch, frame);
		}
	}

	if (recovery_phase == RECOVERY_CLIENT_WAIT_STATE) {
		void* buf = NULL;
		size_t len = 0;
		if (NetLink_takeState(NETLINK_STATE_AUTHORITATIVE, &buf, &len)) {
			bool loaded = apply_authoritative_state(buf, len);
			free(buf);

			if (loaded) reset_netplay_timeline(recovery_resume_frame);
			if (!NetLink_ackResync(recovery_epoch, loaded) || !loaded) {
				recovery_fail("Guest could not adopt authoritative recovery state.");
				return true;
			}
			recovery_phase = RECOVERY_CLIENT_WAIT_COMMIT;
			shim_log("authoritative %s %u adopted; waiting for commit\n",
			         recovery_kind == NETLINK_RECOVERY_RESET ? "reset" : "resync",
			         recovery_epoch);
		}
	}

	if (recovery_phase == RECOVERY_CLIENT_WAIT_COMMIT) {
		uint32_t epoch;
		if (NetLink_takeResyncCommit(&epoch) && epoch == recovery_epoch) {
			if (connection_sync_pending) {
				netplay_synced = netplay_ever_synced = 1;
				connection_sync_pending = 0;
			}
			recovery_phase = RECOVERY_IDLE;
			recovery_started.tv_sec = recovery_started.tv_usec = 0;
			recovery_count++;
			shim_log("authoritative %s %u committed (%u %s this session)\n",
			         recovery_kind == NETLINK_RECOVERY_RESET ? "reset" : "resync",
			         epoch, recovery_count, recovery_count == 1 ? "recovery" : "recoveries");
			recovery_kind = NETLINK_RECOVERY_SYNC;
			return true;
		}
	}

	return recovery_phase != RECOVERY_IDLE;
}

// Report how the session is actually pacing. Attribution is the whole point:
// fps well under the core's own rate means this device cannot keep up, while a
// high stall share with healthy fps means we are waiting on the peer. Both feel
// like "lag" and the fixes are opposite.
static void netplay_reportPacing(void) {
	if (!stat_since.tv_sec) { gettimeofday(&stat_since, NULL); return; }
	if (netplay_frame % PACING_INTERVAL) return;

	struct timeval now;
	gettimeofday(&now, NULL);
	long ms = (now.tv_sec - stat_since.tv_sec) * 1000L
	        + (now.tv_usec - stat_since.tv_usec) / 1000L;
	if (ms <= 0) return;

	// Integer tenths throughout - no float formatting in a core .so.
	unsigned fps10   = (unsigned)((stat_frames * 10000UL) / (unsigned long)ms);
	unsigned total   = stat_frames + stat_stalls;
	unsigned stall_pct = total ? (unsigned)((stat_stalls * 100UL) / total) : 0;

	shim_log("pacing: %u frames in %ldms (%u.%u fps), stalled %u frames (%u%%), "
	         "longest %u, delay %d\n",
	         stat_frames, ms, fps10 / 10, fps10 % 10,
	         stat_stalls, stall_pct, stat_stall_max, input_delay);

	stat_frames = stat_stalls = stat_stall_max = 0;
	stat_since = now;
}

/* The link path carries real GB/GBA serial traffic over Wi-Fi and, alone of the
 * three session paths, reported nothing at all about how it was pacing. It is
 * the one whose timing is most fragile, so it was also the one we could not see
 * into: "trading feels slow" had no number attached to it.
 *
 * The histogram is the useful part. Attribution here is not fps - the cores
 * keep their own timing on this path - it is whether serial data is flowing
 * between frames or arriving frame-locked, which is exactly the before/after
 * signal for delivering packets mid-frame. */
/* Tunable so the threshold can be chosen from the telemetry above rather than
 * from taste, and so a device test can force it without a rebuild. */
static long link_starve_timeout_ms(void) {
	static long timeout = -1;
	if (timeout >= 0) return timeout;
	timeout = LINK_STARVE_TIMEOUT_MS;
	const char* value = getenv("NETPLAY_LINK_STARVE_MS");
	if (value) {
		long requested = strtol(value, NULL, 10);
		if (requested >= 500 && requested <= 300000) timeout = requested;
	}
	return timeout;
}

static uint32_t link_frames;
static uint32_t link_delivered;
static uint32_t link_paused_frames;
static uint32_t link_gap_buckets[4];   /* <1 frame, 1-2, 2-4, >4 */
static struct timeval link_last_delivery;
static struct timeval link_since;

static void link_note_delivery(void) {
	struct timeval now;
	gettimeofday(&now, NULL);
	link_delivered++;
	if (link_last_delivery.tv_sec) {
		uint32_t us = timeval_delta_us(&link_last_delivery, &now);
		unsigned frames16 = us / 16700;
		unsigned bucket = frames16 == 0 ? 0 : frames16 == 1 ? 1 : frames16 < 4 ? 2 : 3;
		link_gap_buckets[bucket]++;
	}
	link_last_delivery = now;
}

static void link_report_pacing(void) {
	if (!link_since.tv_sec) { gettimeofday(&link_since, NULL); return; }
	if (link_frames % PACING_INTERVAL) return;

	struct timeval now;
	gettimeofday(&now, NULL);
	long ms = (now.tv_sec - link_since.tv_sec) * 1000L
	        + (now.tv_usec - link_since.tv_usec) / 1000L;
	if (ms <= 0) return;

	unsigned fps10 = (unsigned)((link_frames * 10000UL) / (unsigned long)ms);
	shim_log("link pacing: %u frames in %ldms (%u.%u fps), %u packets, "
	         "%u dropped, %u frames peer-paused\n",
	         link_frames, ms, fps10 / 10, fps10 % 10, link_delivered,
	         NetLink_droppedPackets(), link_paused_frames);
	shim_log("link delivery gaps: sub-frame %u, 1-2 frames %u, 2-4 %u, over 4 %u\n",
	         link_gap_buckets[0], link_gap_buckets[1],
	         link_gap_buckets[2], link_gap_buckets[3]);

	link_frames = link_delivered = link_paused_frames = 0;
	memset(link_gap_buckets, 0, sizeof(link_gap_buckets));
	link_since = now;
}

// Cheap rolling checksum; we only need to notice divergence, not locate it.
static bool capture_state(void** out, size_t* out_len, uint32_t* out_hash) {
	return capture_authoritative_state(out, out_len, out_hash);
}

static bool netplay_checkDivergence(void) {
	if (NetLink_getRole() == NETLINK_ROLE_HOST) {
		uint32_t frame, hash;
		bool matched;
		while (NetLink_takeCheckpointAck(&frame, &hash, &matched)) {
			if (matched && checkpoint_candidate &&
			    frame == checkpoint_candidate_frame && hash == checkpoint_candidate_hash)
				promote_checkpoint();
			else if (!matched)
				shim_log("guest rejected checkpoint at frame %u\n", frame);
		}
	}

	// Both sides hash at exactly the same frames.
	//
	// retro_serialize is not guaranteed to be side-effect free, and picodrive's
	// is not: an extra call costs 25 bytes of divergence within 900 frames
	// (shim/test/statecheck.c --serprobe). The client used to hash only when it
	// reached the frame a host hash described, so a hash that arrived late meant
	// the host had serialized and the client had not - and the check that exists
	// to detect divergence became a cause of it. The observed signature was a
	// session in sync until the first missed check and desynced from then on.
	//
	// So hashing is unconditional and identically timed on both sides, and the
	// comparison is done separately, whenever the peer's hash turns up.
	if (netplay_frame % HASH_INTERVAL == 0) {
		void* snapshot = NULL;
		size_t snapshot_len = 0;
		uint32_t mine = 0;
		if (!capture_state(&snapshot, &snapshot_len, &mine)) return false;
		if (NetLink_getRole() == NETLINK_ROLE_HOST) {
			if (netplay_frame == 0) shim_log("state at frame 0: %08x\n", mine);
			set_checkpoint_candidate(snapshot, snapshot_len, netplay_frame, mine);
			NetLink_sendHash(netplay_frame, mine);
			return false;
		}
		free(snapshot);
		unsigned slot = (netplay_frame / HASH_INTERVAL) % HASH_HISTORY;
		own_hashes[slot] = (StateHash){ netplay_frame, mine, 1 };
	}

	if (NetLink_getRole() == NETLINK_ROLE_HOST) return false;

	/* The host can reach a checkpoint slightly before this device. Keep those
	 * hashes instead of popping and discarding them merely because our matching
	 * serialize has not happened yet. Keying both rings by frame makes ordering
	 * irrelevant and a later comparison uses exactly the symmetric snapshots. */
	uint32_t f, h;
	while (NetLink_takeHash(&f, &h)) {
		unsigned p = (f / HASH_INTERVAL) % HASH_HISTORY;
		pending_peer_hashes[p] = (StateHash){ f, h, 1 };
	}

	for (unsigned p = 0; p < HASH_HISTORY; p++) {
		if (!pending_peer_hashes[p].valid) continue;
		f = pending_peer_hashes[p].frame;
		h = pending_peer_hashes[p].hash;
		unsigned s = (f / HASH_INTERVAL) % HASH_HISTORY;
		if (!own_hashes[s].valid || own_hashes[s].frame != f) continue;
		pending_peer_hashes[p].valid = 0;
		if (f == 0)
			shim_log("state at frame 0: ours %08x, host %08x - handshake %s\n",
			         own_hashes[s].hash, h,
			         own_hashes[s].hash == h ? "equalised both sides" : "did NOT equalise");
		if (own_hashes[s].hash != h) {
			if (!NetLink_ackCheckpoint(f, h, false)) return true;
			shim_log("DESYNC at frame %u (host %08x, ours %08x)\n",
			         f, h, own_hashes[s].hash);
			if (recovery_phase == RECOVERY_IDLE) {
				if (!desync_recovery_allowed()) {
					recovery_fail("Repeated desyncs. Press A to continue solo.");
					return true;
				}
				if (!NetLink_requestResync(f)) return true;
				recovery_phase = RECOVERY_CLIENT_WAIT_BEGIN;
				recovery_clock_start();
				shim_log("requested authoritative state from host\n");
				return true;
			}
		} else {
			if (!NetLink_ackCheckpoint(f, h, true)) return true;
			shim_log("in sync at frame %u (%08x)\n", f, own_hashes[s].hash);
		}
	}
	return false;
}

// Drive the core's netpacket callbacks to match the link state. Called once per
// frame from retro_run, before the core runs.
static void pump_netpacket(void) {
	if (!have_netpacket || !session_active) return;

	/* A disconnect and reconnect can both happen while MinArch is in its menu.
	 * Retire the old core session before starting the new one; doing this in the
	 * opposite order leaves the newly connected core immediately stopped. */
	if (NetLink_consumeDisconnectEvent() && netpacket_started) {
		if (core_netpacket.disconnected)
			core_netpacket.disconnected(NetLink_remoteClientId());
		if (core_netpacket.stop) core_netpacket.stop();
		netpacket_started = 0;
		shim_log("netpacket session stopped\n");
	}

	if (NetLink_consumeConnectEvent()) {
		if (netpacket_started) {
			if (core_netpacket.disconnected)
				core_netpacket.disconnected(NetLink_remoteClientId());
			if (core_netpacket.stop) core_netpacket.stop();
		}
		core_netpacket.start(NetLink_localClientId(),
		                     shim_netpacket_send,
		                     shim_netpacket_poll_receive);
		netpacket_started = 1;

		// Two players, and the peer is present the moment we are connected.
		if (core_netpacket.connected) {
			core_netpacket.connected(NetLink_remoteClientId());
		}
		shim_log("netpacket session started\n");
	}

	if (!netpacket_started) return;

	/* One budget per frontend frame, refilled here and spent by both this drain
	 * and any poll_receive the core makes before the frame ends. */
	netpacket_budget = MAX_PACKETS_PER_FRAME;
	deliver_packets();

	if (core_netpacket.poll) core_netpacket.poll();
}

static void present_paused_frame(unsigned frame_counter, const char* msg);

static void shim_video_refresh(const void* data, unsigned width, unsigned height, size_t pitch) {
	if (data && session_active) {
		size_t need = pitch * height;
		if (need > last_frame_cap) {
			void* grown = realloc(last_frame, need);
			if (grown) { last_frame = grown; last_frame_cap = need; }
		}
		if (last_frame && need <= last_frame_cap) {
			memcpy(last_frame, data, need);
			last_frame_w = width;
			last_frame_h = height;
			last_frame_pitch = pitch;
		}
	}
	/* This is an overlay, not a loading pause: core.run() produced the frame
	 * above and continues producing audio/input normally. Start the two-second
	 * clock at the first visible frame so a slow core load cannot consume the
	 * entire notice before there is anything on screen. */
	if (startup_notice[0] && last_frame && last_frame_w && last_frame_h) {
		if (!startup_notice_started.tv_sec)
			gettimeofday(&startup_notice_started, NULL);
		if (elapsed_ms(&startup_notice_started) < STARTUP_NOTICE_MS) {
			present_paused_frame(startup_notice_frame++, startup_notice);
			return;
		}
		startup_notice[0] = '\0';
	}
	if (fe_video_refresh) fe_video_refresh(data, width, height, pitch);
}

static size_t pixel_bytes(void) {
	return pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
}

// Whether the frontend has a frame of ours to hold on screen. When it does, a
// short stall can simply present nothing and let that frame stand.
static int have_last_frame(void) {
	return last_frame && last_frame_w && last_frame_h;
}

// Present a dimmed copy of the last frame with a status line and a sweeping
// bar beneath it, both anchored to the bottom so they never cover the action.
//
// A NULL msg re-presents the last frame untouched. That is not the same as
// returning without presenting: skipping fe_video_refresh entirely does not
// leave the previous frame on screen - it leaves whatever the frontend last
// had, which is how a pre-first-frame stall produced a black screen. During a
// burst of short stalls that alternation between real frames and nothing is
// visible as flicker, so a stalled frame must still hand the frontend
// something, even when there is nothing new to say.
static void present_paused_frame(unsigned frame_counter, const char* msg) {
	if (!fe_video_refresh) return;

	// Nothing to overlay: hand back the frame we already have, no copy, no dim.
	// This runs at frame rate during a stall burst, so it stays cheap.
	if (!msg && have_last_frame()) {
		fe_video_refresh(last_frame, last_frame_w, last_frame_h, last_frame_pitch);
		return;
	}

	unsigned fw    = last_frame_w;
	unsigned fh    = last_frame_h;
	size_t   fpitch = last_frame_pitch;
	int      have_last = have_last_frame();

	// Shared-screen netplay gates ahead of the first core.run(), so there is no
	// last frame precisely when the waiting message matters most. Presenting
	// nothing leaves the frontend on a black screen with no explanation - which
	// is indistinguishable from a hang, and was exactly that on device. Fall
	// back to a black frame in the core's own geometry.
	if (!have_last) {
		fw     = av_base_width;
		fh     = av_base_height;
		fpitch = (size_t)fw * pixel_bytes();
		if (!fw || !fh) return;   // av_info not seen yet; nothing sane to draw
	}

	static void*  scratch;
	static size_t scratch_cap;
	size_t need = fpitch * fh;
	if (need > scratch_cap) {
		void* grown = realloc(scratch, need);
		if (!grown) return;
		scratch = grown;
		scratch_cap = need;
	}
	if (have_last) memcpy(scratch, last_frame, need);
	else           memset(scratch, 0, need);

	OVL_Target t = {
		.pixels = scratch,
		.width  = fw,
		.height = fh,
		.pitch  = fpitch,
		.format = (OVL_Format)pixel_format,
	};
	// Dimming an already-black frame is a no-op, so this stays unconditional.
	OVL_dim(&t);

	const char* MSG = msg;

	int scale = (fw >= 480) ? 2 : 1;
	// Separate margins: the bottom one is visual breathing room, the side one
	// only decides wrapping. Keeping the sides tight lets a GBA fit the message
	// on one line - it is 233px against a 240px screen.
	int margin_y = 4 * scale;
	int margin_x = 2 * scale;
	int avail    = (int)fw - margin_x * 2;

	char lines[4][OVL_MAX_LINE];
	int  nlines = OVL_wrap(MSG, scale, avail, lines, 4);
	if (nlines <= 0) return;

	int line_h = OVL_GLYPH_H * scale;
	int leading = 2 * scale;
	int bar_h  = 2 * scale;
	int gap    = 3 * scale;

	int block_h = nlines * line_h + (nlines - 1) * leading + gap + bar_h;
	int top     = (int)fh - margin_y - block_h;
	if (top < 0) top = 0;

	// Centre on ink, not advance width, so a line ending in '.' is not pushed
	// left by the blank columns a period reserves.
	int widest = 0, widest_x = 0;
	for (int i = 0; i < nlines; i++) {
		int bearing, ink = OVL_textInk(lines[i], scale, &bearing);
		if (ink > widest) {
			widest = ink;
			widest_x = ((int)fw - ink) / 2;
		}
	}
	if (widest_x < 0) widest_x = 0;

	int y = top;
	for (int i = 0; i < nlines; i++) {
		int bearing, ink = OVL_textInk(lines[i], scale, &bearing);
		int x = ((int)fw - ink) / 2 - bearing;
		if (x < 0) x = 0;
		OVL_drawText(&t, x, y, lines[i], scale);
		y += line_h + leading;
	}

	// The sweep sits directly beneath the text and spans the same width, so the
	// two read as one element rather than unrelated marks.
	int track_x = widest_x;
	int span = widest / 4 ? widest / 4 : 1;
	int head = (int)((frame_counter * 2) % (unsigned)(widest + span));
	int from = head > span ? head - span : 0;
	int to   = head < widest ? head : widest;

	if (to > from)
		OVL_fillRect(&t, track_x + from, top + block_h - bar_h, to - from, bar_h);

	fe_video_refresh(scratch, fw, fh, fpitch);
}

// Silence for one frame. Without it the frontend's audio buffer drains and
// crackles, which sounds like a fault rather than a pause.
static void present_paused_audio(void) {
	if (!fe_audio_sample_batch || av_fps <= 0.0) return;
	size_t frames = (size_t)(av_sample_rate / av_fps);
	if (!frames || frames > 4096) return;
	static int16_t silence[4096 * 2];
	fe_audio_sample_batch(silence, frames);
}

static void shim_audio_sample(int16_t left, int16_t right) {
	if (fe_audio_sample) fe_audio_sample(left, right);
}

static size_t shim_audio_sample_batch(const int16_t* data, size_t frames) {
	return fe_audio_sample_batch ? fe_audio_sample_batch(data, frames) : 0;
}

static void shim_input_poll(void) {
	if (fe_input_poll) fe_input_poll();
}

static int16_t shim_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
	if (dual_link_mode && (dual_bootstrapped || solo_mode) &&
	    device == RETRO_DEVICE_JOYPAD && index == 0 && port < 2) {
		uint32_t buttons = port == dual_local_console
		                 ? dual_primary_buttons : dual_shadow_buttons;
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)buttons;
		return (buttons >> id) & 1;
	}
	// Shared-screen netplay drives both ports from the synced buffers. Stock
	// minarch returns 0 for every port above 0, which is exactly why the patched
	// build had to add this - the second player has to come from somewhere.
	if (netplay_mode && netplay_synced && device == RETRO_DEVICE_JOYPAD && index == 0 && port < 2) {
		uint32_t b = frame_buttons[port];
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)b;
		return (b >> id) & 1;
	}
	return fe_input_state ? fe_input_state(port, device, index, id) : 0;
}

//////////////////////////////////////////////////////////////////////////////
// libretro entry points
//////////////////////////////////////////////////////////////////////////////

void retro_set_environment(retro_environment_t cb) {
	fe_environment = cb;
	ensure_loaded();
	core.set_environment(shim_environment);
}

void retro_set_video_refresh(retro_video_refresh_t cb) {
	fe_video_refresh = cb;
	ensure_loaded();
	core.set_video_refresh(shim_video_refresh);
}

void retro_set_audio_sample(retro_audio_sample_t cb) {
	fe_audio_sample = cb;
	ensure_loaded();
	core.set_audio_sample(shim_audio_sample);
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
	fe_audio_sample_batch = cb;
	ensure_loaded();
	core.set_audio_sample_batch(shim_audio_sample_batch);
}

void retro_set_input_poll(retro_input_poll_t cb) {
	fe_input_poll = cb;
	ensure_loaded();
	core.set_input_poll(shim_input_poll);
}

void retro_set_input_state(retro_input_state_t cb) {
	fe_input_state = cb;
	ensure_loaded();
	core.set_input_state(shim_input_state);
}

void retro_init(void) {
	ensure_loaded();
	core.init();
}

void retro_deinit(void) {
	if (!core.handle) return;

	// Tell the core the session is over before it tears itself down, otherwise
	// it deinits holding a netpacket interface it thinks is still live.
	if (netplay_mode) {
		netplay_synced = 0;
		input_scheduled = 0;
		last_scheduled = 0;
		NetLink_resetSync();
	}

	if (netpacket_started) {
		if (core_netpacket.disconnected) core_netpacket.disconnected(NetLink_remoteClientId());
		if (core_netpacket.stop)         core_netpacket.stop();
		netpacket_started = 0;
	}
	if (session_active) NetLink_stop();
	free(checkpoint_candidate);
	checkpoint_candidate = NULL;
	checkpoint_candidate_len = 0;
	free(dual_own_rom);
	dual_own_rom = NULL;
	dual_own_rom_len = 0;
	free(dual_hash_buf);
	dual_hash_buf = NULL;
	dual_hash_buf_len = 0;
	cpu_restore();
	core.deinit();
}

unsigned retro_api_version(void) {
	ensure_loaded();
	return core.api_version();
}

void retro_get_system_info(struct retro_system_info* info) {
	ensure_loaded();
	// Verbatim: minarch reads valid_extensions and need_fullpath from here.
	core.get_system_info(info);
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	ensure_loaded();
	core.get_system_av_info(info);
	if (info->timing.fps > 0.0)         av_fps = info->timing.fps;
	if (info->timing.sample_rate > 0.0) av_sample_rate = info->timing.sample_rate;
	if (info->geometry.base_width)      av_base_width  = info->geometry.base_width;
	if (info->geometry.base_height)     av_base_height = info->geometry.base_height;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
	if (!core.handle) return;
	core.set_controller_port_device(port, device);
}

void retro_reset(void) {
	if (!core.handle) return;
	if (dual_link_mode && !solo_mode) {
		if (!dual_bootstrapped || !core.dual_reset_console) {
			show_runtime_notice("Reset unavailable while synchronizing.");
			return;
		}
		dual_reset_pending = 1;
		show_runtime_notice("Resetting this linked console...");
		shim_log("queued reset for logical console %c\n", dual_local_console ? 'B' : 'A');
		return;
	}
	if (session_active && netplay_mode && !solo_mode) {
		if (NetLink_getRole() != NETLINK_ROLE_HOST) {
			show_runtime_notice("Only the host can reset shared-screen play.");
			shim_log("guest reset rejected during shared-screen netplay\n");
			return;
		}
		if (!netplay_synced || recovery_phase != RECOVERY_IDLE || recovery_failed) {
			show_runtime_notice("Reset unavailable while synchronizing.");
			shim_log("host reset rejected while synchronization is active\n");
			return;
		}
		host_reset_pending = 1;
		shim_log("host reset queued for an authoritative frame-boundary transaction\n");
		return;
	}
	/* Link play represents independent consoles, and a shared-screen process
	 * that explicitly continued solo is independent for the rest of its life. */
	core.reset();
}

static const char* recovery_status_message(void) {
	if (recovery_kind == NETLINK_RECOVERY_RESET)
		return NetLink_getRole() == NETLINK_ROLE_HOST
		       ? "Resetting both players..."
		       : "Host reset. Synchronizing...";
	if (!netplay_synced)
		return NetLink_getRole() == NETLINK_ROLE_HOST
		       ? "Sending starting state..."
		       : "Receiving state from host...";
	return NetLink_getRole() == NETLINK_ROLE_HOST
	       ? "Sending authoritative state..."
	       : "Resynchronizing from host...";
}

/* A dual-link failure is recoverable unless it is a statement about the two
 * builds. Everything that can be blamed on the network goes through the same
 * Wait / Continue solo / Exit overlay shared-screen sessions already use, so a
 * dropped packet costs a resynchronization rather than the session. */
static void dual_fail(const char* message) {
	dual_failed = 1;
	snprintf(dual_error, sizeof(dual_error), "%s", message);
	shim_log("instanced Gambatte failed: %s\n", dual_error);
}

static void dual_reset_bootstrap(void) {
	dual_identity_sent = dual_identity_checked = 0;
	dual_verdict_sent = dual_verdict_agreed = 0;
	dual_state_sent = dual_state_loaded = 0;
	dual_checkpoint_sent = dual_checkpoint_loaded = 0;
	dual_ready_sent = 0;
	dual_bootstrapped = 0;
	dual_peer_rom_found = dual_same_rom = 0;
	dual_peer_rom_zipped = 0;
	dual_peer_rom_path[0] = '\0';
	dual_wait_since.tv_sec = dual_wait_since.tv_usec = 0;
	memset(own_hashes, 0, sizeof(own_hashes));
	memset(pending_peer_hashes, 0, sizeof(pending_peer_hashes));
	dual_hash_skips = 0;
	dual_state_size = 0;
	NetLink_resetSync();
}

/* The transport is gone or misbehaving. Tear the pairing down to its bootstrap
 * so the next connection generation re-establishes both consoles from scratch,
 * and hand the player the ordinary failure overlay meanwhile. Resuming an
 * instanced timeline across a reconnect is not possible: the peer's replica of
 * our console advanced under inputs we can no longer prove it received. */
static void dual_recoverable_fail(const char* message) {
	if (recovery_failed) return;
	dual_reset_bootstrap();
	recovery_fail(message);
}

/* Hand the launcher a demotion request and ask the frontend to close. The
 * paired core cannot speak Gambatte's network Game Link - it owns the cable
 * between its own two consoles - so a pairing this device cannot host has to
 * become a different process running the network-serial core. */
static void request_serial_fallback(uint32_t reason) {
	static const char* WHY[] = {
		"agreed", "cartridge not installed on both devices",
		"paired core unavailable", "core builds differ",
	};
	const char* why = reason < sizeof(WHY) / sizeof(WHY[0]) ? WHY[reason] : "unknown";
	const char* marker = getenv(SHIM_ENV_FALLBACK);
	if (marker && marker[0]) {
		FILE* f = fopen(marker, "w");
		if (f) { fprintf(f, "%u\n", reason); fclose(f); }
		else shim_log("could not write serial-fallback marker %s: %s\n",
		              marker, strerror(errno));
	} else {
		shim_log("no serial-fallback marker configured; cannot demote automatically\n");
	}
	shim_log("instanced link declined (%s); relaunching on network serial\n", why);
	dual_demoted = 1;
	exit_requested = 1;
	if (fe_environment) fe_environment(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}

/* Find the peer's cartridge in the local library.
 *
 * Linked play does not require equal ROMs, but it does require that this
 * device can run the peer's console, and no ROM ever crosses the network. Size
 * comes over with the identity precisely so this is a directory walk plus a
 * hash or two rather than hashing an entire Roms tree: distinct Game Boy
 * cartridges of the same length are common, cheap to reject, and rare enough
 * that the SHA-256 is computed only a handful of times.
 *
 * Archives are deliberately not opened. minarch hands us extracted content, so
 * a zipped copy of the peer's ROM cannot be size-matched from the outside and
 * that pairing falls back to network serial. */
static bool romscan_sha256(const void* data, size_t len, uint8_t out[32]) {
	Sha256 hash;
	sha256_init(&hash);
	sha256_update(&hash, data, len);
	sha256_final(&hash, out);
	return true;
}

/* Where the peer's cartridge might be, best bet first.
 *
 * A linked Game Boy cartridge lives in a Game Boy folder, so searching those
 * before the rest of the library is worth doing: on one device that is 2078
 * archives instead of 8489, and 1.6s instead of 6.9s on a cold cache. The whole
 * tree is still searched if that misses, because nothing enforces where a
 * player keeps a file. */
static void peer_rom_dirs(char roots[3][512], const char* dirs[4]) {
	const char* sd = getenv("SDCARD_PATH");
	if (!sd || !sd[0]) sd = "/mnt/SDCARD";
	snprintf(roots[0], 512, "%s/Roms/Game Boy Color (GBC)", sd);
	snprintf(roots[1], 512, "%s/Roms/Game Boy (GB)", sd);
	snprintf(roots[2], 512, "%s/Roms", sd);
	dirs[0] = roots[0];
	dirs[1] = roots[1];
	dirs[2] = roots[2];
	dirs[3] = NULL;
}

/* Long enough for a cold walk of a large library - a measured 2078-archive
 * Game Boy folder takes 1.6s, and the whole tree about 7s - and short enough
 * that a card which has stopped answering does not take the session with it. */
#define PEER_ROM_SEARCH_BUDGET_MS 12000

static bool locate_peer_rom(const uint8_t sha256[32], uint32_t size, uint32_t crc32) {
	char roots[3][512];
	const char* dirs[4];
	peer_rom_dirs(roots, dirs);

	/* The cache belongs beside the pak that produced it, and the pak's location
	 * is only known here through the core we were asked to wrap:
	 * <pak>/cores/override/<platform>/<core>.so. Deriving it from that beats
	 * an environment variable nothing in this process actually sets. */
	char cache_path[512];
	const char* core_path = getenv(SHIM_ENV_REAL_CORE);
	cache_path[0] = '\0';
	if (core_path && core_path[0]) {
		char pak[400];
		snprintf(pak, sizeof(pak), "%s", core_path);
		int trimmed = 0;
		for (; trimmed < 4; trimmed++) {
			char* slash = strrchr(pak, '/');
			if (!slash) break;
			*slash = '\0';
		}
		if (trimmed == 4 && pak[0]) {
			const char* state = getenv("NETPLAY_STATE");
			if (state && state[0]) snprintf(cache_path, sizeof(cache_path), "%s/romscan.cache", state);
			else snprintf(cache_path, sizeof(cache_path), "%s/state/romscan.cache", pak);
		}
	}
	/* Without a usable pak directory the cache simply does not persist; the
	 * search still works, it is only slower. */
	RomScanCache* cache = cache_path[0] ? romscan_cache_open(cache_path) : NULL;

	struct timeval started, finished;
	gettimeofday(&started, NULL);
	/* A large library can take this well past a frame. That is work, not a
	 * frontend that has gone away, and telling the peer we are stalled would
	 * only make it stop sending us the thing we are about to need. */
	NetLink_setCoreRunning(true);
	bool zipped = false;
	size_t examined = 0;
	/* Bounded, because the peer is blocked on this. A library on a slow or
	 * damaged card can take arbitrarily long to walk, and an unbounded search
	 * here freezes both devices with nothing on screen to distinguish it from a
	 * crash - which is exactly what happened. Giving up early costs a serial
	 * fallback; not giving up costs the session. */
	const RomScanResult result =
		romscan_find(cache, dirs, sha256, size, crc32,
		             dual_peer_rom_path, sizeof(dual_peer_rom_path),
		             &dual_peer_rom_zip, &zipped, PEER_ROM_SEARCH_BUDGET_MS,
		             &examined, romscan_sha256);
	const bool found = result == ROMSCAN_FOUND;
	NetLink_setCoreRunning(false);
	gettimeofday(&finished, NULL);
	dual_peer_rom_zipped = found && zipped;

	const size_t known = romscan_cache_count(cache);
	const size_t added = romscan_cache_added(cache);
	romscan_cache_close(cache);

	const unsigned took_ms = timeval_delta_us(&started, &finished) / 1000;
	if (found)
		shim_log("peer cartridge found %s%s (%ums, %zu examined, %zu archives "
		         "known, %zu newly read)\n",
		         dual_peer_rom_path, dual_peer_rom_zipped ? " (in archive)" : "",
		         took_ms, examined, known, added);
	else if (result == ROMSCAN_TIMED_OUT)
		shim_log("gave up looking for the peer cartridge (%u bytes, crc32 %08x) "
		         "after %ums and %zu entries; %zu archives known, %zu newly read\n",
		         size, crc32, took_ms, examined, known, added);
	else
		shim_log("peer cartridge (%u bytes, crc32 %08x) is not installed (%ums, "
		         "%zu examined, %zu archives known, %zu newly read)\n",
		         size, crc32, took_ms, examined, known, added);
	return found;
}

/* Reload the pair with one ROM per console. Console A is always index 0 and
 * console B index 1, so both handhelds build the same two-console machine
 * regardless of which one they present. minarch has already populated the
 * visible console's SRAM from this device's .sav, and that is the only copy of
 * the player's save in the process, so it is carried across the reload. */
static bool dual_rebuild_content(const char* peer_rom) {
	bool linked = peer_rom && peer_rom[0];
	/* Everything that can fail is checked before the running content is torn
	 * down. Past core.unload_game() there is no clean way back except loading
	 * something, and the frontend would be left with a core holding no game. */
	if (!dual_own_rom || !dual_own_rom_len || (linked && !core.load_game_special)) {
		shim_log("cannot rebuild the pair: %s\n",
		         !dual_own_rom_len ? "local cartridge was not retained"
		                           : "core has no two-content entry point");
		return false;
	}
	uint32_t sram_size = 0, rtc_size = 0;
	if (!dual_memory_sizes(dual_local_console, &sram_size, &rtc_size)) return false;
	uint8_t* sram = sram_size ? malloc(sram_size) : NULL;
	uint8_t* rtc = rtc_size ? malloc(rtc_size) : NULL;
	bool ok = (!sram_size || sram) && (!rtc_size || rtc);
	if (ok && sram_size) {
		void* p = core.dual_get_memory_data(dual_local_console, RETRO_MEMORY_SAVE_RAM);
		if (p) memcpy(sram, p, sram_size); else ok = false;
	}
	if (ok && rtc_size) {
		void* p = core.dual_get_memory_data(dual_local_console, RETRO_MEMORY_RTC);
		if (p) memcpy(rtc, p, rtc_size); else ok = false;
	}

	void* peer_data = NULL;
	size_t peer_len = 0;
	if (ok && linked) {
		ok = dual_peer_rom_zipped
		   ? romscan_zip_extract(peer_rom, &dual_peer_rom_zip, &peer_data, &peer_len)
		   : read_whole_file(peer_rom, &peer_data, &peer_len);
	}

	struct retro_game_info own;
	memset(&own, 0, sizeof(own));
	own.path = dual_local_rom_path[0] ? dual_local_rom_path : NULL;
	own.data = dual_own_rom;
	own.size = dual_own_rom_len;

	if (ok) {
		core.unload_game();
		ok = core.dual_set_visible_console(dual_local_console);
		if (ok && linked) {
			struct retro_game_info infos[2];
			memset(infos, 0, sizeof(infos));
			/* Index is the console, not the owner: console A is always 0 on both
			 * handhelds, so each builds the same two-console machine. */
			unsigned local = dual_local_console == GAMBATTE_DUAL_CONSOLE_A ? 0 : 1;
			infos[local] = own;
			infos[!local].path = peer_rom;
			infos[!local].data = peer_data;
			infos[!local].size = peer_len;
			ok = core.load_game_special(GAMBATTE_DUAL_SUBSYSTEM_ID, infos, 2);
		} else if (ok) {
			ok = core.load_game(&own);
		}
		if (!ok) {
			/* Put the player's own game back rather than leaving the frontend
			 * attached to a core with no content. The session is finished either
			 * way, but a visible game beats a black screen. */
			core.dual_set_visible_console(dual_local_console);
			if (!core.load_game(&own))
				shim_log("could not restore the local cartridge after a failed pair rebuild\n");
		}
	}

	/* Restore after the reload: the core reallocates both consoles' memory. */
	if (ok && sram_size) {
		void* p = core.dual_get_memory_data(dual_local_console, RETRO_MEMORY_SAVE_RAM);
		uint32_t now_size = 0, now_rtc = 0;
		if (p && dual_memory_sizes(dual_local_console, &now_size, &now_rtc) &&
		    now_size == sram_size) memcpy(p, sram, sram_size);
		else ok = false;
	}
	if (ok && rtc_size) {
		void* p = core.dual_get_memory_data(dual_local_console, RETRO_MEMORY_RTC);
		if (p) memcpy(p, rtc, rtc_size); else ok = false;
	}

	free(sram);
	free(rtc);
	free(peer_data);
	if (ok && linked)
		shim_log("loaded linked cartridges: console %c local, console %c from %s\n",
		         dual_local_console ? 'B' : 'A', dual_peer_console ? 'B' : 'A', peer_rom);
	else if (ok)
		shim_log("rebuilt the pair from the local cartridge alone\n");
	return ok;
}

/* Both handhelds end up running the same two consoles. Each sends its own
 * console's save memory once and loads the peer's into its replica, which is
 * what carries SRAM/RTC across without ever persisting the peer's save locally.
 * After the ready barrier only inputs and diagnostics use Wi-Fi.
 *
 * Ahead of that sits the agreement: identity, then a verdict each. Instanced
 * play needs *both* devices able to run *both* cartridges, and only this side
 * can answer that about this side's library - so the verdict is exchanged and
 * ANDed rather than inferred. A no from either demotes both to network serial,
 * which is a relaunch, not a failure. */
static bool dual_bootstrap_tick(void) {
	if (dual_failed || dual_bootstrapped) return dual_bootstrapped;
	/* Already asked the launcher for the network-serial core; the frontend is
	 * shutting down. Asking again every frame would rewrite the marker and
	 * re-issue SHUTDOWN for as long as it takes to get there. */
	if (dual_demoted) return false;

	if (!NetLink_isConnected()) {
		if (!dual_wait_since.tv_sec) gettimeofday(&dual_wait_since, NULL);
		if (elapsed_ms(&dual_wait_since) > peer_wait_timeout_ms())
			dual_recoverable_fail("Peer unavailable.");
		return false;
	}

	uint32_t generation = NetLink_connectionGeneration();
	if (generation != dual_generation) {
		dual_generation = generation;
		dual_reset_bootstrap();
		shim_log("connection %u requires a fresh instanced bootstrap\n", generation);
	}
	if (!dual_wait_since.tv_sec) gettimeofday(&dual_wait_since, NULL);
	if (elapsed_ms(&dual_wait_since) > peer_wait_timeout_ms()) {
		dual_recoverable_fail("Peer stopped responding during setup.");
		return false;
	}

	if (!dual_identity_sent) {
		if (!rom_hash_ready) {
			dual_fail("Could not identify the local cartridge.");
			return false;
		}
		/* Let the probes settle before proposing - one sample is not a
		 * maximum - but never wait past that: a link that has not answered a
		 * probe in RTT_SETTLE_MS is not one whose measurement we would trust
		 * anyway, and the configured value stands. */
		if (!input_delay_pinned) {
			if (!rtt_settled(&dual_wait_since)) return false;
			adopt_measured_delay(rtt_proposed_delay());
		}
		NetLinkSessionIdentity mine;
		memset(&mine, 0, sizeof(mine));
		memcpy(mine.rom_sha256, rom_sha256, sizeof(mine.rom_sha256));
		mine.mode = 2; /* instanced link */
		mine.input_delay = (uint32_t)input_delay;
		mine.core_identity = core_identity();
		mine.rom_size = dual_local_rom_size;
		/* CRC32 of the same bytes rom_sha256 covers. A zip records this for its
		 * contents, so the peer can reject almost every candidate in a library
		 * without decompressing any of it. It is a filter only - the SHA-256
		 * still decides. */
		mine.rom_crc32 = dual_local_rom_crc32;
		mine.wall_clock_utc = (uint64_t)time(NULL);
		dual_local_wall_clock = mine.wall_clock_utc;
		/* state_size and the save-memory sizes describe the *pair*, and the pair
		 * is not final until linked content is loaded below. They are checked
		 * where they are used: apply_dual_memory compares sizes against the
		 * console it is writing, and the checkpoint length must equal
		 * dual_serialize_size() exactly. */
		if (!NetLink_sendSessionIdentity(&mine)) return false;
		dual_identity_sent = 1;
		shim_log("sent instanced-link identity\n");
	}

	if (!dual_identity_checked) {
		NetLinkSessionIdentity peer;
		if (!NetLink_takeSessionIdentity(&peer)) return false;
		if (peer.mode != 2) {
			dual_fail("Peer is not set up for instanced link play.");
			return false;
		}
		adopt_peer_delay(peer.input_delay, "peer");
		if (peer.core_identity != core_identity()) {
			request_serial_fallback(NETLINK_LINK_CORE_MISMATCH);
			return false;
		}
		dual_same_rom = !memcmp(peer.rom_sha256, rom_sha256, sizeof(rom_sha256));
		/* A GB cartridge is at most 8MB; anything else is not one, and hunting
		 * for it would only waste the player's time. */
		if (dual_same_rom) {
			dual_peer_rom_found = 1;
			shim_log("both consoles run the same cartridge\n");
		} else if (peer.rom_size && peer.rom_size <= 8u * 1024u * 1024u) {
			dual_peer_rom_found = locate_peer_rom(peer.rom_sha256, peer.rom_size,
			                                      peer.rom_crc32);
		} else {
			dual_peer_rom_found = 0;
			shim_log("peer declared an implausible cartridge size %u\n", peer.rom_size);
		}
		/* Each cartridge should show its own owner's time of day, and both
		 * devices have to install the same pair for the two replicas to agree.
		 * Console A is the host's, console B the client's, so both sides can
		 * work out the same two numbers from the two identities. */
		{
			const uint64_t host_clock = NetLink_getRole() == NETLINK_ROLE_HOST
			                          ? dual_local_wall_clock : peer.wall_clock_utc;
			const uint64_t client_clock = NetLink_getRole() == NETLINK_ROLE_HOST
			                            ? peer.wall_clock_utc : dual_local_wall_clock;
			if (!core.dual_set_clock_epochs(host_clock, client_clock)) {
				dual_fail("Could not set the linked cartridge clocks.");
				return false;
			}
			shim_log("cartridge clocks set: A=%llu B=%llu (peer is %+lld s from us)\n",
			         (unsigned long long)host_clock,
			         (unsigned long long)client_clock,
			         (long long)((int64_t)peer.wall_clock_utc -
			                     (int64_t)dual_local_wall_clock));
		}
		dual_identity_checked = 1;
		shim_log("peer identity accepted; agreed input delay %d\n", input_delay);
	}

	if (!dual_verdict_sent) {
		if (!NetLink_sendLinkVerdict(dual_peer_rom_found != 0,
		                             dual_peer_rom_found ? NETLINK_LINK_OK
		                                                 : NETLINK_LINK_NO_PEER_ROM))
			return false;
		dual_verdict_sent = 1;
	}

	if (!dual_verdict_agreed) {
		bool peer_can_pair = false;
		uint32_t peer_reason = NETLINK_LINK_OK;
		if (!NetLink_takeLinkVerdict(&peer_can_pair, &peer_reason)) return false;
		if (!dual_peer_rom_found || !peer_can_pair) {
			request_serial_fallback(!dual_peer_rom_found ? NETLINK_LINK_NO_PEER_ROM
			                                             : peer_reason);
			return false;
		}
		dual_verdict_agreed = 1;
		shim_log("instanced link agreed by both devices\n");
	}

	/* Different cartridges need the pair rebuilt with one per console. The
	 * same-cartridge case arrives already loaded that way and must not be
	 * disturbed - a reload would discard the frontend's SRAM population for
	 * nothing - so this compares against what is loaded rather than doing it
	 * once. A peer that reconnects having relaunched a different game is the
	 * case that makes the difference. */
	{
		const char* want = dual_same_rom ? "" : dual_peer_rom_path;
		if (!dual_content_built || strcmp(dual_content_rom, want)) {
			if (!dual_rebuild_content(want)) {
				dual_fail("Could not load the linked cartridges together.");
				return false;
			}
			snprintf(dual_content_rom, sizeof(dual_content_rom), "%s", want);
			dual_content_built = 1;
		}
	}

	if (!dual_state_sent) {
		void* state = NULL;
		size_t len = 0;
		if (!capture_dual_memory(dual_local_console, &state, &len)) {
			dual_fail("Could not capture local Gambatte save memory.");
			return false;
		}
		bool sent = NetLink_sendState(NETLINK_STATE_CONSOLE_MEMORY, state, len);
		free(state);
		if (!sent) return false;
		dual_state_sent = 1;
		shim_log("sent console %c save memory to peer (%zu bytes)\n",
		         dual_local_console ? 'B' : 'A', len);
	}

	if (!dual_state_loaded) {
		void* peer_state = NULL;
		size_t peer_len = 0;
		if (!NetLink_takeState(NETLINK_STATE_CONSOLE_MEMORY, &peer_state, &peer_len))
			return false;
		bool loaded = apply_dual_memory(dual_peer_console, peer_state, peer_len);
		free(peer_state);
		if (!loaded) {
			dual_fail("Paired Gambatte could not adopt the peer save memory.");
			return false;
		}
		dual_state_loaded = 1;
		shim_log("console %c save memory adopted\n",
		         dual_peer_console ? 'B' : 'A');
	}

	/* The memory exchange makes both logical consoles equivalent. One paired
	 * checkpoint from the host then removes any residual initialization or RTC
	 * timing difference before the first synchronized input frame. */
	if (NetLink_getRole() == NETLINK_ROLE_HOST && !dual_checkpoint_sent) {
		if (!core.dual_is_checkpoint_safe()) return false;
		dual_state_size = core.dual_serialize_size();
		size_t len = dual_state_size;
		void* state = len ? malloc(len) : NULL;
		/* Zero first: the shared-screen capture does the same, and a buffer the
		 * core does not completely fill would otherwise put this device's heap
		 * into a payload both devices hash. */
		if (state) memset(state, 0, len);
		if (!state || !core.dual_serialize(state, len)) {
			free(state);
			dual_fail("Could not capture paired Gambatte checkpoint.");
			return false;
		}
		bool sent = NetLink_sendState(NETLINK_STATE_PAIRED_CHECKPOINT, state, len);
		/* Adopt our own checkpoint, so the last state-affecting operation is
		 * the identical unserialize on both devices rather than a capture here
		 * and a restore there. Costs one load at bootstrap and removes a whole
		 * class of asymmetry between the two replicas. */
		bool adopted = sent && core.dual_unserialize(state, len);
		free(state);
		if (!sent) return false;
		if (!adopted) {
			dual_fail("Host could not adopt its own paired checkpoint.");
			return false;
		}
		dual_checkpoint_sent = dual_checkpoint_loaded = 1;
		shim_log("sent authoritative paired checkpoint (%zu bytes) and adopted it\n", len);
	}
	if (NetLink_getRole() == NETLINK_ROLE_CLIENT && !dual_checkpoint_loaded) {
		void* state = NULL;
		size_t len = 0;
		if (!NetLink_takeState(NETLINK_STATE_PAIRED_CHECKPOINT, &state, &len))
			return false;
		/* Same point in the exchange as the host's, so both devices have called
		 * the mutating size query exactly once by the time play starts. */
		dual_state_size = core.dual_serialize_size();
		bool loaded = core.dual_is_checkpoint_safe() &&
		              len == dual_state_size &&
		              core.dual_unserialize(state, len);
		free(state);
		if (!loaded) {
			dual_fail("Could not adopt host paired Gambatte checkpoint.");
			return false;
		}
		dual_checkpoint_loaded = 1;
		shim_log("adopted authoritative paired checkpoint\n");
	}
	if (!dual_checkpoint_loaded) return false;

	if (!dual_ready_sent) {
		NetLink_resetTimeline();
		if (!NetLink_ackResync(DUAL_READY_EPOCH, true)) return false;
		dual_ready_sent = 1;
		shim_log("paired checkpoint ready; waiting at input barrier\n");
		return false;
	}

	uint32_t ready_epoch = 0;
	bool peer_loaded = false;
	if (!NetLink_takeResyncAck(&ready_epoch, &peer_loaded)) return false;
	if (ready_epoch != DUAL_READY_EPOCH || !peer_loaded) {
		dual_fail("Peer rejected the instanced-link bootstrap.");
		return false;
	}
	memset(local_inputs, 0, sizeof(local_inputs));
	dual_frame = 0;
	last_scheduled = 0;
	input_scheduled = 0;
	dual_bootstrapped = 1;
	dual_wait_since.tv_sec = dual_wait_since.tv_usec = 0;
	recovery_failed = 0;
	recovery_error[0] = '\0';
	shim_log("instanced paired core ready; cable is local and Wi-Fi is input-only\n");
	return true;
}

static uint32_t timeval_delta_us(const struct timeval* from, const struct timeval* to) {
	long sec = to->tv_sec - from->tv_sec;
	long usec = to->tv_usec - from->tv_usec;
	long long total = (long long)sec * 1000000LL + usec;
	if (total <= 0) return 0;
	return total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
}

/* One paired core call advances both consoles and the cable between them. */
static bool run_dual_frame(void) {
	struct timeval started, finished;
	gettimeofday(&started, NULL);
	NetLink_setCoreRunning(true);
	core.run();
	NetLink_setCoreRunning(false);
	gettimeofday(&finished, NULL);
	uint32_t elapsed = timeval_delta_us(&started, &finished);
	dual_pair_run_us += elapsed;
	if (elapsed > dual_pair_run_max_us) dual_pair_run_max_us = elapsed;
	return true;
}

static void dual_report_pacing(void) {
	if (!stat_since.tv_sec) { gettimeofday(&stat_since, NULL); return; }
	if (!dual_frame || dual_frame % PACING_INTERVAL) return;
	struct timeval now;
	gettimeofday(&now, NULL);
	long ms = (now.tv_sec - stat_since.tv_sec) * 1000L
	        + (now.tv_usec - stat_since.tv_usec) / 1000L;
	if (ms <= 0) return;
	unsigned fps10 = (unsigned)((stat_frames * 10000UL) / (unsigned long)ms);
	unsigned total = stat_frames + stat_stalls;
	unsigned stall_pct = total ? (unsigned)((stat_stalls * 100UL) / total) : 0;
	shim_log("instanced pacing: %u paired frames in %ldms (%u.%u fps), "
	         "input stalls %u (%u%%), longest %u, delay %d\n",
	         stat_frames, ms, fps10 / 10, fps10 % 10, stat_stalls,
	         stall_pct, stat_stall_max, input_delay);
	if (stat_frames) {
		shim_log("instanced core time: paired call avg %lluus max %uus "
		         "(both consoles and the local cable)\n",
		         (unsigned long long)(dual_pair_run_us / stat_frames), dual_pair_run_max_us);
	}
	stat_frames = stat_stalls = stat_stall_max = 0;
	dual_pair_run_us = 0;
	dual_pair_run_max_us = 0;
	stat_since = now;
}

/* Hash both consoles plus the link coordinator at fixed frames and compare with
 * the peer's hash of the same frame.
 *
 * Symmetric and identically timed on both devices: each hashes at the same
 * dual_frame and the comparison happens separately, whenever the peer's value
 * turns up, so a hash arriving a frame late is not itself a source of
 * divergence. Frame-keyed rings make ordering irrelevant.
 *
 * The paired checkpoint deliberately excludes presentation, diagnostics and
 * cached input, which is what lets two devices showing different consoles hash
 * the same bytes.
 *
 * Returns true if the session has been failed. */
static bool dual_check_agreement(void) {
	if (!core.dual_serialize_size || !core.dual_serialize) return false;

	if (dual_frame % HASH_INTERVAL == 0) {
		/* Not a skippable condition. A device that skipped a round would be
		 * comparing states reached by different routes from then on, and a
		 * failure there would say nothing about the emulation. The bus is idle
		 * at a frame boundary
		 * by construction - both consoles have finished and their service loops
		 * drained before core.run() returns - so this should not fire; if it
		 * does, the assumption is wrong and saying so beats poisoning the run. */
		if (!core.dual_is_checkpoint_safe()) {
			dual_hash_skips++;
			shim_log("paired state not capturable at frame %u; the serial bus was "
			         "not idle at a frame boundary\n", dual_frame);
			dual_recoverable_fail("Could not verify the linked consoles.");
			return true;
		}
		if (dual_state_size && dual_state_size != dual_hash_buf_len) {
			free(dual_hash_buf);
			dual_hash_buf = malloc(dual_state_size);
			dual_hash_buf_len = dual_hash_buf ? dual_state_size : 0;
		}
		if (dual_hash_buf) {
			memset(dual_hash_buf, 0, dual_hash_buf_len);
			if (core.dual_serialize(dual_hash_buf, dual_hash_buf_len)) {
				uint32_t mine = hash_bytes(dual_hash_buf, dual_hash_buf_len);
				unsigned slot = (dual_frame / HASH_INTERVAL) % HASH_HISTORY;
				own_hashes[slot] = (StateHash){ dual_frame, mine, 1 };
				NetLink_sendHash(dual_frame, mine);
				if (!dual_frame)
					shim_log("paired state at frame 0: %08x (%zu bytes)\n",
					         mine, dual_hash_buf_len);
			}
		}
	}

	uint32_t f, h;
	while (NetLink_takeHash(&f, &h)) {
		unsigned p = (f / HASH_INTERVAL) % HASH_HISTORY;
		pending_peer_hashes[p] = (StateHash){ f, h, 1 };
	}

	for (unsigned p = 0; p < HASH_HISTORY; p++) {
		if (!pending_peer_hashes[p].valid) continue;
		f = pending_peer_hashes[p].frame;
		h = pending_peer_hashes[p].hash;
		unsigned sl = (f / HASH_INTERVAL) % HASH_HISTORY;
		if (!own_hashes[sl].valid || own_hashes[sl].frame != f) continue;
		pending_peer_hashes[p].valid = 0;
		if (own_hashes[sl].hash == h) {
			shim_log("paired consoles agree at frame %u (%08x)\n", f, h);
			continue;
		}
		/* Nothing can be salvaged: an in-process cable has no resync, and both
		 * replicas have been feeding their players a different game since some
		 * frame before this one. Say so and offer the usual choices. */
		shim_log("PAIRED DESYNC at frame %u (ours %08x, peer %08x) after %u skipped checks\n",
		         f, own_hashes[sl].hash, h, dual_hash_skips);
		dual_recoverable_fail("Consoles have diverged. Link play stopped.");
		return true;
	}
	return false;
}

/* Returns true when this frontend frame was fully handled (including waits). */
static bool dual_link_tick(void) {
	NetLink_markFrame();
	/* Continue solo keeps the paired core - it is the only thing that can run
	 * this content - and simply stops taking the peer's console anywhere. The
	 * abandoned console holds neutral input; it is still emulated, so the link
	 * cable stays electrically sane and the local save keeps working. */
	if (solo_mode) {
		dual_primary_buttons = fe_input_state
			? (uint16_t)fe_input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK)
			: 0;
		dual_shadow_buttons = 0;
		run_dual_frame();
		dual_frame++;
		return true;
	}
	/* dual_failed is terminal - the two builds or the two cartridges cannot be
	 * paired at all. recovery_failed is the recoverable kind and shares the
	 * shared-screen overlay, so a dropped link offers Wait / Solo / Exit. */
	if (dual_failed) {
		if (fe_input_poll) fe_input_poll();
		present_paused_frame(dual_frame, dual_error);
		present_paused_audio();
		return true;
	}
	if (recovery_failed) {
		if (fe_input_poll) fe_input_poll();
		failure_input_tick();
		char failure_message[224];
		failure_overlay_message(failure_message, sizeof(failure_message));
		present_paused_frame(dual_frame, failure_message);
		present_paused_audio();
		return true;
	}
	if (NetLink_isPeerPaused()) {
		if (fe_input_poll) fe_input_poll();
		present_paused_frame(dual_frame, "Waiting for other player (menu open)...");
		present_paused_audio();
		return true;
	}
	if (!dual_bootstrap_tick()) {
		if (fe_input_poll) fe_input_poll();
		present_paused_frame(dual_frame,
		                     exit_requested ? "Switching to link cable..."
		                     : dual_failed ? dual_error
		                     : dual_verdict_agreed ? "Synchronizing linked consoles..."
		                     : "Starting instanced link...");
		present_paused_audio();
		return true;
	}

	uint32_t target = dual_frame + (uint32_t)input_delay;
	if (!input_scheduled || target > last_scheduled) {
		/* uint16_t first: the callback returns int16_t, and a set bit 15 (R3)
		 * would otherwise sign-extend across the reset flag below and reset a
		 * console on both replicas. */
		uint32_t local = fe_input_state
			? (uint16_t)fe_input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK)
			: 0;
		if (dual_reset_pending) local |= DUAL_INPUT_RESET;
		local_inputs[target % 256] = local;
		if (!NetLink_sendInput(target, local)) {
			/* The transport closed the connection. Do not consume the queued
			 * reset: the next generation re-bootstraps and can still carry it. */
			dual_recoverable_fail("Lost contact with the other player.");
			return true;
		}
		last_scheduled = target;
		input_scheduled = 1;
		dual_reset_pending = 0;
	}
	uint32_t mine = dual_frame < (uint32_t)input_delay ? 0 : local_inputs[dual_frame % 256];
	uint32_t theirs = 0;
	if (dual_frame >= (uint32_t)input_delay &&
	    !NetLink_getRemoteInput(dual_frame, &theirs)) {
		if (fe_input_poll) fe_input_poll();
		if (!input_stall_since.tv_sec) gettimeofday(&input_stall_since, NULL);
		if (elapsed_ms(&input_stall_since) > peer_wait_timeout_ms()) {
			dual_recoverable_fail(NetLink_isConnected()
			                      ? "Peer stopped sending input."
			                      : "Peer disconnected.");
			return true;
		}
		stat_stalls++;
		stall_run++;
		if (stall_run > stat_stall_max) stat_stall_max = stall_run;
		present_paused_frame(stall_run,
		                     stall_run > STALL_OVERLAY_FRAMES ? "Waiting for other player's input..." : NULL);
		present_paused_audio();
		return true;
	}
	input_stall_since.tv_sec = input_stall_since.tv_usec = 0;
	stall_run = 0;
	if (mine & DUAL_INPUT_RESET) {
		if (!core.dual_reset_console(dual_local_console)) {
			dual_fail("Could not reset the local logical console.");
			return true;
		}
		shim_log("applied synchronized reset to console %c at frame %u\n",
		         dual_local_console ? 'B' : 'A', dual_frame);
	}
	if (theirs & DUAL_INPUT_RESET) {
		if (!core.dual_reset_console(dual_peer_console)) {
			dual_fail("Could not mirror the peer logical-console reset.");
			return true;
		}
		shim_log("applied synchronized peer reset to console %c at frame %u\n",
		         dual_peer_console ? 'B' : 'A', dual_frame);
	}
	dual_primary_buttons = mine & ~DUAL_INPUT_RESET;
	dual_shadow_buttons = theirs & ~DUAL_INPUT_RESET;
	/* Before the advance, so dual_frame names the state being hashed and the
	 * first check covers frame 0 - which is exactly the assertion that the
	 * bootstrap checkpoint left both devices holding the same two consoles. */
	if (dual_check_agreement()) return true;
	if (!run_dual_frame()) return true;
	dual_frame++;
	stat_frames++;
	dual_report_pacing();
	return true;
}

void retro_run(void) {
	if (!core.handle) return;

	/* From the frame loop, so it lands after minarch has applied its own CPU
	 * Speed option, and re-asserted occasionally so a menu round trip cannot
	 * quietly hand the device back to the conservative governor mid-session. */
	if (session_active) {
		static unsigned cpu_pin_countdown;
		if (!cpu_pin_countdown--) {
			cpu_pin_performance();
			cpu_pin_countdown = PACING_INTERVAL;
		}
	}

	// Netpacket state and inbound packets are settled before the core runs, so
	// a frame sees everything that arrived since the last one.
	pump_netpacket();
	if (session_active && dual_link_mode) {
		dual_link_tick();
		return;
	}

	if (session_active && netplay_mode && !solo_mode) {
		NetLink_markFrame();
		if (NetLink_isConnected()) {
			peer_missing_since.tv_sec = peer_missing_since.tv_usec = 0;
		} else {
			if (!peer_missing_since.tv_sec) gettimeofday(&peer_missing_since, NULL);
			if (!recovery_failed && elapsed_ms(&peer_missing_since) > peer_wait_timeout_ms())
				recovery_fail("Peer unavailable.");
		}
		netplay_handshake();

		if (netplay_recovery_tick()) {
			if (fe_input_poll) fe_input_poll();
			if (recovery_failed) failure_input_tick();
			static unsigned recovery_wait;
			char failure_message[224];
			if (recovery_failed) failure_overlay_message(failure_message, sizeof(failure_message));
			present_paused_frame(recovery_wait++, recovery_failed ? failure_message
			                     : recovery_status_message());
			present_paused_audio();
			return;
		}

		if (!netplay_synced) {
			// Nothing to show yet and nothing to run; keep the frontend alive.
			if (fe_input_poll) fe_input_poll();
			static unsigned waiting;
			present_paused_frame(waiting++, "Starting session...");
			present_paused_audio();
			return;
		}

		// Sample and send each frame's input exactly once. A stall returns
		// without advancing netplay_frame, so re-sampling here would overwrite
		// an input the peer may already have committed to - the two sides would
		// then run the same frame from different inputs.
		uint32_t target = netplay_frame + (uint32_t)input_delay;
		if (!input_scheduled || target > last_scheduled) {
			uint32_t local = fe_input_state
				? (uint32_t)fe_input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK)
				: 0;
			local_inputs[target % 256] = local;
			NetLink_sendInput(target, local);
			last_scheduled = target;
			input_scheduled = 1;
		}

		// Our own input for the current frame was decided INPUT_DELAY frames
		// ago; the first few frames are primed neutral.
		uint32_t mine = (netplay_frame - timeline_start_frame < (uint32_t)input_delay)
		              ? 0 : local_inputs[netplay_frame % 256];

		uint32_t theirs = 0;
		if (netplay_frame - timeline_start_frame >= (uint32_t)input_delay &&
		    !NetLink_getRemoteInput(netplay_frame, &theirs)) {
			// Peer input for this frame has not arrived. Stall rather than run
			// ahead: advancing without it would desync immediately.
			if (fe_input_poll) fe_input_poll();
			if (!input_stall_since.tv_sec) gettimeofday(&input_stall_since, NULL);
			if (elapsed_ms(&input_stall_since) > peer_wait_timeout_ms()) {
				recovery_fail(NetLink_isConnected()
				              ? "Peer stopped sending input."
				              : "Peer disconnected.");
				failure_input_tick();
				char failure_message[224];
				failure_overlay_message(failure_message, sizeof(failure_message));
				present_paused_frame(stall_run, failure_message);
				present_paused_audio();
				return;
			}

			stat_stalls++;
			stall_run++;
			if (stall_run > stat_stall_max) stat_stall_max = stall_run;

			// Always present something - see present_paused_frame. Only say
			// anything once the wait is long enough to be worth explaining: a
			// held frame reads as a hitch, while a message that appears and
			// vanishes every few frames reads as a fault.
			present_paused_frame(stall_run,
			                     (stall_run > STALL_OVERLAY_FRAMES || !have_last_frame())
			                         ? "Waiting for other player..." : NULL);
			present_paused_audio();
			return;
		}
		input_stall_since.tv_sec = input_stall_since.tv_usec = 0;
		stall_run = 0;

		int host = (NetLink_getRole() == NETLINK_ROLE_HOST);
		frame_buttons[0] = host ? mine : theirs;
		frame_buttons[1] = host ? theirs : mine;

		if (netplay_checkDivergence()) {
			if (fe_input_poll) fe_input_poll();
			present_paused_frame(0, "Desync detected - resynchronizing...");
			present_paused_audio();
			return;
		}
		netplay_frame++;
		stat_frames++;
		netplay_reportPacing();
	}
	else if (session_active && !netplay_mode) {
		NetLink_markFrame();
		link_frames++;
		link_report_pacing();

		if (recovery_failed) {
			if (fe_input_poll) fe_input_poll();
			failure_input_tick();
			char failure_message[224];
			failure_overlay_message(failure_message, sizeof(failure_message));
			present_paused_frame(link_frames, failure_message);
			present_paused_audio();
			return;
		}

		// Peer's frontend is blocked - a menu, a sleep. Running ahead would only
		// fill a queue it is not draining, so skip the frame. This is a legal
		// dupe frame; the frontend loop keeps polling input and stays responsive.
		if (NetLink_isPeerPaused()) {
			link_paused_frames++;
			// Input is polled only from inside core.run(). Skip that without
			// doing this and PAD_poll never runs, so no button - including
			// MENU - is ever seen and the frontend is frozen, not merely
			// paused. minarch does the same at each of its own frame skips.
			if (fe_input_poll) fe_input_poll();

			static unsigned paused_frames;
			present_paused_frame(paused_frames++, "Waiting for other player (menu open)...");
			present_paused_audio();
			return;
		}
	}

	// TODO(netplay): gate frame advance on the peer for input-lockstep netplay.
	// Returning without running the core is a legal frame skip; link play does
	// not need it - the cores keep their own timing.
	//
	// Bracketed so a core that blocks in here waiting on its peer is not
	// mistaken for a frontend that has gone away.
	struct timeval link_run_started, link_run_finished;
	gettimeofday(&link_run_started, NULL);
	NetLink_setCoreRunning(true);
	core.run();
	NetLink_setCoreRunning(false);
	gettimeofday(&link_run_finished, NULL);
	uint32_t link_run_us = timeval_delta_us(&link_run_started, &link_run_finished);
	if (NetLink_isConnected() &&
	    link_run_us > (uint32_t)link_starve_timeout_ms() * 1000u) {
		shim_log("link starved: one core call blocked %ums while the peer was connected\n",
		         link_run_us / 1000u);
		recovery_fail("Other player's game stopped sending link data.");
	}
}

size_t retro_serialize_size(void) {
	if (!core.handle) return 0;
	/* Save states are deliberately unavailable for every armed session. Returning
	 * zero is the libretro capability signal minarch uses to omit its save/load
	 * actions. Protocol synchronization calls core.serialize_* directly and is
	 * unaffected; battery-backed SRAM remains normal. */
	if (session_active) return 0;
	return core.serialize_size();
}

bool retro_serialize(void* data, size_t size) {
	if (!core.handle) return false;
	if (session_active) return false;
	return core.serialize(data, size);
}

bool retro_unserialize(const void* data, size_t size) {
	if (!core.handle) return false;
	if (session_active) {
		shim_log("frontend save-state load blocked during session\n");
		return false;
	}

	return core.unserialize(data, size);
}

void retro_cheat_reset(void) {
	if (!core.handle) return;
	core.cheat_reset();
}

void retro_cheat_set(unsigned index, bool enabled, const char* code) {
	if (!core.handle) return;
	core.cheat_set(index, enabled, code);
}

static bool hash_content_path(Sha256* hash, const char* path, unsigned depth) {
	if (depth > 4) return false;
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	uint8_t buf[32768];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) != 0) sha256_update(hash, buf, n);
	bool ok = !ferror(f);
	fclose(f);
	if (!ok) return false;

	const char* ext = strrchr(path, '.');
	bool cue = ext && !strcasecmp(ext, ".cue");
	bool m3u = ext && (!strcasecmp(ext, ".m3u") || !strcasecmp(ext, ".m3u8"));
	if (!cue && !m3u) return true;
	f = fopen(path, "r");
	if (!f) return false;
	char dir[1024], line[2048];
	snprintf(dir, sizeof(dir), "%s", path);
	char* slash = strrchr(dir, '/');
	if (slash) *slash = '\0'; else snprintf(dir, sizeof(dir), ".");
	while (ok && fgets(line, sizeof(line), f)) {
		char* ref = line;
		char* end = strpbrk(ref, "\r\n"); if (end) *end = '\0';
		if (cue) {
			while (*ref == ' ' || *ref == '\t') ref++;
			if (strncasecmp(ref, "FILE", 4) || (ref[4] != ' ' && ref[4] != '\t')) continue;
			ref += 4; while (*ref == ' ' || *ref == '\t') ref++;
			if (*ref == '"') { ref++; end = strchr(ref, '"'); }
			else { end = strpbrk(ref, " \t"); }
			if (end) *end = '\0';
		} else {
			while (*ref == ' ' || *ref == '\t') ref++;
			if (!*ref || *ref == '#') continue;
			end = ref + strlen(ref);
			while (end > ref && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
		}
		if (!*ref) continue;
		char child[2048];
		if (ref[0] == '/') snprintf(child, sizeof(child), "%s", ref);
		else snprintf(child, sizeof(child), "%s/%s", dir, ref);
		static const uint8_t separator[] = { 0, 'R', 'E', 'F', 0 };
		sha256_update(hash, separator, sizeof(separator));
		ok = hash_content_path(hash, child, depth + 1);
	}
	fclose(f);
	return ok;
}

static bool hash_game_content(const struct retro_game_info* game, uint8_t out[32]) {
	Sha256 hash;
	sha256_init(&hash);
	if (game && game->data && game->size) {
		sha256_update(&hash, game->data, game->size);
	} else if (game && game->path) {
		if (!hash_content_path(&hash, game->path, 0)) return false;
	} else return false;
	sha256_final(&hash, out);
	return true;
}

bool retro_load_game(const struct retro_game_info* game) {
	ensure_loaded();
	if (session_active && (netplay_mode || dual_link_mode)) {
		rom_hash_ready = hash_game_content(game, rom_sha256);
		if (!rom_hash_ready) {
			if (dual_link_mode) {
				dual_failed = 1;
				snprintf(dual_error, sizeof(dual_error), "Cannot hash ROM for instanced link.");
			} else recovery_fail("Cannot hash ROM content. Shared-screen launch refused.");
		} else {
			shim_log("ROM SHA-256: %02x%02x%02x%02x...%02x%02x%02x%02x\n",
			         rom_sha256[0], rom_sha256[1], rom_sha256[2], rom_sha256[3],
			         rom_sha256[28], rom_sha256[29], rom_sha256[30], rom_sha256[31]);
		}
	} else rom_hash_ready = 0;

	/* Remember our own cartridge. If the peer turns out to be running a
	 * different one that this device also owns, the pair is reloaded as two
	 * contents and both halves have to be supplied again - the frontend's
	 * buffer is not ours to keep. Failing to copy it is not fatal here; it
	 * only costs the different-cartridge case, which then demotes to serial. */
	if (dual_link_mode) {
		snprintf(dual_local_rom_path, sizeof(dual_local_rom_path), "%s",
		         game && game->path ? game->path : "");
		free(dual_own_rom);
		dual_own_rom = NULL;
		dual_own_rom_len = 0;
		if (game && game->data && game->size) {
			dual_own_rom = malloc(game->size);
			if (dual_own_rom) {
				memcpy(dual_own_rom, game->data, game->size);
				dual_own_rom_len = game->size;
			}
		} else if (dual_local_rom_path[0]) {
			read_whole_file(dual_local_rom_path, &dual_own_rom, &dual_own_rom_len);
		}
		dual_local_rom_size = (uint32_t)dual_own_rom_len;
		dual_local_rom_crc32 = dual_own_rom_len
			? (uint32_t)crc32(0L, (const Bytef*)dual_own_rom, (uInt)dual_own_rom_len)
			: 0;
		/* What the frontend just loaded: our cartridge in both consoles. */
		dual_content_rom[0] = '\0';
		dual_content_built = 1;
		if (!dual_own_rom_len)
			shim_log("could not retain the local cartridge; linked cartridges "
			         "will fall back to network serial\n");
	}
	return core.load_game(game);
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info) {
	ensure_loaded();
	if (!core.load_game_special) return false;
	if (session_active && netplay_mode) {
		recovery_fail("Cannot hash multi-content game. Shared-screen launch refused.");
	}
	return core.load_game_special(game_type, info, num_info);
}

void retro_unload_game(void) {
	if (!core.handle) return;
	core.unload_game();
}

unsigned retro_get_region(void) {
	if (!core.handle) return RETRO_REGION_NTSC;
	return core.get_region();
}

void* retro_get_memory_data(unsigned id) {
	if (!core.handle) return NULL;
	/* Minarch uses these entry points for both its startup read and its
	 * menu/sleep/exit writes. On a shared-screen guest, hide persistent memory
	 * from the frontend in both directions. Protocol code above deliberately
	 * calls core.get_memory_* directly, so the guest core still receives and
	 * uses the host's authoritative in-memory copy. */
	if (session_active && netplay_mode &&
	    NetLink_getRole() == NETLINK_ROLE_CLIENT &&
	    (id == RETRO_MEMORY_SAVE_RAM || id == RETRO_MEMORY_RTC))
		return NULL;
	return core.get_memory_data(id);
}

size_t retro_get_memory_size(unsigned id) {
	if (!core.handle) return 0;
	if (session_active && netplay_mode &&
	    NetLink_getRole() == NETLINK_ROLE_CLIENT &&
	    (id == RETRO_MEMORY_SAVE_RAM || id == RETRO_MEMORY_RTC))
		return 0;
	return core.get_memory_size(id);
}
