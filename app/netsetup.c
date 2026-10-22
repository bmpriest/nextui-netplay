#include "netsetup.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <dirent.h>
#include <gnu/libc-version.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DISCOVERY_PORT  55438
#define DISCOVERY_MAGIC 0x4E504C4BU /* 'NPLK' - same family as the link */
#define ANNOUNCE_MS     500

typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint32_t version;
	uint32_t mode;
	uint32_t hotspot;
	char     platform[16];
	char     ssid[NS_SSID_LEN];
	char     psk[NS_PSK_LEN];
} NS_Announce;

static char ns_platform[16] = "tg5040";
static char ns_pak[256];
static char ns_state[256];
static char ns_sd[128] = "/mnt/SDCARD";

static int announce_fd = -1;
static int discover_fd = -1;
static struct timeval last_announce;
static NS_Mode announce_mode = NS_MODE_NETPLAY;
static char announce_ssid[NS_SSID_LEN];
static char announce_psk[NS_PSK_LEN];
static bool hotspot_running;   /* we are serving the ad hoc network */
static bool joined_hotspot;    /* we moved our client stack onto someone's */
static char ns_ap_if[64];      /* interface hostapd was given, for teardown */
static char joined_ssid[NS_SSID_LEN];  /* recorded into the session so the */
static char joined_psk[NS_PSK_LEN];    /* launch stub can rejoin per game */
static NS_ProgressFn ns_progress;      /* lets the UI animate a blocking join */
static bool file_exists(const char* p);

/* Everything that differs per platform lives in launcher/wifi-platform.sh, and
 * this is how the app reaches it.
 *
 * A second copy of that table here would be the obvious alternative and is the
 * wrong one: the launcher scripts need identical answers on every launch, and
 * two tables in two languages drift silently - which is the class of bug this
 * whole change exists to remove. What stays inline below is only what does not
 * vary: `ip addr/link`, `killall wpa_supplicant`, and the AP-side `iw` calls,
 * which are meaningful exactly on the platforms that can host at all.
 *
 * Cost is one short-lived shell per call, against `system()` calls this code
 * already makes everywhere; the hottest caller polls once per second. */
static int wifi_plat(const char* fmt, ...) {
	char args[320];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(args, sizeof(args), fmt, ap);
	va_end(ap);

	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
	         "SDCARD_PATH='%s' PLATFORM='%s' SYSTEM_PATH='%s/.system/%s' "
	         "sh '%s/launcher/wifi-platform.sh' %s",
	         ns_sd, ns_platform, ns_sd, ns_platform, ns_pak, args);
	return system(cmd);
}

/* First line of the helper's output, trimmed. Empty string when it says
 * nothing, which every caller treats as "no answer" rather than an error. */
static void wifi_plat_out(char* out, int len, const char* fmt, ...) {
	out[0] = '\0';

	char args[320];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(args, sizeof(args), fmt, ap);
	va_end(ap);

	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
	         "SDCARD_PATH='%s' PLATFORM='%s' SYSTEM_PATH='%s/.system/%s' "
	         "sh '%s/launcher/wifi-platform.sh' %s 2>/dev/null",
	         ns_sd, ns_platform, ns_sd, ns_platform, ns_pak, args);
	FILE* f = popen(cmd, "r");
	if (!f) return;
	if (fgets(out, len, f)) {
		char* nl = strpbrk(out, "\r\n");
		if (nl) *nl = '\0';
	}
	pclose(f);
}

/* Associated to this SSID, or to anything when ssid is NULL. */
static bool wifi_associated(const char* ssid) {
	if (ssid && ssid[0]) return wifi_plat("assoc '%s'", ssid) == 0;
	return wifi_plat("assoc") == 0;
}

//////////////////////////////////////////////////////////////////////////////
// environment
//////////////////////////////////////////////////////////////////////////////

void NS_init(void) {
	const char* p = getenv("PLATFORM");
	if (p && p[0]) snprintf(ns_platform, sizeof(ns_platform), "%s", p);

	const char* sd = getenv("SDCARD_PATH");
	if (sd && sd[0]) snprintf(ns_sd, sizeof(ns_sd), "%s", sd);

	snprintf(ns_pak, sizeof(ns_pak), "%s/Tools/%s/Netplay.pak", ns_sd, ns_platform);
	snprintf(ns_state, sizeof(ns_state), "%s/.userdata/shared/Netplay", ns_sd);

	/* Updates replace the pak directory, so no user preference or recovery
	 * record may live underneath it. Migrate files from the historical location
	 * without overwriting anything already present in the shared directory. */
	char shared[256];
	snprintf(shared, sizeof(shared), "%s/.userdata/shared", ns_sd);
	mkdir(shared, 0777);
	mkdir(ns_state, 0777);
	char legacy[320];
	snprintf(legacy, sizeof(legacy), "%s/state", ns_pak);
	DIR* dir = opendir(legacy);
	if (dir) {
		struct dirent* entry;
		while ((entry = readdir(dir))) {
			if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
			char old_path[512], new_path[512];
			snprintf(old_path, sizeof(old_path), "%s/%s", legacy, entry->d_name);
			if (!strcmp(entry->d_name, "harness-backup")) {
				snprintf(new_path, sizeof(new_path), "%s/netplay-harness", ns_state);
				mkdir(new_path, 0777);
				snprintf(new_path, sizeof(new_path), "%s/netplay-harness/session-backups", ns_state);
			} else {
				snprintf(new_path, sizeof(new_path), "%s/%s", ns_state, entry->d_name);
			}
			if (!file_exists(new_path)) rename(old_path, new_path);
		}
		closedir(dir);
	}
	char old_conf[320], new_conf[320];
	snprintf(old_conf, sizeof(old_conf), "%s/session.conf", ns_pak);
	snprintf(new_conf, sizeof(new_conf), "%s/session.conf", ns_state);
	if (!file_exists(new_conf) && file_exists(old_conf)) rename(old_conf, new_conf);
}

const char* NS_platform(void) { return ns_platform; }
const char* NS_pakPath(void)  { return ns_pak; }
const char* NS_statePath(void) { return ns_state; }

/* Read the first non-loopback IPv4 from `ip addr`. Cheaper and more portable
 * across these firmwares than getifaddrs, which busybox images vary on. */
bool NS_localIP(char* out, int len) {
	FILE* f = popen("ip -4 addr show 2>/dev/null | grep -oE 'inet [0-9.]+' | grep -v '127\\.' | head -1 | cut -d' ' -f2", "r");
	if (!f) return false;
	bool ok = false;
	if (fgets(out, len, f)) {
		char* nl = strpbrk(out, "\r\n");
		if (nl) *nl = '\0';
		ok = out[0] != '\0';
	}
	pclose(f);
	return ok;
}

static bool file_exists(const char* p) {
	struct stat st;
	return stat(p, &st) == 0;
}

//////////////////////////////////////////////////////////////////////////////
// wifi power save
//
// The radio parks itself between beacons, which costs tens of milliseconds on
// a link that is otherwise idle - exactly the shape of traffic netplay makes,
// a few bytes per frame. On the Brick this is on by default and shows up as
// input jitter. Off while a session is armed, restored to whatever it was on
// disarm, because leaving it off costs battery.
//////////////////////////////////////////////////////////////////////////////

/* Goes to stderr, which launch.sh redirects into the pak's log. The ad hoc path
 * used to report only through on-screen status, so a failure left nothing to
 * read afterwards and every diagnosis started from scratch. */
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
static void ns_stamp(char* out, int len) {
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

static void ns_log(const char* fmt, ...) {
	char ts[48];
	ns_stamp(ts, sizeof(ts));

	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[%s] [netplay-app] ", ts);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fflush(stderr);
}

/* popen with a deadline.
 *
 * `iw dev wlan0 scan` can block indefinitely on these radios - it wedged the
 * app on the Join screen with no buttons working, because the read happens on
 * the thread that draws the UI. Nothing shelled out to from a screen may be
 * unbounded. Returns the number of lines read; kills the child on timeout.
 *
 * timeout(1) is not on these images, so the bound is enforced here: the pipe is
 * non-blocking and we give up on the clock. */
/* Defined with the join code below; the scan helpers above need it. */
static bool platform_ctrl_dir(char* out, int len);

static int popen_bounded(const char* cmd, int timeout_ms,
                         void (*on_line)(const char*, void*), void* ctx) {
	FILE* f = popen(cmd, "r");
	if (!f) return -1;

	int fd = fileno(f);
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	struct timeval start;
	gettimeofday(&start, NULL);

	char line[512];
	size_t used = 0;
	int lines = 0;

	for (;;) {
		struct timeval now;
		gettimeofday(&now, NULL);
		long ms = (now.tv_sec - start.tv_sec) * 1000L
		        + (now.tv_usec - start.tv_usec) / 1000L;
		if (ms > timeout_ms) {
			ns_log("command timed out after %dms: %.60s\n", timeout_ms, cmd);
			break;
		}

		char c;
		ssize_t n = read(fd, &c, 1);
		if (n == 1) {
			if (c == '\n' || used + 1 >= sizeof(line)) {
				line[used] = '\0';
				if (used && on_line) on_line(line, ctx);
				if (used) lines++;
				used = 0;
			} else {
				line[used++] = c;
			}
			continue;
		}
		if (n == 0) break;                               /* clean EOF */
		if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(20 * 1000); continue; }
		break;
	}

	pclose(f);
	return lines;
}

static void ps_state_path(char* out, int len) {
	snprintf(out, len, "%s/wifi_powersave", ns_state);
}

/* "on" / "off". Empty where the platform has no power-save control at all
 * (H700 has no `iw`), in which case there is nothing to record or restore. */
static bool wifi_powerSaveGet(char* out, int len) {
	wifi_plat_out(out, len, "powersave-get");
	return out[0] != '\0';
}

void NS_wifiPowerSaveDisable(void) {
	char prior[16] = "";
	char path[512];
	/* Off by choice: leave the radio as the platform configured it. pre-launch.sh
	 * reads the same key from the settings file for the per-game re-apply. */
	if (!NS_settings()->disable_powersave) return;
	ps_state_path(path, sizeof(path));

	/* Record the prior value once. Re-arming without disarming first must not
	 * overwrite it with the "off" we ourselves set. */
	if (!file_exists(path) && wifi_powerSaveGet(prior, sizeof(prior))) {
		FILE* f = fopen(path, "w");
		if (f) { fprintf(f, "%s\n", prior); fclose(f); }
	}
	wifi_plat("powersave off");
}

bool NS_wifiPowerSaveNeedsReload(void) {
	return wifi_plat("powersave-needs-reload") == 0;
}

bool NS_wifiPowerSaveReload(char* err, int errlen) {
	ns_log("reloading the wifi driver with power save off\n");
	int rc = wifi_plat("powersave-reload off");
	if (rc == 0) return true;
	/* 3 is "driver back, no address yet": the supplicant usually finishes on
	 * its own, so power save is off even though WiFi is not quite back. */
	bool reloaded = WIFEXITED(rc) && WEXITSTATUS(rc) == 3;
	snprintf(err, errlen, reloaded
	         ? "Driver reloaded - WiFi is still reconnecting."
	         : "Could not reload the WiFi driver - reboot to recover.");
	ns_log("wifi driver reload exited %d\n", WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
	return reloaded && !NS_wifiPowerSaveNeedsReload();
}

void NS_wifiPowerSaveRestore(void) {
	char path[512];
	ps_state_path(path, sizeof(path));

	FILE* f = fopen(path, "r");
	if (f) {
		char prior[16] = "";
		if (fgets(prior, sizeof(prior), f)) {
			char* nl = strpbrk(prior, "\r\n");
			if (nl) *nl = '\0';
			if (!strcmp(prior, "on")) wifi_plat("powersave on");
		}
		fclose(f);
		remove(path);
	}
}

//////////////////////////////////////////////////////////////////////////////
// settings
//////////////////////////////////////////////////////////////////////////////

const char* const NS_INST_CORE[NS_INST_CORES] = { "gambatte", "mgba" };

/* Executable sharing is frozen and therefore defaults off. Compatibility cores
 * are local, pinned artifacts and are the safe default fallback. */
static NS_Settings ns_set = {
	.share_cores   = false,
	.compatibility_cores = true,
	.force_compatibility = false,
	.verbose_logs = true,
	.simple_client = false,
	.add_gameswitcher = false,
	.disable_powersave = false,
	.status_icon   = false,
	.quick_menu    = false,
	.instanced     = NS_INST_OFF,
	.inst_core     = { false, false },
};
static bool ns_set_loaded = false;

static void settings_path(char* out, int len) {
	snprintf(out, len, "%s/settings", ns_state);
}

/* The status icon watcher follows the setting and the session file on its own
 * once running; this starts it, or stops it and hides the icon, right away. */
void NS_statusIconSync(void) {
	char cmd[1200];
	snprintf(cmd, sizeof(cmd),
	         "SDCARD_PATH='%s' PLATFORM='%s' NETPLAY_PAK='%s' "
	         "sh '%s/launcher/status-icon.sh' sync >/dev/null 2>&1",
	         ns_sd, ns_platform, ns_pak, ns_pak);
	system(cmd);
}

/* The quick menu runs for as long as the setting is on, armed or not. */
void NS_quickMenuSync(void) {
	char cmd[1200];
	snprintf(cmd, sizeof(cmd),
	         "SDCARD_PATH='%s' PLATFORM='%s' NETPLAY_PAK='%s' "
	         "sh '%s/launcher/quick-menu.sh' sync >/dev/null 2>&1",
	         ns_sd, ns_platform, ns_pak, ns_pak);
	system(cmd);
}

bool NS_varnishInstalled(void) {
	char path[512];
	snprintf(path, sizeof(path), "%s/Tools/%s/Varnish.pak/varnish", ns_sd, ns_platform);
	return access(path, X_OK) == 0;
}

static const char* compatibility_arch(void) {
	return !strcmp(ns_platform, "my282") ? "armv7" : "aarch64";
}

NS_Settings* NS_settings(void) {
	if (!ns_set_loaded) NS_settingsLoad();
	return &ns_set;
}

static bool instanced_core_enabled(const char* core) {
	NS_Settings* settings = NS_settings();
	if (settings->instanced == NS_INST_OFF) return false;
	if (settings->instanced == NS_INST_ALL) return true;
	for (int i = 0; i < NS_INST_CORES; i++)
		if (!strcmp(core, NS_INST_CORE[i])) return settings->inst_core[i];
	return false;
}

void NS_settingsLoad(void) {
	ns_set_loaded = true;              /* set first: a missing file means defaults */

	char path[512];
	settings_path(path, sizeof(path));
	FILE* f = fopen(path, "r");
	if (!f) return;

	char line[160];
	while (fgets(line, sizeof(line), f)) {
		char* nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';
		char* eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		const char* k = line;
		int v = atoi(eq + 1);

		if      (!strcmp(k, "share_cores"))   ns_set.share_cores = v != 0;
		else if (!strcmp(k, "compatibility_cores")) ns_set.compatibility_cores = v != 0;
		else if (!strcmp(k, "force_compatibility")) ns_set.force_compatibility = v != 0;
		else if (!strcmp(k, "verbose_logs")) ns_set.verbose_logs = v != 0;
		else if (!strcmp(k, "simple_client")) ns_set.simple_client = v != 0;
		else if (!strcmp(k, "add_gameswitcher")) ns_set.add_gameswitcher = v != 0;
		else if (!strcmp(k, "disable_powersave")) ns_set.disable_powersave = v != 0;
		else if (!strcmp(k, "status_icon")) ns_set.status_icon = v != 0;
		else if (!strcmp(k, "quick_menu")) ns_set.quick_menu = v != 0;
		else if (!strcmp(k, "instanced"))
			ns_set.instanced = (v < 0 || v > NS_INST_SELECTED) ? NS_INST_OFF : (NS_InstMode)v;
		else {
			for (int i = 0; i < NS_INST_CORES; i++) {
				char key[64];
				snprintf(key, sizeof(key), "inst_%s", NS_INST_CORE[i]);
				if (!strcmp(k, key)) ns_set.inst_core[i] = v != 0;
			}
		}
	}
	fclose(f);
	/* The testing override is a stronger form of using packaged cores. Keep old
	 * or hand-edited settings internally coherent rather than showing an
	 * impossible Force=Yes / Use=No combination. */
	if (ns_set.force_compatibility) ns_set.compatibility_cores = true;
}

void NS_settingsSave(void) {
	char path[512], tmp[540];
	settings_path(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	/* Write-then-rename: a half-written settings file read at next launch would
	 * silently reset to defaults, and losing "simple client" mid-session is the
	 * kind of thing nobody would connect back to a power cut. */
	FILE* f = fopen(tmp, "w");
	if (!f) { ns_log("cannot write settings\n"); return; }
	fprintf(f, "share_cores=%d\n",   ns_set.share_cores ? 1 : 0);
	fprintf(f, "compatibility_cores=%d\n", ns_set.compatibility_cores ? 1 : 0);
	fprintf(f, "force_compatibility=%d\n", ns_set.force_compatibility ? 1 : 0);
	fprintf(f, "verbose_logs=%d\n", ns_set.verbose_logs ? 1 : 0);
	fprintf(f, "simple_client=%d\n", ns_set.simple_client ? 1 : 0);
	fprintf(f, "add_gameswitcher=%d\n", ns_set.add_gameswitcher ? 1 : 0);
	fprintf(f, "disable_powersave=%d\n", ns_set.disable_powersave ? 1 : 0);
	fprintf(f, "status_icon=%d\n", ns_set.status_icon ? 1 : 0);
	fprintf(f, "quick_menu=%d\n", ns_set.quick_menu ? 1 : 0);
	fprintf(f, "instanced=%d\n",     (int)ns_set.instanced);
	for (int i = 0; i < NS_INST_CORES; i++)
		fprintf(f, "inst_%s=%d\n", NS_INST_CORE[i], ns_set.inst_core[i] ? 1 : 0);
	fclose(f);
	if (rename(tmp, path) != 0) remove(tmp);

	/* The redirect and recents view are derived state. Keep them synchronized
	 * immediately so toggling the option never requires a reboot or re-arm. */
	char cmd[1200];
	snprintf(cmd, sizeof(cmd),
	         "SDCARD_PATH='%s' PLATFORM='%s' NETPLAY_PAK='%s' "
	         "sh '%s/launcher/gameswitcher.sh' %s >/dev/null 2>&1",
	         ns_sd, ns_platform, ns_pak, ns_pak,
	         ns_set.add_gameswitcher ? (NS_isArmed() ? "active" : "enable") : "disable");
	system(cmd);
	NS_statusIconSync();
	NS_quickMenuSync();
}

/* Three places a core can legitimately live: the platform's own directory, the
 * cores this pak ships to replace them, and an EXTRAS pak that bundles its own
 * (which is where mgba is - MGBA.pak, not the system cores dir). */
bool NS_coreInstalled(const char* core) {
	char p[640];
	/* mGBA is intentionally installable from this pak, so its picker remains
	 * available before an SD-card MGBA.pak exists. This path is platform-specific
	 * and therefore also prevents unsupported devices enabling it. */
	if (!strcmp(core, "mgba")) {
		snprintf(p, sizeof(p), "%s/cores/mgba/%s/MGBA.pak/mgba_libretro.so",
		         ns_pak, ns_platform);
		return file_exists(p);
	}

	snprintf(p, sizeof(p), "%s/.system/%s/cores/%s_libretro.so", ns_sd, ns_platform, core);
	if (file_exists(p)) return true;

	snprintf(p, sizeof(p), "%s/cores/compatibility/%s/%s_libretro.so",
	         ns_pak, compatibility_arch(), core);
	if (file_exists(p)) return true;

	char cmd[768];
	snprintf(cmd, sizeof(cmd),
	         "ls '%s/Emus/%s'/*.pak/%s_libretro.so >/dev/null 2>&1", ns_sd, ns_platform, core);
	return system(cmd) == 0;
}

static bool mgba_helper(const char* args, char* err, int errlen) {
	char cmd[1280];
	snprintf(cmd, sizeof(cmd),
	         "SDCARD_PATH='%s' PLATFORM='%s' NETPLAY_PAK='%s' NETPLAY_STATE='%s' "
	         "sh '%s/launcher/mgba-manage.sh' %s >/tmp/netplay-mgba-result 2>&1",
	         ns_sd, ns_platform, ns_pak, ns_state, ns_pak, args);
	int rc = system(cmd);
	if (rc == 0) return true;
	if (err && errlen) {
		FILE* f = fopen("/tmp/netplay-mgba-result", "r");
		if (f && fgets(err, errlen, f)) {
			char* nl = strpbrk(err, "\r\n"); if (nl) *nl = '\0';
		} else snprintf(err, errlen, "mGBA file operation failed.");
		if (f) fclose(f);
	}
	return false;
}

bool NS_mgbaStatus(NS_MgbaStatus* out) {
	if (!out) return false;
	memset(out, 0, sizeof(*out));
	if (!mgba_helper("status", NULL, 0)) return false;
	FILE* f = fopen("/tmp/netplay-mgba-result", "r");
	if (!f) return false;
	char line[160];
	while (fgets(line, sizeof(line), f)) {
		char* nl = strpbrk(line, "\r\n"); if (nl) *nl = '\0';
		char* eq = strchr(line, '='); if (!eq) continue;
		*eq++ = '\0';
		if (!strcmp(line, "installed")) out->installed = atoi(eq) != 0;
		else if (!strcmp(line, "managed")) out->managed = atoi(eq) != 0;
		else if (!strcmp(line, "missing")) out->missing = atoi(eq) != 0;
		else if (!strcmp(line, "mismatch")) out->mismatch = atoi(eq) != 0;
		else if (!strcmp(line, "mismatch_ignored")) out->mismatch_ignored = atoi(eq) != 0;
		else if (!strcmp(line, "backup_date")) snprintf(out->backup_date, sizeof(out->backup_date), "%s", eq);
	}
	fclose(f);
	return true;
}

bool NS_mgbaInstall(char* err, int errlen) { return mgba_helper("install", err, errlen); }
bool NS_mgbaIgnoreMismatch(char* err, int errlen) { return mgba_helper("ignore", err, errlen); }
bool NS_mgbaRestore(bool pak, bool saves, bool states, char* err, int errlen) {
	char args[64];
	snprintf(args, sizeof(args), "restore %d %d %d", pak ? 1 : 0, saves ? 1 : 0, states ? 1 : 0);
	return mgba_helper(args, err, errlen);
}

//////////////////////////////////////////////////////////////////////////////
// core manifest
//////////////////////////////////////////////////////////////////////////////

/* Cores the shim can drive. Kept in step with NETPLAY_CORES in the launcher
 * scripts; a core absent here is simply never offered for sharing. */
static const char* MANIFEST_CORES[] = {
	"fceumm", "picodrive", "snes9x", "snes9x2005", "gambatte", "gpsp",
	"mgba", "mednafen_supafaust", "pcsx_rearmed",
};

uint32_t NS_runtimeGlibc(void) {
	const char* v = gnu_get_libc_version();
	unsigned maj = 0, min = 0;
	if (v && sscanf(v, "%u.%u", &maj, &min) == 2) return maj * 1000 + min;
	return 0;
}

/* CRC32, size, ELF machine and glibc floor in one pass - the file is a few MB
 * and reading it four times would be four times the SD card wear. */
static bool scan_core_file(const char* path, NS_CoreInfo* out) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;

	static uint32_t table[256];
	static int built = 0;
	if (!built) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		built = 1;
	}

	uint32_t crc = 0xFFFFFFFFu, best_glibc = 0;
	size_t total = 0;
	uint16_t machine = 0;
	char buf[16384];
	char tail[32];
	size_t taillen = 0;
	size_t n;

	while ((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
		if (total == 0 && n >= 20 && !memcmp(buf, "\177ELF", 4))
			machine = (uint16_t)((unsigned char)buf[18] | ((unsigned char)buf[19] << 8));
		for (size_t i = 0; i < n; i++) crc = table[(crc ^ (unsigned char)buf[i]) & 0xFF] ^ (crc >> 8);

		/* GLIBC_ strings, searched across the seam between reads. */
		char win[16384 + 32];
		memcpy(win, tail, taillen);
		memcpy(win + taillen, buf, n);
		size_t have = taillen + n;
		win[have] = '\0';
		for (size_t i = 0; i + 10 < have; i++) {
			if (memcmp(win + i, "GLIBC_", 6)) continue;
			unsigned maj = 0, min = 0;
			if (sscanf(win + i + 6, "%u.%u", &maj, &min) == 2) {
				uint32_t v = maj * 1000 + min;
				if (v > best_glibc) best_glibc = v;
			}
		}
		taillen = have > 32 ? 32 : have;
		memcpy(tail, win + have - taillen, taillen);
		total += n;
	}
	fclose(f);
	if (!total) return false;

	out->crc = crc ^ 0xFFFFFFFFu;
	out->size = (uint32_t)total;
	out->machine = machine;
	out->glibc = best_glibc;
	return true;
}

/* library_version without a ROM. */
static void probe_version(const char* path, char* out, int len) {
	struct core_system_info {
		const char* name;
		const char* version;
		const char* extensions;
		bool need_fullpath;
		bool block_extract;
	};
	out[0] = '\0';
	void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!h) { ns_log("manifest: %s will not load here (%s)\n", path, dlerror()); return; }
	void (*gsi)(struct core_system_info*);
	*(void**)&gsi = dlsym(h, "retro_get_system_info");
	if (gsi) {
		struct core_system_info i;
		memset(&i, 0, sizeof(i));
		gsi(&i);
		if (i.version) snprintf(out, len, "%s", i.version);
	}
	dlclose(h);
}

/* The device's installed core is always considered first. Compatibility cores
 * are deliberately excluded here; otherwise merely bundling one would make it
 * look like the device was already using it. */
static bool installed_core_path(const char* core, char* out, int len) {
	snprintf(out, len, "%s/.system/%s/cores/%s_libretro.so", ns_sd, ns_platform, core);
	if (file_exists(out)) return true;

	/* EXTRAS paks bundle their own - mgba lives in MGBA.pak, not the cores dir. */
	char cmd[700];
	snprintf(cmd, sizeof(cmd),
	         "ls '%s/Emus/%s'/*.pak/%s_libretro.so 2>/dev/null | head -1", ns_sd, ns_platform, core);
	FILE* p = popen(cmd, "r");
	if (!p) return false;
	bool ok = false;
	if (fgets(out, len, p)) {
		char* nl = strpbrk(out, "\r\n");
		if (nl) *nl = '\0';
		ok = out[0] != '\0';
	}
	pclose(p);
	return ok;
}

static bool compatibility_core_path(const char* core, char* out, int len) {
	/* A30's stock SFC pak invokes snes9x2005_libretro.so. The shared fallback
	 * uses one canonical filename regardless of which installed Snes9x variant
	 * led to negotiation; both architecture copies contain the same pinned
	 * portable, lower-overhead Supafaust implementation. */
	if (!strcmp(core, "snes9x2005")) core = "snes9x";
	snprintf(out, len, "%s/cores/compatibility/%s/%s_libretro.so",
	         ns_pak, compatibility_arch(), core);
	return file_exists(out);
}

int NS_coreManifest(NS_CoreInfo* out, int max) {
	int n = 0;
	for (size_t i = 0; i < sizeof(MANIFEST_CORES)/sizeof(MANIFEST_CORES[0]) && n < max; i++) {
		memset(&out[n], 0, sizeof(out[n]));
		snprintf(out[n].core, sizeof(out[n].core), "%s", MANIFEST_CORES[i]);

		char path[256];
		if (installed_core_path(MANIFEST_CORES[i], path, sizeof(path))) {
			snprintf(out[n].path, sizeof(out[n].path), "%s", path);
			if (scan_core_file(path, &out[n])) {
				probe_version(path, out[n].version, sizeof(out[n].version));
				out[n].installed = true;
			}
		}

		if (compatibility_core_path(MANIFEST_CORES[i], path, sizeof(path))) {
			NS_CoreInfo compat;
			memset(&compat, 0, sizeof(compat));
			if (scan_core_file(path, &compat)) {
				probe_version(path, out[n].compat_version, sizeof(out[n].compat_version));
				out[n].compat_available = out[n].compat_version[0] != '\0';
			}
		}

		if (!out[n].installed && !out[n].compat_available) continue;
		n++;
	}
	ns_log("manifest: %d core(s), runtime glibc %u.%u\n",
	       n, NS_runtimeGlibc() / 1000, NS_runtimeGlibc() % 1000);
	return n;
}

//////////////////////////////////////////////////////////////////////////////
// core compatibility negotiation
//////////////////////////////////////////////////////////////////////////////

#define CORE_MAGIC 0x4E504333u   /* 'NPC3': metadata-only compatibility protocol */
#define CORE_BUSY  0x42555359u   /* 'BUSY': this session already admitted a peer */
#define CORE_IO_MS   30000
/* The host serves this from its render loop, so its budget is a frozen UI, not
 * merely a slow transfer. The exchange is a few KB on a LAN: a healthy peer is
 * done in well under a second, and anything approaching this is already a peer
 * that is not coming back. */
#define CORE_SERVE_IO_MS 8000

typedef struct __attribute__((packed)) {
	char     core[32];
	char     version[32];
	char     compat_version[32];
	uint32_t crc;
	uint32_t size;
	uint32_t glibc;
	uint16_t machine;
	uint8_t  installed;
	uint8_t  compat_available;
} CoreWire;

static int core_listen_fd = -1;
static char core_last_peer[NS_IP_LEN];

/* Bounded, so neither side can be parked forever by a peer that stops talking
 * mid-transfer - the failure that has bitten this codebase repeatedly.
 *
 * The deadline only bounds anything if the socket can return control. It is
 * checked between calls, so on a *blocking* socket recv() simply never comes
 * back and none of this runs - which is what the host's accepted socket was:
 * the listener is non-blocking, but accept() does not pass O_NONBLOCK to the
 * connection on Linux, and unlike the client path nothing set a timeout on it.
 * A peer that completed the TCP handshake and then went silent parked the
 * host's render loop indefinitely. Every caller must give its socket a receive
 * timeout; see core_socket_deadline. */
static bool io_all_to(int fd, void* buf, size_t len, bool writing, long budget_ms) {
	uint8_t* p = buf;
	size_t done = 0;
	struct timeval start, now;
	gettimeofday(&start, NULL);

	while (done < len) {
		gettimeofday(&now, NULL);
		long ms = (now.tv_sec - start.tv_sec) * 1000L + (now.tv_usec - start.tv_usec) / 1000L;
		if (ms > budget_ms) return false;

		ssize_t n = writing ? send(fd, p + done, len - done, MSG_NOSIGNAL)
		                    : recv(fd, p + done, len - done, 0);
		if (n > 0) { done += (size_t)n; continue; }
		if (n == 0) return false;                       /* peer closed */
		if (errno == EINTR) continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(20 * 1000); continue; }
		return false;
	}
	return true;
}

/* Retained for the frozen core-sharing block below, which is `#if 0` and so
 * has no live caller - keeping it means that code still compiles unchanged if
 * the setting ever returns. */
__attribute__((unused))
static bool io_all(int fd, void* buf, size_t len, bool writing) {
	return io_all_to(fd, buf, len, writing, CORE_IO_MS);
}

/* Give a socket a send/receive timeout, so a silent peer surfaces as EAGAIN and
 * the deadline above is reachable. Well under the caller's budget, so the
 * budget is what actually decides when to give up. */
static void core_socket_deadline(int fd, long budget_ms) {
	struct timeval tv = {
		.tv_sec  = (budget_ms / 4) / 1000,
		.tv_usec = ((budget_ms / 4) % 1000) * 1000,
	};
	if (!tv.tv_sec && !tv.tv_usec) tv.tv_usec = 250 * 1000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

#define INST_CAP_GAMBATTE (1u << 0)
#define INST_CAP_MGBA     (1u << 1)

static uint32_t instanced_available_mask(void);

static bool send_manifest_to(int fd, NS_CoreInfo* m, int n, long budget_ms) {
	uint32_t hdr[5] = { htonl(CORE_MAGIC), htonl((uint32_t)n),
	                    htonl(NS_settings()->compatibility_cores ? 1u : 0u),
	                    htonl(NS_settings()->force_compatibility ? 1u : 0u),
	                    htonl(instanced_available_mask()) };
	if (!io_all_to(fd, hdr, sizeof(hdr), true, budget_ms)) return false;
	for (int i = 0; i < n; i++) {
		CoreWire w;
		memset(&w, 0, sizeof(w));
		snprintf(w.core, sizeof(w.core), "%s", m[i].core);
		snprintf(w.version, sizeof(w.version), "%s", m[i].version);
		snprintf(w.compat_version, sizeof(w.compat_version), "%s", m[i].compat_version);
		w.crc = htonl(m[i].crc);
		w.size = htonl(m[i].size);
		w.glibc = htonl(m[i].glibc);
		w.machine = htons(m[i].machine);
		w.installed = m[i].installed ? 1 : 0;
		w.compat_available = m[i].compat_available ? 1 : 0;
		if (!io_all_to(fd, &w, sizeof(w), true, budget_ms)) return false;
	}
	return true;
}

static int recv_manifest_to(int fd, NS_CoreInfo* out, int max, long budget_ms,
                         bool* peer_compat_enabled, bool* peer_force_compatibility,
                         uint32_t* peer_instanced) {
	uint32_t hdr[5];
	if (!io_all_to(fd, hdr, sizeof(hdr), false, budget_ms)) return -1;
	if (ntohl(hdr[0]) == CORE_BUSY) return -2;
	if (ntohl(hdr[0]) != CORE_MAGIC) return -1;
	int n = (int)ntohl(hdr[1]);
	*peer_compat_enabled = ntohl(hdr[2]) != 0;
	*peer_force_compatibility = ntohl(hdr[3]) != 0;
	*peer_instanced = ntohl(hdr[4]);
	if (n < 0 || n > NS_MAX_MANIFEST) return -1;

	int kept = 0;
	for (int i = 0; i < n; i++) {
		CoreWire w;
		if (!io_all_to(fd, &w, sizeof(w), false, budget_ms)) return -1;
		if (kept >= max) continue;
		w.core[sizeof(w.core) - 1] = '\0';
		w.version[sizeof(w.version) - 1] = '\0';
		w.compat_version[sizeof(w.compat_version) - 1] = '\0';
		memset(&out[kept], 0, sizeof(out[kept]));
		snprintf(out[kept].core, sizeof(out[kept].core), "%s", w.core);
		snprintf(out[kept].version, sizeof(out[kept].version), "%s", w.version);
		snprintf(out[kept].compat_version, sizeof(out[kept].compat_version), "%s", w.compat_version);
		out[kept].crc = ntohl(w.crc);
		out[kept].size = ntohl(w.size);
		out[kept].glibc = ntohl(w.glibc);
		out[kept].machine = ntohs(w.machine);
		out[kept].installed = w.installed != 0;
		out[kept].compat_available = w.compat_available != 0;
		kept++;
	}
	return kept;
}

static NS_CoreInfo* find_core(NS_CoreInfo* m, int n, const char* name) {
	for (int i = 0; i < n; i++) if (!strcmp(m[i].core, name)) return &m[i];
	return NULL;
}

static bool installed_builds_match(const char* core, const NS_CoreInfo* a, const NS_CoreInfo* b) {
	if (!a || !b || !a->installed || !b->installed) return false;
	/* Link cores need the pak's networking fixes, not merely the same upstream
	 * revision on both devices. A system core carrying the same marked build is
	 * fine; an unmarked stock build is not feature-compatible. */
	if (!strcmp(core, "gambatte") || !strcmp(core, "gpsp")) {
		if (!a->compat_available || !b->compat_available ||
		    strcmp(a->version, a->compat_version) || strcmp(b->version, b->compat_version))
			return false;
	}
	if (a->version[0] && b->version[0]) return !strcmp(a->version, b->version);
	return a->machine == b->machine && a->size == b->size && a->crc == b->crc;
}

static bool compatibility_builds_match(const NS_CoreInfo* a, const NS_CoreInfo* b) {
	return a && b && a->compat_available && b->compat_available &&
	       a->compat_version[0] && !strcmp(a->compat_version, b->compat_version);
}

/* Rewrite only the generated compatibility selections, preserving role,
 * transport and hand-edited options. Both copies must agree because the
 * launcher reads session while session.conf is the durable shared template. */
static bool write_compat_file(const char* path,
                              const char selected[][32], int count,
                              const char mismatched[][32], int mismatch_count,
                              uint32_t instanced_mask) {
	char tmp[560];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE* in = fopen(path, "r");
	if (!in) return false;
	FILE* out = fopen(tmp, "w");
	if (!out) { fclose(in); return false; }

	char line[512];
	while (fgets(line, sizeof(line), in))
		if (strncmp(line, "compat_core.", 12) &&
		    strncmp(line, "core_mismatch.", 14) &&
		    strncmp(line, "instanced_gambatte=", 19) &&
		    strncmp(line, "instanced_mgba=", 15)) fputs(line, out);
	for (int i = 0; i < count; i++) fprintf(out, "compat_core.%s=1\n", selected[i]);
	for (int i = 0; i < mismatch_count; i++)
		fprintf(out, "core_mismatch.%s=1\n", mismatched[i]);
	/* NS_arm wrote this device's wish; the exchange replaces it with the
	 * agreement. Instanced play cannot be one-sided - a device running the
	 * paired core against a peer running the ordinary network-serial core has
	 * nothing to talk to - and both session files are rewritten here. */
	fprintf(out, "instanced_gambatte=%d\n", (instanced_mask & INST_CAP_GAMBATTE) ? 1 : 0);
	fprintf(out, "instanced_mgba=%d\n", (instanced_mask & INST_CAP_MGBA) ? 1 : 0);

	fclose(in);
	if (fclose(out) != 0 || rename(tmp, path) != 0) {
		remove(tmp);
		return false;
	}
	return true;
}

/* Whether this device can offer each paired core: its setting is on and its
 * paired artifact is actually staged. Each bit must be present on both devices,
 * so the masks are exchanged with the manifest and intersected. */
static uint32_t instanced_available_mask(void) {
	char path[512];
	uint32_t mask = 0;
	snprintf(path, sizeof(path), "%s/cores/override/%s/gambatte_dual_libretro.so",
	         ns_pak, ns_platform);
	if (instanced_core_enabled("gambatte") && file_exists(path))
		mask |= INST_CAP_GAMBATTE;
	snprintf(path, sizeof(path), "%s/cores/override/%s/mgba_dual_libretro.so",
	         ns_pak, ns_platform);
	if (instanced_core_enabled("mgba") && file_exists(path))
		mask |= INST_CAP_MGBA;
	return mask;
}

static int select_compatibility(NS_CoreInfo* mine, int mn, NS_CoreInfo* theirs, int tn,
								bool peer_enabled, bool peer_force,
								uint32_t peer_instanced) {
	char selected[NS_MAX_MANIFEST][32];
	char mismatched[NS_MAX_MANIFEST][32];
	int count = 0;
	int mismatch_count = 0;
	bool enabled = NS_settings()->compatibility_cores && peer_enabled;
	bool forced = enabled && (NS_settings()->force_compatibility || peer_force);

	for (size_t i = 0; i < sizeof(MANIFEST_CORES) / sizeof(MANIFEST_CORES[0]); i++) {
		NS_CoreInfo* a = find_core(mine, mn, MANIFEST_CORES[i]);
		NS_CoreInfo* b = find_core(theirs, tn, MANIFEST_CORES[i]);
		if (forced && (a || b)) {
			if (compatibility_builds_match(a, b)) {
				snprintf(selected[count++], sizeof(selected[0]), "%s", MANIFEST_CORES[i]);
				ns_log("%s: testing override selected the packaged compatibility core\n",
				       MANIFEST_CORES[i]);
			} else {
				ns_log("%s: testing override requested but no common packaged core exists\n",
				       MANIFEST_CORES[i]);
			}
			continue;
		}
		if (installed_builds_match(MANIFEST_CORES[i], a, b)) {
			ns_log("%s: installed builds match; keeping them\n", MANIFEST_CORES[i]);
			continue;
		}
		if (enabled && compatibility_builds_match(a, b)) {
			snprintf(selected[count++], sizeof(selected[0]), "%s", MANIFEST_CORES[i]);
			ns_log("%s: installed builds differ; both will use compatibility cores\n",
			       MANIFEST_CORES[i]);
		} else if (a && b && (a->installed || b->installed)) {
			snprintf(mismatched[mismatch_count++], sizeof(mismatched[0]), "%s",
			         MANIFEST_CORES[i]);
			ns_log("%s: installed builds differ and no common compatibility core is enabled\n",
			       MANIFEST_CORES[i]);
		}
	}
	uint32_t local_instanced = instanced_available_mask();
	uint32_t instanced = local_instanced & peer_instanced;
	ns_log("paired cores: here=0x%x peer=0x%x agreed=0x%x (gambatte=%d mgba=%d)\n",
	       local_instanced, peer_instanced, instanced,
	       (instanced & INST_CAP_GAMBATTE) ? 1 : 0,
	       (instanced & INST_CAP_MGBA) ? 1 : 0);

	char path[512];
	snprintf(path, sizeof(path), "%s/session.conf", ns_state);
	if (!write_compat_file(path, selected, count, mismatched, mismatch_count, instanced)) {
		ns_log("cannot update compatibility selections in session.conf\n");
		return -1;
	}
	snprintf(path, sizeof(path), "%s/session", ns_state);
	if (!write_compat_file(path, selected, count, mismatched, mismatch_count, instanced)) {
		ns_log("cannot update compatibility selections in active session\n");
		/* Do not leave a durable selection that disagrees with the active file. */
		snprintf(path, sizeof(path), "%s/session.conf", ns_state);
		write_compat_file(path, selected, 0, mismatched, 0, false);
		return -1;
	}
	return count;
}

//////////////////////////////////////////////////////////////////////////////
// host side

bool NS_compatServeStart(void) {
	if (core_listen_fd >= 0) return true;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		ns_log("core compatibility negotiation: socket failed: %s\n", strerror(errno));
		return false;
	}
	int on = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = INADDR_ANY;
	a.sin_port = htons(NS_CORE_PORT);
	if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(fd, 2) != 0) {
		ns_log("core compatibility negotiation: listen failed: %s\n", strerror(errno));
		close(fd);
		return false;
	}
	core_listen_fd = fd;
	ns_log("core compatibility negotiation listening on %d\n", NS_CORE_PORT);
	return true;
}

void NS_compatServeStop(void) {
	if (core_listen_fd >= 0) { close(core_listen_fd); core_listen_fd = -1; }
}

const char* NS_compatLastPeer(void) { return core_last_peer; }

void NS_compatServeTick(void) {
	if (core_listen_fd < 0) return;

	struct sockaddr_in peer;
	socklen_t peer_len = sizeof(peer);
	int fd = accept(core_listen_fd, (struct sockaddr*)&peer, &peer_len);
	if (fd < 0) return;                       /* nothing waiting - normal */
	/* accept() does not inherit the listener's O_NONBLOCK on Linux, so without
	 * this the connection is fully blocking and every deadline below is
	 * unreachable. This is the whole of the fix; the rest is bookkeeping. */
	core_socket_deadline(fd, CORE_SERVE_IO_MS);
	char peer_ip[NS_IP_LEN] = "";
	inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
	if (core_last_peer[0] && strcmp(core_last_peer, peer_ip)) {
		uint32_t busy[5] = { htonl(CORE_BUSY), 0, 0, 0, 0 };
		io_all_to(fd, busy, sizeof(busy), true, CORE_SERVE_IO_MS);
		ns_log("core compatibility negotiation: %s rejected BUSY; admitted peer is %s\n",
		       peer_ip, core_last_peer);
		close(fd);
		return;
	}

	NS_CoreInfo mine[NS_MAX_MANIFEST];
	int n = NS_coreManifest(mine, NS_MAX_MANIFEST);

	NS_CoreInfo theirs[NS_MAX_MANIFEST];
	bool peer_compat_enabled = false;
	bool peer_force_compatibility = false;
	uint32_t peer_instanced = 0;
	int tn = recv_manifest_to(fd, theirs, NS_MAX_MANIFEST, CORE_SERVE_IO_MS,
	                          &peer_compat_enabled, &peer_force_compatibility,
	                          &peer_instanced);
	if (tn < 0 ||
	    !send_manifest_to(fd, mine, n, CORE_SERVE_IO_MS)) {
		ns_log("core compatibility negotiation: manifest failed "
		       "(peer %s went quiet or spoke nonsense)\n", peer_ip);
		close(fd);
		return;
	}
	snprintf(core_last_peer, sizeof(core_last_peer), "%s", peer_ip);
	int selected = select_compatibility(mine, n, theirs, tn,
	                                    peer_compat_enabled, peer_force_compatibility,
	                                    peer_instanced);
	if (selected < 0)
		ns_log("core compatibility negotiation: could not activate selections\n");
	else
		ns_log("core compatibility negotiation: %d fallback(s) selected\n", selected);

	/* Frozen core-sharing implementation. Executable files must never be
	 * accepted from an unauthenticated peer. Kept here, excluded from the build,
	 * so the setting and its former implementation can be revisited together. */
#if 0
	/* Serve requests until the client says it is done. The client drives,
	 * because it is the side that knows what it could not load. */
	for (;;) {
		uint8_t op;
		char name[32];
		if (!io_all(fd, &op, 1, false)) break;
		if (op == CORE_OP_DONE) break;
		if (!io_all(fd, name, sizeof(name), false)) break;
		name[sizeof(name) - 1] = '\0';

		if (op == CORE_OP_GET) {
			NS_CoreInfo* c = find_core(mine, n, name);
			uint32_t len = 0;
			uint8_t* buf = NULL;
			if (c) {
				FILE* f = fopen(c->path, "rb");
				if (f) {
					buf = malloc(c->size);
					if (buf && fread(buf, 1, c->size, f) == c->size) len = c->size;
					else { free(buf); buf = NULL; }
					fclose(f);
				}
			}
			uint32_t nl_len = htonl(len);
			if (!io_all(fd, &nl_len, 4, true)) { free(buf); break; }
			if (len && !io_all(fd, buf, len, true)) { free(buf); break; }
			ns_log("core exchange: sent %s (%u bytes)\n", name, len);
			free(buf);
		} else if (op == CORE_OP_PUT) {
			uint32_t len;
			if (!io_all(fd, &len, 4, false)) break;
			len = ntohl(len);
			if (!len || len > CORE_MAX_BYTES) break;
			uint8_t* buf = malloc(len);
			if (!buf || !io_all(fd, buf, len, false)) { free(buf); break; }

			char dir[512], path[600], cmd[700];
			staged_dir(dir, sizeof(dir));
			snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
			system(cmd);
			snprintf(path, sizeof(path), "%s/%s_libretro.so", dir, name);
			FILE* out = fopen(path, "wb");
			if (out) {
				size_t w = fwrite(buf, 1, len, out);
				fclose(out);
				if (w == len) ns_log("core exchange: staged %s from peer (%u bytes)\n", name, len);
				else remove(path);
			}
			free(buf);
		} else break;
	}
#endif
	close(fd);
}

//////////////////////////////////////////////////////////////////////////////
// client side

/* Compare installed builds first, then select the pak fallback on both sides
 * only when both settings and both local compatibility artifacts permit it. */
int NS_compatSync(const char* host_ip, char* err, int errlen) {
	NS_CoreInfo mine[NS_MAX_MANIFEST];
	int n = NS_coreManifest(mine, NS_MAX_MANIFEST);
	if (n <= 0) { snprintf(err, errlen, "no cores found locally"); return -1; }

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) { snprintf(err, errlen, "no socket"); return -1; }

	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(NS_CORE_PORT);
	if (inet_pton(AF_INET, host_ip, &a.sin_addr) != 1) {
		close(fd);
		snprintf(err, errlen, "bad host address");
		return -1;
	}

	core_socket_deadline(fd, CORE_IO_MS);

	if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) {
		close(fd);
		snprintf(err, errlen, "host is not serving cores");
		return -1;
	}

	NS_CoreInfo theirs[NS_MAX_MANIFEST];
	bool peer_compat_enabled = false;
	bool peer_force_compatibility = false;
	uint32_t peer_instanced = 0;
	if (!send_manifest_to(fd, mine, n, CORE_IO_MS)) {
		close(fd);
		snprintf(err, errlen, "manifest exchange failed");
		return -1;
	}
	int tn = recv_manifest_to(fd, theirs, NS_MAX_MANIFEST, CORE_IO_MS,
	                       &peer_compat_enabled, &peer_force_compatibility,
	                       &peer_instanced);
	if (tn == -2) {
		close(fd);
		snprintf(err, errlen, "host is busy with another guest");
		return -1;
	}
	if (tn < 0) {
		close(fd);
		snprintf(err, errlen, "manifest exchange failed");
		return -1;
	}

	int selected = select_compatibility(mine, n, theirs, tn,
	                                    peer_compat_enabled, peer_force_compatibility,
	                                    peer_instanced);
	if (selected == -2)
		snprintf(err, errlen, "forced compatibility core unavailable");
	else if (selected < 0)
		snprintf(err, errlen, "cannot activate core selection");

	/* Frozen peer-to-peer executable transfer. Compatibility negotiation above
	 * exchanges metadata only and selects local, packaged artifacts. */
#if 0
	uint32_t my_runtime = NS_runtimeGlibc();
	int staged = 0, attempted = 0;
	for (int i = 0; i < n; i++) {
		NS_CoreInfo* t = find_core(theirs, tn, mine[i].core);
		if (!t || !t->version[0]) continue;                 /* peer lacks it */
		if (mine[i].version[0] && !strcmp(mine[i].version, t->version)) continue;
		if (t->machine != mine[i].machine) continue;        /* unbridgeable */

		attempted++;
		bool we_can_load_theirs = t->glibc <= my_runtime;
		bool they_can_load_ours = mine[i].glibc <= peer_runtime;

		if (we_can_load_theirs) {
			uint8_t op = CORE_OP_GET;
			char name[32];
			memset(name, 0, sizeof(name));
			snprintf(name, sizeof(name), "%s", mine[i].core);
			if (!io_all(fd, &op, 1, true) || !io_all(fd, name, sizeof(name), true)) break;

			uint32_t len;
			if (!io_all(fd, &len, 4, false)) break;
			len = ntohl(len);
			if (!len || len > CORE_MAX_BYTES) continue;
			uint8_t* buf = malloc(len);
			if (!buf) break;
			if (!io_all(fd, buf, len, false)) { free(buf); break; }

			char dir[512], path[600], cmd[700];
			staged_dir(dir, sizeof(dir));
			snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
			system(cmd);
			snprintf(path, sizeof(path), "%s/%s_libretro.so", dir, mine[i].core);
			FILE* out = fopen(path, "wb");
			bool ok = false;
			if (out) {
				ok = fwrite(buf, 1, len, out) == len;
				fclose(out);
			}
			free(buf);

			/* Prove it loads before leaving it where the shim will find it. */
			if (ok) {
				void* probe = dlopen(path, RTLD_NOW | RTLD_LOCAL);
				if (probe) { dlclose(probe); staged++;
					ns_log("staged %s from host (%u bytes)\n", mine[i].core, len); }
				else { ns_log("host's %s will not load here (%s)\n", mine[i].core, dlerror());
					remove(path); ok = false; }
			}
			if (!ok) remove(path);

		} else if (they_can_load_ours) {
			/* We cannot use theirs but they can use ours - push instead. */
			FILE* f = fopen(mine[i].path, "rb");
			if (!f) continue;
			uint8_t* buf = malloc(mine[i].size);
			if (!buf) { fclose(f); continue; }
			size_t got = fread(buf, 1, mine[i].size, f);
			fclose(f);

			uint8_t op = CORE_OP_PUT;
			char name[32];
			memset(name, 0, sizeof(name));
			snprintf(name, sizeof(name), "%s", mine[i].core);
			uint32_t len = htonl((uint32_t)got);
			if (got == mine[i].size &&
			    io_all(fd, &op, 1, true) && io_all(fd, name, sizeof(name), true) &&
			    io_all(fd, &len, 4, true) && io_all(fd, buf, got, true)) {
				ns_log("pushed our %s to the host (%zu bytes)\n", mine[i].core, got);
				staged++;
			}
			free(buf);
		} else {
			ns_log("%s: neither build is loadable on the other device\n", mine[i].core);
		}
	}

	uint8_t done = CORE_OP_DONE;
	io_all(fd, &done, 1, true);
	(void)my_runtime;
	(void)attempted;
#endif
	close(fd);

	if (selected >= 0)
		ns_log("core compatibility negotiation: %d fallback(s) selected\n", selected);
	return selected;
}

//////////////////////////////////////////////////////////////////////////////
// assumption checks
//////////////////////////////////////////////////////////////////////////////

static void add(NS_Check* out, int* n, int max, NS_CheckResult r,
                const char* label, const char* detail) {
	if (*n >= max) return;
	out[*n].result = r;
	snprintf(out[*n].label, sizeof(out[*n].label), "%s", label);
	snprintf(out[*n].detail, sizeof(out[*n].detail), "%s", detail);
	(*n)++;
}

int NS_runChecks(NS_Check* out, int max, NS_CheckResult* worst) {
	int n = 0;
	NS_CheckResult w = NS_CHECK_OK;
	char path[512], line[512];

	/* 1. Emu paks must invoke minarch.elf unqualified, or our PATH shim is
	 *    never consulted and netplay silently does nothing. */
	int checked = 0, unqualified = 0;
	snprintf(path, sizeof(path), "%s/.system/%s/paks/Emus", ns_sd, ns_platform);
	{
		char cmd[1200];
		snprintf(cmd, sizeof(cmd),
		         "for p in '%s'/*.pak; do "
		         "f=\"$p/launch.sh\"; "
		         "if grep -q 'Installed by Netplay.pak' \"$f\" 2>/dev/null && "
		         "[ -f \"$p/launch.sh.old\" ]; then f=\"$p/launch.sh.old\"; fi; "
		         "grep -q 'minarch.elf' \"$f\" 2>/dev/null && echo \"$f\"; "
		         "done", path);
		FILE* f = popen(cmd, "r");
		if (f) {
			while (fgets(line, sizeof(line), f)) {
				char* nl = strpbrk(line, "\r\n"); if (nl) *nl = '\0';
				checked++;
				char cmd2[700];
				/* An absolute path before minarch.elf means PATH is bypassed. */
				snprintf(cmd2, sizeof(cmd2),
				         "grep -qE '(^|[^/[:alnum:]_])minarch\\.elf' '%s' && "
				         "! grep -qE '/[A-Za-z0-9_./-]*/minarch\\.elf' '%s'", line, line);
				if (system(cmd2) == 0) unqualified++;
			}
			pclose(f);
		}
	}
	if (checked == 0)
		add(out, &n, max, NS_CHECK_FAIL, "Emulator paks", "none found where expected");
	else if (unqualified == 0)
		add(out, &n, max, NS_CHECK_FAIL, "minarch on PATH", "paks call it by absolute path");
	else if (unqualified < checked)
		add(out, &n, max, NS_CHECK_WARN, "minarch on PATH", "some paks bypass PATH");
	else
		add(out, &n, max, NS_CHECK_OK, "minarch on PATH", "resolved via PATH");

	/* 2. An SD-card Emus pak must win over the system one, or stubs are inert. */
	{
		char probe[512];
		snprintf(probe, sizeof(probe), "%s/Emus/%s", ns_sd, ns_platform);
		/* Both levels: a freshly formatted card ships Bios, Roms, Saves, Tools
		 * and nothing else, so Emus/ does not exist yet and a single mkdir fails
		 * on the missing parent. The check then reported a hard failure - which
		 * refuses to arm - for a directory the installer creates for itself
		 * without difficulty. */
		char parent[512];
		snprintf(parent, sizeof(parent), "%s/Emus", ns_sd);
		mkdir(parent, 0755);
		mkdir(probe, 0755);
		bool writable = access(probe, W_OK) == 0;
		add(out, &n, max,
		    writable ? NS_CHECK_OK : NS_CHECK_FAIL,
		    "SD pak override", writable ? "path writable" : "cannot create or write");
	}

	/* 3. core.name is basename truncated at the last underscore, and it feeds
	 *    config_dir AND states_dir. If that ever changes, a staged shim would
	 *    quietly relocate the user's save states - the only check here whose
	 *    failure costs data rather than features. */
	{
		const char* sample = "gpsp_libretro.so";
		char derived[64];
		snprintf(derived, sizeof(derived), "%s", sample);
		char* us = strrchr(derived, '_');
		if (us) *us = '\0';

		char states[512];
		snprintf(states, sizeof(states), "%s/.userdata/shared/GBA-%s", ns_sd, derived);
		bool ok = (strcmp(derived, "gpsp") == 0);
		char detail[96];
		snprintf(detail, sizeof(detail), ok && file_exists(states)
		         ? "save states resolve to GBA-gpsp"
		         : (ok ? "naming ok (no existing GBA states)" : "naming rule changed"));
		add(out, &n, max, ok ? NS_CHECK_OK : NS_CHECK_FAIL, "Save state paths", detail);
	}

	/* 4. Our own pieces are present for this platform. */
	{
		char shim[512];
		snprintf(shim, sizeof(shim), "%s/bin/%s/netplay_shim.so", ns_pak, ns_platform);
		bool ok = file_exists(shim);
		add(out, &n, max, ok ? NS_CHECK_OK : NS_CHECK_FAIL, "Shim for this device",
		    ok ? ns_platform : "missing - unsupported platform");
	}

	/* 5. This device can tell whether it is associated, and can get an address.
	 *
	 * Both were assumed rather than checked, and on the H700 both assumptions
	 * were false: no `iw`, no `udhcpc`. Nothing said so. A missing association
	 * probe is indistinguishable from "not associated", so the join loop ran
	 * its full budget and reported that it could not find the host; a missing
	 * DHCP client meant the restore path could never produce an address and
	 * left WiFi needing a reboot after every session. Two silent absences cost
	 * two rounds of field debugging, which is what this check is for.
	 *
	 * A warning, not a failure. Shared-screen play over an existing network
	 * touches none of this - it is only ad hoc that moves the client stack -
	 * so refusing to arm would be wrong. */
	{
		/* Short tokens: "iw", "wpa_cli", "dhclient", "dhcpcd", "none". Sized
		 * so both fit in a detail line with room to spare. */
		char assoc[24] = "", dhcp[24] = "";
		wifi_plat_out(assoc, sizeof(assoc), "assoc-kind");
		wifi_plat_out(dhcp,  sizeof(dhcp),  "dhcp-kind");

		bool no_assoc = !assoc[0] || !strcmp(assoc, "none");
		bool no_dhcp  = !dhcp[0]  || !strcmp(dhcp,  "none");

		char detail[96];
		if (no_assoc && no_dhcp)
			snprintf(detail, sizeof(detail), "no association probe and no DHCP client");
		else if (no_assoc)
			snprintf(detail, sizeof(detail), "no association probe (%s for DHCP)", dhcp);
		else if (no_dhcp)
			snprintf(detail, sizeof(detail), "%s works, but no DHCP client", assoc);
		else
			snprintf(detail, sizeof(detail), "%s + %s", assoc, dhcp);

		add(out, &n, max, (no_assoc || no_dhcp) ? NS_CHECK_WARN : NS_CHECK_OK,
		    "WiFi tools for ad hoc", detail);
	}

	for (int i = 0; i < n; i++) if (out[i].result > w) w = out[i].result;
	if (worst) *worst = w;
	return n;
}

//////////////////////////////////////////////////////////////////////////////
// session
//////////////////////////////////////////////////////////////////////////////

bool NS_isArmed(void) {
	char p[512];
	snprintf(p, sizeof(p), "%s/session", ns_state);
	return file_exists(p);
}

bool NS_cleanupStaleSession(void) {
	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
	         "SDCARD_PATH='%s' PLATFORM='%s' NETPLAY_PAK='%s' "
	         "sh '%s/launcher/session-cleanup.sh' >/dev/null 2>&1",
	         ns_sd, ns_platform, ns_pak, ns_pak);
	int rc = system(cmd);
	return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 10;
}

static int broker_pid(void) {
	char path[512];
	snprintf(path, sizeof(path), "%s/broker.pid", ns_state);
	FILE* f = fopen(path, "r");
	int pid = 0;
	if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
	if (pid <= 1) return 0;

	char proc[64];
	snprintf(proc, sizeof(proc), "/proc/%d/cmdline", pid);
	f = fopen(proc, "r");
	if (!f) return 0;
	char cmdline[256] = "";
	size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, f);
	fclose(f);
	cmdline[n] = '\0';
	return strstr(cmdline, "netplay-broker.elf") ? pid : 0;
}

bool NS_sessionInfo(NS_SessionInfo* out) {
	if (!out) return false;
	memset(out, 0, sizeof(*out));

	char path[512];
	snprintf(path, sizeof(path), "%s/session", ns_state);
	FILE* f = fopen(path, "r");
	if (!f) return false;
	out->armed = true;
	out->role = NS_ROLE_HOST;

	char line[192];
	while (fgets(line, sizeof(line), f)) {
		char* nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';
		char* value = strchr(line, '=');
		if (!value) continue;
		*value++ = '\0';
		if (!strcmp(line, "role")) out->role = !strcmp(value, "client") ? NS_ROLE_CLIENT : NS_ROLE_HOST;
		else if (!strcmp(line, "peer")) snprintf(out->host, sizeof(out->host), "%s", value);
		else if (!strcmp(line, "adhoc_ssid")) snprintf(out->network, sizeof(out->network), "%s", value);
		else if (!strcmp(line, "adhoc_psk")) snprintf(out->psk, sizeof(out->psk), "%s", value);
	}
	fclose(f);
	if (out->role == NS_ROLE_HOST && !out->network[0] &&
	    system("pidof hostapd >/dev/null 2>&1") == 0) {
		/* Upgrade an already-running host created before host metadata was
		 * written into the shared session record. */
		f = fopen("/tmp/netplay_hostapd.conf", "r");
		while (f && fgets(line, sizeof(line), f)) {
			char value[NS_PSK_LEN];
			if (sscanf(line, "ssid=%23s", value) == 1)
				snprintf(out->network, sizeof(out->network), "%s", value);
			else if (sscanf(line, "wpa_passphrase=%23s", value) == 1)
				snprintf(out->psk, sizeof(out->psk), "%s", value);
		}
		if (f) fclose(f);
	}

	out->broker_running = broker_pid() != 0;
	if (out->role != NS_ROLE_HOST || !out->broker_running) return true;

	snprintf(path, sizeof(path), "%s/broker.status", ns_state);
	f = fopen(path, "r");
	if (!f) return true;
	while (fgets(line, sizeof(line), f)) {
		char* nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';
		int i;
		if (sscanf(line, "guest_count=%d", &i) == 1) {
			out->guest_count = i < 0 ? 0 : i > 4 ? 4 : i;
			continue;
		}
		char* value = strchr(line, '=');
		if (!value) continue;
		*value++ = '\0';
		if (sscanf(line, "guest_%d_id", &i) == 1 && i >= 0 && i < 4)
			snprintf(out->guest[i].id, sizeof(out->guest[i].id), "%s", value);
		else if (sscanf(line, "guest_%d_ip", &i) == 1 && i >= 0 && i < 4)
			snprintf(out->guest[i].ip, sizeof(out->guest[i].ip), "%s", value);
	}
	fclose(f);
	return true;
}

bool NS_brokerStart(char* err, int errlen) {
	if (broker_pid()) return true;
	char stale[512];
	snprintf(stale, sizeof(stale), "%s/broker.pid", ns_state); remove(stale);
	snprintf(stale, sizeof(stale), "%s/broker.status", ns_state); remove(stale);

	char executable[512];
	snprintf(executable, sizeof(executable), "%s/bin/%s/netplay-broker.elf", ns_pak, ns_platform);
	if (access(executable, X_OK) != 0) {
		snprintf(err, errlen, "persistent host broker is missing");
		return false;
	}

	pid_t pid = fork();
	if (pid < 0) {
		snprintf(err, errlen, "could not start host broker");
		return false;
	}
	if (pid == 0) {
		setsid();
		char log_path[512];
		snprintf(log_path, sizeof(log_path), "%s/broker.log", ns_state);
		int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
		int null_fd = open("/dev/null", O_RDONLY);
		if (null_fd >= 0) { dup2(null_fd, STDIN_FILENO); close(null_fd); }
		if (log_fd >= 0) {
			dup2(log_fd, STDOUT_FILENO);
			dup2(log_fd, STDERR_FILENO);
			if (log_fd > STDERR_FILENO) close(log_fd);
		}
		long max_fd = sysconf(_SC_OPEN_MAX);
		if (max_fd < 0 || max_fd > 4096) max_fd = 4096;
		for (int fd = STDERR_FILENO + 1; fd < max_fd; fd++) close(fd);
		setenv("SDCARD_PATH", ns_sd, 1);
		setenv("PLATFORM", ns_platform, 1);
		execl(executable, executable, (char*)NULL);
		_exit(127);
	}

	for (int i = 0; i < 30; i++) {
		usleep(100 * 1000);
		snprintf(stale, sizeof(stale), "%s/broker.status", ns_state);
		if (broker_pid() && file_exists(stale)) return true;
		if (kill(pid, 0) != 0 && errno == ESRCH) break;
	}
	waitpid(pid, NULL, WNOHANG);
	snprintf(err, errlen, "host broker did not start");
	return false;
}

void NS_brokerStop(void) {
	int pid = broker_pid();
	if (pid) {
		kill(pid, SIGTERM);
		for (int i = 0; i < 20 && kill(pid, 0) == 0; i++) {
			int status;
			if (waitpid(pid, &status, WNOHANG) == pid) break;
			usleep(100 * 1000);
		}
		if (kill(pid, 0) == 0) kill(pid, SIGKILL);
		waitpid(pid, NULL, WNOHANG);
	}
	char path[512];
	snprintf(path, sizeof(path), "%s/broker.pid", ns_state); remove(path);
	snprintf(path, sizeof(path), "%s/broker.status", ns_state); remove(path);
}

bool NS_arm(NS_Role role, const char* peer_ip, char* err, int errlen) {
	char path[512];

	snprintf(path, sizeof(path), "%s/session.conf", ns_state);
	FILE* f = fopen(path, "w");
	if (!f) {
		snprintf(err, errlen, "cannot write session.conf");
		return false;
	}

	/* No mode= line. The shim reads the core's library_name and decides, so a
	 * single armed session covers every system. Writing one here would pin all
	 * of them to whichever mode was picked before a game was even chosen.
	 * Hand-editing mode=netplay or mode=link into this file still overrides. */

	/* A new arm must never inherit a checkpoint from an older play session of
	 * the same ROM. The identifier is local to each device; it only namespaces
	 * that device's /tmp checkpoint and therefore need not be negotiated. */
	unsigned char nonce[16];
	memset(nonce, 0, sizeof(nonce));
	FILE* random = fopen("/dev/urandom", "rb");
	size_t random_bytes = 0;
	if (random) { random_bytes = fread(nonce, 1, sizeof(nonce), random); fclose(random); }
	if (random_bytes != sizeof(nonce)) {
		struct timeval now;
		gettimeofday(&now, NULL);
		memcpy(nonce, &now, sizeof(now) < sizeof(nonce) ? sizeof(now) : sizeof(nonce));
		nonce[sizeof(nonce) - 1] ^= (unsigned char)getpid();
	}
	for (size_t i = 0; i < sizeof(nonce); i++) fprintf(f, i ? "%02x" : "session_id=%02x", nonce[i]);
	fputc('\n', f);

	/* App-process persistence is intentional; cross-boot persistence is not.
	 * A boot identity lets the launcher discard an otherwise-valid session
	 * after a crash/reboot without using guest presence as a liveness signal. */
	FILE* boot = fopen("/proc/sys/kernel/random/boot_id", "r");
	if (boot) {
		char boot_id[80] = "";
		if (fgets(boot_id, sizeof(boot_id), boot)) {
			char* nl = strpbrk(boot_id, "\r\n");
			if (nl) *nl = '\0';
			if (boot_id[0]) fprintf(f, "boot_id=%s\n", boot_id);
		}
		fclose(boot);
	}

	/* Sized for the transport actually in use. Both sides reach the same answer
	 * because a client only holds an ad hoc address when it joined this host's
	 * network, and the host only serves one when it is hosting ad hoc.
	 *
	 * This is a safety floor, not merely a startup guess. The shim can raise it
	 * when measured RTT requires more room, but a couple of quiet handshake
	 * probes cannot lower it: later jitter is what the window must survive. The
	 * transport selection is information the shim cannot infer on its own. */
	bool adhoc = hotspot_running || joined_hotspot;
	fprintf(f, "input_delay=%d\n", adhoc ? NS_INPUT_DELAY_ADHOC : NS_INPUT_DELAY_WIFI);
	fprintf(f, "input_delay_auto=1\n");

	/* Kept in the session format while peer executable sharing is frozen. */
	fprintf(f, "share_cores=%d\n", NS_settings()->share_cores ? 1 : 0);
	fprintf(f, "compatibility_cores=%d\n", NS_settings()->compatibility_cores ? 1 : 0);
	fprintf(f, "force_compatibility=%d\n", NS_settings()->force_compatibility ? 1 : 0);
	fprintf(f, "verbose_logs=%d\n", NS_settings()->verbose_logs ? 1 : 0);
	/* These are local wishes until the manifest exchange replaces them with the
	 * intersection of artifacts/settings available on both devices. */
	fprintf(f, "instanced_gambatte=%d\n", instanced_core_enabled("gambatte") ? 1 : 0);
	fprintf(f, "instanced_mgba=%d\n", instanced_core_enabled("mgba") ? 1 : 0);

	/* Record the ad hoc network so the launch stub can put us back on it. The
	 * join done here does not survive the app exiting - the platform brings its
	 * own WiFi back up before the game ever starts. */
	if (role == NS_ROLE_HOST && announce_ssid[0]) {
		fprintf(f, "adhoc_ssid=%s\nadhoc_psk=%s\n", announce_ssid, announce_psk);
	} else if (joined_hotspot && joined_ssid[0]) {
		fprintf(f, "adhoc_ssid=%s\nadhoc_psk=%s\n", joined_ssid, joined_psk);
	}

	if (role == NS_ROLE_HOST) {
		fprintf(f, "role=host\nport=55437\n");
		/* Host side of both link cores. Written unconditionally: an override for
		 * an option the core does not have is ignored, and the only cores that
		 * read these are the ones the shim puts in link mode anyway. */
		fprintf(f, "option.gambatte_gb_link_mode=Network Server\n");
		fprintf(f, "option.gambatte_gb_link_network_port=56400\n");
	} else {
		fprintf(f, "role=client\nport=55437\npeer=%s\n", peer_ip);
		fprintf(f, "option.gambatte_gb_link_mode=Network Client\n");
		fprintf(f, "option.gambatte_gb_link_network_port=56400\n");
		/* gambatte encodes the peer address as twelve single-digit options,
		 * three per octet, each zero-padded then atoi'd. */
		unsigned o[4] = {0, 0, 0, 0};
		sscanf(peer_ip, "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]);
		int idx = 1;
		for (int i = 0; i < 4; i++) {
			char digits[4];
			snprintf(digits, sizeof(digits), "%03u", o[i] % 1000);
			for (int d = 0; d < 3; d++)
				fprintf(f, "option.gambatte_gb_link_network_server_ip_%d=%c\n",
				        idx++, digits[d]);
		}
	}
	fclose(f);

	char cmd[1200];
	snprintf(cmd, sizeof(cmd), "cp '%s/session.conf' '%s/session'", ns_state, ns_state);
	if (system(cmd) != 0) {
		snprintf(err, errlen, "cannot activate session");
		return false;
	}

	/* Bind mounts first, launch stubs only if that fails.
	 *
	 * The mount route writes nothing to the user's Emus tree - it stages a copy
	 * of each system pak and mounts it over the original in place. The stub
	 * route creates a directory per system under Emus/<PLATFORM>/ and is kept
	 * only for devices where mounting is unavailable. bind-mount.sh clears any
	 * stubs left by a previous version before mounting, so the two can never be
	 * active at once - an SD stub would shadow the mount and wrap twice. */
	snprintf(cmd, sizeof(cmd),
	         "SDCARD_PATH='%s' PLATFORM='%s' SYSTEM_PATH='%s/.system/%s' "
	         "USERDATA_PATH='%s/.userdata/%s' "
	         "sh '%s/launcher/bind-mount.sh' up >/dev/null 2>&1",
	         ns_sd, ns_platform, ns_sd, ns_platform, ns_sd, ns_platform, ns_pak);
	if (system(cmd) != 0) {
		ns_log("bind mounts unavailable - falling back to launch stubs\n");
		/* `up` may have covered some paks before another target failed. Never
		 * combine that partial mount set with SD launch stubs: undo the entire
		 * owned mount transaction before trying the fallback route. */
		snprintf(cmd, sizeof(cmd),
		         "SDCARD_PATH='%s' PLATFORM='%s' SYSTEM_PATH='%s/.system/%s' "
		         "USERDATA_PATH='%s/.userdata/%s' "
		         "sh '%s/launcher/bind-mount.sh' down >/dev/null 2>&1",
		         ns_sd, ns_platform, ns_sd, ns_platform, ns_sd, ns_platform, ns_pak);
		system(cmd);
		snprintf(cmd, sizeof(cmd),
		         "SDCARD_PATH='%s' PLATFORM='%s' SYSTEM_PATH='%s/.system/%s' "
		         "sh '%s/launcher/install-stubs.sh' install >/dev/null 2>&1",
		         ns_sd, ns_platform, ns_sd, ns_platform, ns_pak);
		if (system(cmd) != 0) {
			snprintf(err, errlen, "could not install launch stubs");
			return false;
		}
	}

	/* The launch stub does this again per game - power save comes back when the
	 * interface reassociates - but doing it here means discovery and the
	 * handshake are not fighting the radio either. */
	NS_wifiPowerSaveDisable();
	if (NS_settings()->add_gameswitcher) {
		snprintf(cmd, sizeof(cmd),
		         "SDCARD_PATH='%s' PLATFORM='%s' NETPLAY_PAK='%s' "
		         "sh '%s/launcher/gameswitcher.sh' active >/dev/null 2>&1",
		         ns_sd, ns_platform, ns_pak, ns_pak);
		system(cmd);
	}
	NS_statusIconSync();
	return true;
}

void NS_disarm(void) {
	char cmd[1200];
	NS_brokerStop();
	NS_announceStop();
	NS_compatServeStop();
	/* Takes both routes down - mounts, the boot hook, and any legacy stubs -
	 * so "off" means off regardless of how this device was covered. */
	snprintf(cmd, sizeof(cmd),
	         "SDCARD_PATH='%s' PLATFORM='%s' SYSTEM_PATH='%s/.system/%s' "
	         "USERDATA_PATH='%s/.userdata/%s' "
	         "sh '%s/launcher/bind-mount.sh' down >/dev/null 2>&1",
	         ns_sd, ns_platform, ns_sd, ns_platform, ns_sd, ns_platform, ns_pak);
	system(cmd);

	snprintf(cmd, sizeof(cmd), "rm -f '%s/session' '%s/force-shim'",
	         ns_state, ns_state);
	system(cmd);
	snprintf(cmd, sizeof(cmd),
	         "SDCARD_PATH='%s' PLATFORM='%s' NETPLAY_PAK='%s' "
	         "sh '%s/launcher/gameswitcher.sh' idle >/dev/null 2>&1",
	         ns_sd, ns_platform, ns_pak, ns_pak);
	system(cmd);
	NS_statusIconSync();

	NS_hotspotStop();
	NS_wifiPowerSaveRestore();
}

static void wifi_client_leave_adhoc(void);

/* Drop the session but leave the launch stubs installed.
 *
 * With no session file the shim loads as a pure passthrough, so games launch
 * exactly as they would without us - the difference from NS_disarm is only
 * that the stubs stay in place, so the next session is one button press away
 * instead of a full reinstall across every Emus pak. */
void NS_endSession(void) {
	char cmd[1200];
	NS_SessionInfo session;
	bool was_client = NS_sessionInfo(&session) && session.role == NS_ROLE_CLIENT;
	NS_brokerStop();
	NS_announceStop();
	NS_compatServeStop();
	snprintf(cmd, sizeof(cmd), "rm -f '%s/session'", ns_state);
	system(cmd);
	snprintf(cmd, sizeof(cmd),
	         "SDCARD_PATH='%s' PLATFORM='%s' NETPLAY_PAK='%s' "
	         "sh '%s/launcher/gameswitcher.sh' idle >/dev/null 2>&1",
	         ns_sd, ns_platform, ns_pak, ns_pak);
	system(cmd);
	NS_statusIconSync();

	/* A session is a network arrangement as much as a file. Remember the role
	 * before removing that file: a guest must restore its station interface,
	 * but must never enter host teardown merely because an unrelated/stale
	 * hostapd process exists on the system. */
	if (was_client)
		wifi_client_leave_adhoc();
	else
		NS_hotspotStop();
	NS_wifiPowerSaveRestore();
}

/* Covered by either route: mounts (preferred) or legacy stubs. */
bool NS_stubsInstalled(void) {
	char p[512];
	snprintf(p, sizeof(p), "%s/mounts.list", ns_state);
	if (file_exists(p)) return true;
	snprintf(p, sizeof(p), "%s/stubs.list", ns_state);
	return file_exists(p);
}

//////////////////////////////////////////////////////////////////////////////
// discovery
//////////////////////////////////////////////////////////////////////////////

static long ms_since(const struct timeval* t) {
	struct timeval now;
	gettimeofday(&now, NULL);
	return (now.tv_sec - t->tv_sec) * 1000L + (now.tv_usec - t->tv_usec) / 1000L;
}

void NS_announceStart(NS_Mode mode) {
	announce_mode = mode;
	if (announce_fd >= 0) return;
	announce_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (announce_fd < 0) return;
	int on = 1;
	setsockopt(announce_fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
	memset(&last_announce, 0, sizeof(last_announce));
}

void NS_announceTick(void) {
	if (announce_fd < 0) return;
	if (last_announce.tv_sec && ms_since(&last_announce) < ANNOUNCE_MS) return;

	NS_Announce a;
	memset(&a, 0, sizeof(a));
	a.magic = htonl(DISCOVERY_MAGIC);
	a.version = htonl(1);
	a.mode = htonl((uint32_t)announce_mode);
	a.hotspot = htonl(announce_ssid[0] ? 1u : 0u);
	snprintf(a.ssid, sizeof(a.ssid), "%s", announce_ssid);
	snprintf(a.psk, sizeof(a.psk), "%s", announce_psk);
	snprintf(a.platform, sizeof(a.platform), "%s", ns_platform);

	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(DISCOVERY_PORT);
	to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	sendto(announce_fd, &a, sizeof(a), 0, (struct sockaddr*)&to, sizeof(to));

	gettimeofday(&last_announce, NULL);
}

void NS_announceStop(void) {
	if (announce_fd >= 0) { close(announce_fd); announce_fd = -1; }
}

struct scan_ctx { NS_Peer* out; int max; int n; };

static void scan_line(const char* line, void* v) {
	struct scan_ctx* c = v;
	if (c->n >= c->max || !line[0]) return;
	memset(&c->out[c->n], 0, sizeof(c->out[c->n]));
	snprintf(c->out[c->n].ssid, sizeof(c->out[c->n].ssid), "%s", line);
	snprintf(c->out[c->n].psk,  sizeof(c->out[c->n].psk),  "%s", NS_ADHOC_PSK);
	/* The host's address on its own network is fixed, so this is known before
	 * we join - which is what makes joining by SSID alone work. */
	snprintf(c->out[c->n].ip, sizeof(c->out[c->n].ip), "%s", NS_HOTSPOT_HOST_IP);
	c->out[c->n].hotspot = true;
	c->out[c->n].scanned = true;
	c->n++;
}

int NS_scanAdhoc(NS_Peer* out, int max) {
	if (!out || max <= 0) return 0;

	/* Prefer the supplicant's cached results over `iw scan`.
	 *
	 * wpa_cli scan_results returns whatever the supplicant already knows and
	 * comes back immediately; `iw dev wlan0 scan` asks the radio to go and look,
	 * which takes seconds when it works and hangs outright when it does not.
	 * The frontend polls the same way, so the cache is kept warm for us. */
	char ctrl[256];
	char cmd[600];
	struct scan_ctx c = { out, max, 0 };

	if (platform_ctrl_dir(ctrl, sizeof(ctrl))) {
		/* Ask for a refresh but do not wait on it - this returns at once. */
		snprintf(cmd, sizeof(cmd), "wpa_cli -p %s -i wlan0 scan >/dev/null 2>&1", ctrl);
		system(cmd);
		snprintf(cmd, sizeof(cmd),
		         "wpa_cli -p %s -i wlan0 scan_results 2>/dev/null "
		         "| awk -F'\t' 'NR>1 && NF>=5 {print $5}' "
		         "| grep -i '^%s-' | sort -u", ctrl, NS_ADHOC_PREFIX);
		popen_bounded(cmd, 6000, scan_line, &c);
	}

	/* The control directory now resolves to the platform's own socket even when
	 * no supplicant is running, which is right for everything else that uses it
	 * but would have quietly retired this fallback: an empty cache and a dead
	 * supplicant both read as "no networks". Asking the radio directly is the
	 * slower answer and stays the second one, not the abandoned one. */
	if (c.n == 0) {
		snprintf(cmd, sizeof(cmd),
		         "iw dev wlan0 scan 2>/dev/null | sed -n 's/^[[:space:]]*SSID: //p' "
		         "| grep -i '^%s-' | sort -u", NS_ADHOC_PREFIX);
		popen_bounded(cmd, 6000, scan_line, &c);
	}

	ns_log("scan found %d ad hoc network(s)\n", c.n);
	return c.n;
}

void NS_discoverStart(void) {
	if (discover_fd >= 0) return;
	discover_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (discover_fd < 0) return;

	int on = 1;
	setsockopt(discover_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(DISCOVERY_PORT);
	if (bind(discover_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		close(discover_fd); discover_fd = -1; return;
	}
	int flags = fcntl(discover_fd, F_GETFL, 0);
	if (flags >= 0) fcntl(discover_fd, F_SETFL, flags | O_NONBLOCK);
}

int NS_discoverTick(NS_Peer* peers, int max) {
	static int count = 0;
	if (discover_fd < 0) return count;
	if (peers == NULL) { count = 0; return 0; }

	NS_Announce a;
	struct sockaddr_in from;
	socklen_t flen = sizeof(from);

	while (recvfrom(discover_fd, &a, sizeof(a), 0, (struct sockaddr*)&from, &flen) == (ssize_t)sizeof(a)) {
		if (ntohl(a.magic) != DISCOVERY_MAGIC) continue;

		char ip[NS_IP_LEN];
		inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));

		bool known = false;
		for (int i = 0; i < count; i++)
			if (!strcmp(peers[i].ip, ip)) { known = true; break; }
		if (known || count >= max) continue;

		snprintf(peers[count].ip, NS_IP_LEN, "%s", ip);
		a.platform[sizeof(a.platform) - 1] = '\0';
		snprintf(peers[count].platform, sizeof(peers[count].platform), "%s", a.platform);
		peers[count].mode = (ntohl(a.mode) == NS_MODE_LINK) ? NS_MODE_LINK : NS_MODE_NETPLAY;
		peers[count].hotspot = ntohl(a.hotspot) != 0;
		a.ssid[sizeof(a.ssid) - 1] = '\0';
		a.psk[sizeof(a.psk) - 1]   = '\0';
		snprintf(peers[count].ssid, NS_SSID_LEN, "%s", a.ssid);
		snprintf(peers[count].psk,  NS_PSK_LEN,  "%s", a.psk);
		count++;
	}
	return count;
}

void NS_discoverStop(void) {
	/* Reset before closing. NS_discoverTick returns early when the fd is gone,
	 * so asking it to clear the count afterwards did nothing - and the stale
	 * peers[] then reappeared on the next visit to Join, offering a host that
	 * may have gone away or moved to an address that no longer routes. */
	NS_discoverTick(NULL, 0);
	if (discover_fd >= 0) { close(discover_fd); discover_fd = -1; }
}

//////////////////////////////////////////////////////////////////////////////
// ad hoc network
//
// Both devices negotiate while still on their existing network, then move
// together. The host hands wlan0 from wpa_supplicant to hostapd - these
// handhelds have one usable radio, so hosting means leaving the old network
// rather than running both at once.
//////////////////////////////////////////////////////////////////////////////

void NS_announceHotspot(const char* ssid, const char* psk) {
	snprintf(announce_ssid, sizeof(announce_ssid), "%s", ssid ? ssid : "");
	snprintf(announce_psk, sizeof(announce_psk), "%s", psk ? psk : "");
}

/* Fixed rather than generated. Generated credentials had to be advertised
 * before either side moved, which coupled ad hoc to discovery working first.
 * A known SSID and passphrase decouples them: the client can join even if it
 * never saw an announcement. The trade is that two pairs in one room collide. */
void NS_hotspotCredentials(char* ssid, int ssid_len, char* psk, int psk_len) {
	/* A short code in the SSID, so two pairs in one room do not collide and the
	 * joining device can be told which network is theirs without typing
	 * anything. Kept for the life of a session: regenerating it between the
	 * announcement and the AP coming up would leave the client looking for a
	 * network that no longer exists. */
	static char cached[NS_SSID_LEN];
	if (!cached[0]) {
		static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
		unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
		char code[5];
		for (int i = 0; i < 4; i++) {
			seed = seed * 1103515245u + 12345u;
			code[i] = alphabet[(seed >> 16) % (sizeof(alphabet) - 1)];
		}
		code[4] = '\0';
		snprintf(cached, sizeof(cached), "%s-%s", NS_ADHOC_PREFIX, code);
	}
	snprintf(ssid, ssid_len, "%s", cached);
	snprintf(psk,  psk_len,  "%s", NS_ADHOC_PSK);
}

/* The AP interface, which is not the one carrying the client connection.
 *
 * This firmware exposes a second interface dedicated to AP mode, and the radio
 * advertises `#{ managed } <= 2, #{ AP } <= 1` - so the host can serve the ad
 * hoc network while staying associated to the existing one. Pointing hostapd at
 * wlan0, as this used to, could never work while wlan0 was an associated
 * station: the interface stayed type managed and no client could associate. */
static bool ap_interface(char* out, int len) {
	/* Name and type together. Taking the first interface that simply is not
	 * wlan0 picked up hostapd's own leftover monitor vif - iw lists mon.wlan1
	 * before wlan1 - and hostapd then failed with "Could not read interface
	 * mon.wlan1 flags", after the previous working AP had already been killed. */
	FILE* p = popen("iw dev 2>/dev/null | sed -n 's/^\\s*Interface /IF /p; s/^\\s*type /TYPE /p'", "r");
	if (!p) return false;

	char line[96];
	char name[64] = "";
	bool found = false;
	while (fgets(line, sizeof(line), p) && !found) {
		char* nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';

		if (!strncmp(line, "IF ", 3)) {
			snprintf(name, sizeof(name), "%s", line + 3);
		} else if (!strncmp(line, "TYPE ", 5) && name[0]) {
			const char* type = line + 5;
			if (strcmp(name, "wlan0") != 0 &&
			    strncmp(name, "mon.", 4) != 0 &&
			    strcmp(type, "monitor") != 0) {
				snprintf(out, len, "%s", name);
				found = true;
			}
			name[0] = '\0';
		}
	}
	pclose(p);
	return found;
}

/* hostapd leaves these behind when it is killed rather than stopped, and they
 * then shadow the real interface. */
static void drop_monitor_interfaces(void) {
	system("for i in $(iw dev 2>/dev/null | sed -n 's/^\\s*Interface //p'); do "
	       "  case \"$i\" in mon.*) iw dev \"$i\" del 2>/dev/null;; esac; "
	       "done");
}

/* Pick the least congested 2.4GHz channel.
 *
 * Only usable when we are not associated: with a station up, the single-channel
 * constraint pins the AP to its channel and there is no choice to make. A scan
 * takes about two seconds on these radios and sees ~20 networks in a busy
 * environment, so this is cheap enough to do at every independent start.
 *
 * Weighted by overlap, not just exact matches - a 20MHz AP splatters over the
 * two channels either side of it. */
struct chan_ctx { int seen[15]; };

static void chan_line(const char* line, void* v) {
	struct chan_ctx* c = v;
	int freq = atoi(line);
	if (freq < 2412 || freq > 2472) return;
	int ch = (freq - 2407) / 5;
	if (ch >= 1 && ch <= 14) c->seen[ch]++;
}

static int quiet_channel(void) {
	/* Same reasoning as NS_scanAdhoc: cached results, and bounded either way.
	 * This runs from the Host screen, so a hang here freezes the UI. */
	char ctrl[256];
	char cmd[600];
	if (platform_ctrl_dir(ctrl, sizeof(ctrl))) {
		snprintf(cmd, sizeof(cmd), "wpa_cli -p %s -i wlan0 scan >/dev/null 2>&1", ctrl);
		system(cmd);
		snprintf(cmd, sizeof(cmd),
		         "wpa_cli -p %s -i wlan0 scan_results 2>/dev/null | awk -F'\t' 'NR>1 {print $2}'",
		         ctrl);
	} else {
		snprintf(cmd, sizeof(cmd), "iw dev wlan0 scan 2>/dev/null | sed -n 's/.*freq: *//p'");
	}

	struct chan_ctx c;
	memset(&c, 0, sizeof(c));
	popen_bounded(cmd, 6000, chan_line, &c);

	int best = 0, best_cost = -1;
	for (int ch = 1; ch <= 11; ch++) {
		int cost = 0;
		for (int o = ch - 2; o <= ch + 2; o++)
			if (o >= 1 && o <= 14) cost += c.seen[o];
		if (best_cost < 0 || cost < best_cost) { best_cost = cost; best = ch; }
	}
	if (best) ns_log("channel survey: picked %d (overlap score %d)\n", best, best_cost);
	return best;
}

/* Both interfaces share one radio and the driver permits a single channel, so
 * the AP has to sit on whatever channel the station is already using. A
 * hardcoded channel is rejected outright whenever it disagrees. */
static int station_channel(void) {
	FILE* p = popen("iw dev wlan0 link 2>/dev/null | sed -n 's/.*freq: *//p' | head -1", "r");
	if (!p) return 0;
	char line[32] = "";
	int freq = 0;
	if (fgets(line, sizeof(line), p)) freq = atoi(line);
	pclose(p);
	if (freq < 2412 || freq > 2484) return 0;
	return freq == 2484 ? 14 : (freq - 2407) / 5;
}

/* How the platform started its own supplicant, captured before we kill it.
 *
 * Guessing this is what broke the A30: the restore path assumed
 * /etc/wifi/wifi_init.sh, which exists on tg5040 and does not exist on my282,
 * so tearing the stack down there left the device with no network and nothing
 * to bring it back. Replaying the exact command line works on both. */
static char saved_supplicant[512];

static void wifi_client_stack_down(void) {
	saved_supplicant[0] = '\0';
	/* Match on argv[0] only. Matching anywhere in the command line also matches
	 * the shell running this very search - its own arguments contain the string
	 * - and whichever /proc entry came first would win. */
	FILE* p = popen(
		"for d in /proc/[0-9]*; do "
		"  a0=$(tr '\\0' '\\n' < $d/cmdline 2>/dev/null | head -1); "
		"  case \"$a0\" in */wpa_supplicant|wpa_supplicant) "
		"    tr '\\0' ' ' < $d/cmdline 2>/dev/null; echo; break;; "
		"  esac; "
		"done 2>/dev/null", "r");
	if (p) {
		if (fgets(saved_supplicant, sizeof(saved_supplicant), p)) {
			char* nl = strpbrk(saved_supplicant, "\r\n");
			if (nl) *nl = '\0';
		}
		pclose(p);
	}

	/* Persist it. Held only in memory, this is lost the moment the app exits or
	 * dies - which is exactly when a device is stranded on a network that is no
	 * longer there and has nothing left that knows how to get back. */
	if (saved_supplicant[0]) {
		char path[512];
		snprintf(path, sizeof(path), "%s/wifi_restore", ns_state);
		FILE* w = fopen(path, "w");
		if (w) { fprintf(w, "%s\n", saved_supplicant); fclose(w); }
	}

	/* The supplicant command line is not the whole client stack. H700 runs a
	 * `wpa_cli -a <script>` alongside it and that action script is the only
	 * thing on the device that ever starts a DHCP client; killing the
	 * supplicant takes the hook with it. Restoring one without the other is
	 * what left that platform associating forever with no address - not just
	 * for our session, but for every reassociation until the next reboot.
	 *
	 * A no-op wherever no such process exists, which is every other platform. */
	if (wifi_plat("hook-capture '%s/wifi_restore_hook'", ns_state) == 0)
		ns_log("recorded the platform's supplicant DHCP hook\n");

	system("killall -q wpa_supplicant 2>/dev/null");
	wifi_plat("dhcp-stop");
	system("ip addr flush dev wlan0 2>/dev/null");
	system("ip route flush dev wlan0 2>/dev/null");
}

/* The persisted copy, for when this process never captured one itself. */
static bool load_saved_supplicant(void) {
	if (saved_supplicant[0]) return true;

	char path[512];
	snprintf(path, sizeof(path), "%s/wifi_restore", ns_state);
	FILE* f = fopen(path, "r");
	if (!f) return false;
	if (fgets(saved_supplicant, sizeof(saved_supplicant), f)) {
		char* nl = strpbrk(saved_supplicant, "\r\n");
		if (nl) *nl = '\0';
	}
	fclose(f);
	return saved_supplicant[0] != '\0';
}

/* How long to wait for the client stack to come back, in wall-clock seconds.
 *
 * One deadline, not one per step. Splitting it into an association budget and a
 * DHCP budget was wrong twice over: if association used its whole 8s the DHCP
 * phase then ran anyway against a link that did not exist, burning its budget to
 * reach a foregone conclusion - and a device that associated at 9s was declared
 * failed with 8 unused seconds sitting in the next phase.
 *
 * 25s because that is what this hardware needs. Measured on the my282: after a
 * supplicant restart the watchdog still saw "not associated" at its 20s check
 * and was associated by the next one. The original code appeared to work only
 * because a blocking `udhcpc -t 8` sat there for ~24s and gave association that
 * long to finish by accident; shortening it to 8s removed the accident and
 * exposed the real requirement. Spent animating, not frozen. */
#define RESTORE_TOTAL_S 25

/* Re-ask for a lease if one has not arrived this long after associating. The
 * first request can go out during the 4-way handshake and simply be dropped. */
#define RESTORE_DHCP_RETRY_S 8

/* A30 occasionally accepts the captured command but loses the first daemon
 * during the radio transition. One clean retry inside the existing deadline
 * is much faster than handing the same command to the watchdog half a minute
 * later (the failure observed in the field). */
#define RESTORE_SUPPLICANT_RETRY_S 10
#define WIFI_RESTORE_LOCK "/tmp/netplay_wifi_restore.lock"

static bool wifi_link_up(void) {
	return wifi_associated(NULL);
}

static bool wifi_has_ip(void) {
	return system("ip -4 addr show wlan0 2>/dev/null | grep -q 'inet '") == 0;
}

static bool supplicant_running(void) {
	return system("pidof wpa_supplicant >/dev/null 2>&1") == 0;
}

static bool wifi_restore_lock_acquire(void) {
	if (mkdir(WIFI_RESTORE_LOCK, 0700) == 0) {
		char path[128];
		snprintf(path, sizeof(path), "%s/pid", WIFI_RESTORE_LOCK);
		FILE* f = fopen(path, "w");
		if (f) { fprintf(f, "%d\n", getpid()); fclose(f); }
		return true;
	}

	/* Do not kill or wait behind the watchdog: the retained recovery record
	 * means this caller can safely stand down while the current owner finishes.
	 * Recover only an unmistakably stale directory left by a crashed owner. */
	char path[128];
	snprintf(path, sizeof(path), "%s/pid", WIFI_RESTORE_LOCK);
	FILE* f = fopen(path, "r");
	int pid = 0;
	bool live = f && fscanf(f, "%d", &pid) == 1 && pid > 0;
	if (f) fclose(f);
	if (live) {
		char proc[64];
		snprintf(proc, sizeof(proc), "/proc/%d", pid);
		live = file_exists(proc);
	}
	if (live) {
		ns_log("wifi restoration already owned by pid %d\n", pid);
		return false;
	}
	unlink(path);
	if (rmdir(WIFI_RESTORE_LOCK) != 0 || mkdir(WIFI_RESTORE_LOCK, 0700) != 0)
		return false;
	snprintf(path, sizeof(path), "%s/pid", WIFI_RESTORE_LOCK);
	f = fopen(path, "w");
	if (f) { fprintf(f, "%d\n", getpid()); fclose(f); }
	return true;
}

static void wifi_restore_lock_release(void) {
	char path[128];
	snprintf(path, sizeof(path), "%s/pid", WIFI_RESTORE_LOCK);
	unlink(path);
	rmdir(WIFI_RESTORE_LOCK);
}

/* Put back whatever was running before, preferring the platform's own script
 * when it has one and falling back to the captured command line.
 *
 * Returns whether wlan0 ended up with an address. Callers used to get no signal
 * at all and reported success unconditionally, so the UI could claim it was
 * connected while the radio was still associating. */
static bool wifi_client_stack_up(void) {
	if (!wifi_restore_lock_acquire()) return false;

	system("killall -q wpa_supplicant 2>/dev/null");
	wifi_plat("dhcp-stop");
	/* Do not race a daemon which has accepted SIGTERM but has not released its
	 * control socket yet. The A30 log showed precisely that first-start loss. */
	for (int i = 0; i < 10 && supplicant_running(); i++) usleep(100000);
	system("ip addr flush dev wlan0 2>/dev/null");
	system("ip route flush dev wlan0 2>/dev/null");
	system("ip link set wlan0 up 2>/dev/null");

	bool can_retry_saved = false;
	bool used_saved = false;
	char cmd[600] = "";
	/* Prefer the exact daemon command captured before joining ad hoc. On the
	 * Brick, wifi_init.sh stop/start performs rfkill and service teardown after
	 * we have already stopped/flushed the client stack; that redundant path was
	 * measured blocking the UI for 10.1s before restoration even began. */
	if (load_saved_supplicant()) {
		/* Captured service command lines are not guaranteed to contain -B. A
		 * foreground wpa_supplicant here blocks the UI forever; explicitly make
		 * the replay daemonise while preserving every platform-specific option. */
		bool already_backgrounds = strstr(saved_supplicant, " -B") != NULL;
		snprintf(cmd, sizeof(cmd), "%s%s >/dev/null 2>&1", saved_supplicant,
		         already_backgrounds ? "" : " -B");
		system(cmd);
		wifi_plat("hook-replay '%s/wifi_restore_hook'", ns_state);
		can_retry_saved = true;
		used_saved = true;
		ns_log("started saved supplicant command\n");
	} else if (wifi_plat("wifi-init start") == 0) {
		/* The stack was already stopped above; start is sufficient and avoids
		 * a second service/rfkill teardown.
		 *
		 * The path is resolved per platform now. Hardcoding /etc/wifi here was
		 * correct only on tg50xx; my282, my355 and h700 all keep their script
		 * under SYSTEM_PATH, so this entire recovery route was dead code on
		 * three of the five platforms that reach it. */
		ns_log("started wifi restoration via the platform wifi_init.sh\n");
	} else {
		ns_log("WARNING: nothing recorded to restore wifi with\n");
		wifi_restore_lock_release();
		return false;
	}

	/* One loop to the deadline. What we are waiting for is an address; whether
	 * the time goes on associating or on DHCP is not something to budget for
	 * separately.
	 *
	 * DHCP runs backgrounded and is polled, because the answer is observable
	 * independently of udhcpc's exit. Blocking on `udhcpc -t 8` cost up to ~24s
	 * of frozen UI: busybox pauses -T seconds between discover packets and
	 * defaults to 3, so the retry count multiplies straight into wall clock. */
	bool assoc = false, got_ip = false;
	int dhcp_at = -1;

	for (int i = 0; i < RESTORE_TOTAL_S && !got_ip; i++) {
		if (ns_progress)
			ns_progress(assoc ? "Getting original WiFi address"
			                  : "Reconnecting to original WiFi",
			            i + 1, RESTORE_TOTAL_S);
		sleep(1);

		/* Checked every second regardless of what the association probe thinks.
		 *
		 * This used to sit behind the `continue` below, so an address could
		 * only ever be noticed on a second where we had already decided we were
		 * associated. On a platform whose DHCP is driven by the supplicant's
		 * own hook the address can arrive without this loop doing anything at
		 * all - and on one where the association probe is simply unavailable it
		 * never became observable, so a restore that had in fact succeeded was
		 * reported as a failure and the recovery breadcrumb was kept forever.
		 * The answer we actually want is "does wlan0 have an address"; asking
		 * it directly costs one `ip` call a second. */
		got_ip = wifi_has_ip();
		if (got_ip) break;

		if (!assoc) {
			assoc = wifi_link_up();
			if (!assoc) {
				if (can_retry_saved && i + 1 == RESTORE_SUPPLICANT_RETRY_S) {
					ns_log("still unassociated after %ds - restarting saved supplicant\n",
					       i + 1);
					system("killall -q wpa_supplicant 2>/dev/null");
					for (int n = 0; n < 10 && supplicant_running(); n++) usleep(100000);
					system(cmd);
					wifi_plat("hook-replay '%s/wifi_restore_hook'", ns_state);
				}
				continue;
			}
			ns_log("re-associated after %ds\n", i + 1);
		}

		/* Ask on the first second we are associated, and again if nothing has
		 * arrived - never leaving a request unretried inside the deadline.
		 * Persistent, because we are handing the radio back to the user and
		 * will not be here when the lease expires. */
		if (dhcp_at < 0 || i - dhcp_at >= RESTORE_DHCP_RETRY_S) {
			wifi_plat("dhcp-stop");
			wifi_plat("dhcp-start persistent");
			dhcp_at = i;
		}
	}

	if (got_ip) {
		ns_log("restored wifi via %s\n",
		       used_saved ? "saved supplicant command" : "platform startup script");
		wifi_restore_lock_release();
		return true;
	}

	/* Out of time, but not necessarily failed - association may still land a few
	 * seconds from now. Leave udhcpc running rather than killing the one process
	 * that would finish the job, and report honestly so the caller does not draw
	 * a connected state that is not there yet. The restore record stays put, so
	 * the watchdog and the next app launch both still know to finish this. */
	ns_log("wifi not back after %ds (associated=%d) - leaving dhcp running\n",
	       RESTORE_TOTAL_S, assoc);
	wifi_restore_lock_release();
	return false;
}

/* Is a watchdog holding this session? Its pidfile is the shared token. */
static bool watchdog_running(void) {
	FILE* f = fopen("/tmp/netplay_watchdog.pid", "r");
	if (!f) return false;
	int pid = 0;
	bool ok = fscanf(f, "%d", &pid) == 1 && pid > 0;
	fclose(f);
	if (!ok) return false;

	char proc[64];
	snprintf(proc, sizeof(proc), "/proc/%d", pid);
	return file_exists(proc);
}

/* Recover a device left on a network that is gone.
 *
 * Two ways in: a join that failed partway, and a join that worked whose host
 * then tore its AP down. Both end with the radio associated to nothing, and
 * neither used to leave anything behind that knew how to undo it. */
NS_Recovery NS_wifiRecoverIfStranded(void) {
	char path[512];
	snprintf(path, sizeof(path), "%s/wifi_restore", ns_state);
	if (!file_exists(path)) return NS_RECOVERY_NONE;   /* never moved the stack */

	bool connected = wifi_associated(NULL);
	bool has_ip    = wifi_has_ip();

	char cur_ssid[NS_SSID_LEN + 40] = "";
	wifi_plat_out(cur_ssid, sizeof(cur_ssid), "assoc-ssid");
	bool on_adhoc = !strncmp(cur_ssid, NS_ADHOC_PREFIX "-", sizeof(NS_ADHOC_PREFIX));

	/* Healthy on some other network: nothing to do, and the record is stale.
	 *
	 * Unless a watchdog is mid-count. It is the record's other reader, and
	 * removing it underneath makes it stand down believing the session ended -
	 * which is exactly what happened in testing, aborting a recovery at 2 of 3
	 * strikes. Whoever finishes gets to clean up. */
	if (connected && has_ip && !on_adhoc) {
		if (!watchdog_running()) remove(path);
		return NS_RECOVERY_NONE;
	}

	/* Still usefully on the ad hoc network with a session armed - leave it. */
	if (on_adhoc && has_ip && NS_isArmed()) return NS_RECOVERY_NONE;

	ns_log("stranded (connected=%d ip=%d adhoc=%d) - restoring wifi\n",
	       connected, has_ip, on_adhoc);
	joined_hotspot = false;
	/* Only drop the breadcrumb once we are actually back. Removing it on a
	 * restore that timed out deletes the one record the watchdog and the next
	 * app launch use to know a restore is still owed - stranding the device for
	 * good, which is the exact failure this whole mechanism exists to stop. */
	bool back = wifi_client_stack_up();
	if (back) {
		remove(path);
		char hook[512];
		snprintf(hook, sizeof(hook), "%s/wifi_restore_hook", ns_state);
		remove(hook);
	}
	return back ? NS_RECOVERY_DONE : NS_RECOVERY_FAILED;
}

/* Are we currently sitting on one of our own ad hoc networks?
 *
 * Observable, unlike joined_hotspot, which is process-local. The normal flow is
 * join -> leave the app to play -> come back later to end the session, and by
 * then joined_hotspot is false in a fresh process - so gating the client-side
 * restore on it meant ending a session left the device on a network that was
 * about to be torn down. */
static bool on_own_adhoc(void) {
	char ssid[NS_SSID_LEN + 40] = "";
	wifi_plat_out(ssid, sizeof(ssid), "assoc-ssid");
	return !strncmp(ssid, NS_ADHOC_PREFIX "-", sizeof(NS_ADHOC_PREFIX));
}

static void wifi_client_leave_adhoc(void) {
	if (!joined_hotspot && !on_own_adhoc()) return;
	ns_log("leaving ad hoc network, restoring wifi\n");
	wifi_client_stack_up();
	joined_hotspot = false;
}

/* An in-process flag is not enough: the app can be reopened after a crash or a
 * quit that skipped teardown, and a hotspot left running still has to be
 * cleanable. */
static bool hotspot_processes_alive(void) {
	return system("pidof hostapd >/dev/null 2>&1") == 0
	    || system("pidof udhcpd  >/dev/null 2>&1") == 0;
}

static bool active_hotspot_interface(char* out, int len) {
	out[0] = '\0';
	if (ns_ap_if[0]) {
		snprintf(out, len, "%s", ns_ap_if);
		return true;
	}
	if (!hotspot_processes_alive()) return false;

	FILE* conf = fopen("/tmp/netplay_hostapd.conf", "r");
	char line[128];
	while (conf && fgets(line, sizeof(line), conf)) {
		if (sscanf(line, "interface=%63s", out) == 1) break;
	}
	if (conf) fclose(conf);
	return out[0] != '\0';
}

bool NS_hotspotStart(const char* ssid, const char* psk, char* err, int errlen) {
	/* Already serving this network? Leave it alone.
	 *
	 * Tearing it down and rebuilding it is what stranded a client that had
	 * already joined: the old AP was killed, the restart then failed, and the
	 * peer was left associated to a network that no longer existed with no way
	 * back. Restarting is only worth the risk if something is actually wrong. */
	char cur[128];
	if (hotspot_running && ns_ap_if[0]
	    && system("pidof hostapd >/dev/null 2>&1") == 0) {
		snprintf(cur, sizeof(cur),
		         "iw dev %s info 2>/dev/null | grep -q 'ssid %s'", ns_ap_if, ssid);
		if (system(cur) == 0) {
			ns_log("AP already up on %s as %s - keeping it\n", ns_ap_if, ssid);
			return true;
		}
	}

	drop_monitor_interfaces();

	char apif[64];
	if (!ap_interface(apif, sizeof(apif))) {
		snprintf(err, errlen, "no AP interface on this device - it cannot host ad hoc");
		return false;
	}
	if (system("command -v udhcpd >/dev/null 2>&1") != 0) {
		/* my282 ships no udhcpd. Without it a client associates and then sits
		 * with no address, which used to read as a hang. Say so instead. */
		snprintf(err, errlen, "no udhcpd on this device - it cannot host ad hoc");
		return false;
	}

	/* Follow the station if there is one - the radio permits a single channel.
	 * Free of that constraint, survey and take the quietest. */
	int ch = station_channel();
	if (!ch) ch = quiet_channel();
	if (!ch) ch = 1;

	FILE* f = fopen("/tmp/netplay_hostapd.conf", "w");
	if (!f) { snprintf(err, errlen, "cannot write hostapd config"); return false; }
	fprintf(f,
		"interface=%s\ndriver=nl80211\nssid=%s\nhw_mode=g\nchannel=%d\n"
		"auth_algs=1\nwpa=2\nwpa_passphrase=%s\nwpa_key_mgmt=WPA-PSK\n"
		"rsn_pairwise=CCMP\nctrl_interface=/var/run/hostapd\n",
		apif, ssid, ch, psk);
	fclose(f);

	f = fopen("/tmp/netplay_udhcpd.conf", "w");
	if (!f) { snprintf(err, errlen, "cannot write dhcp config"); return false; }
	fprintf(f,
		"start 10.0.0.20\nend 10.0.0.40\ninterface %s\n"
		"lease_file /tmp/netplay_udhcpd.leases\npidfile /tmp/netplay_udhcpd.pid\n"
		"option subnet 255.255.255.0\noption lease 3600\n", apif);
	fclose(f);
	system("touch /tmp/netplay_udhcpd.leases");

	/* Same graceful shutdown as the teardown path - see NS_hotspotStop. A
	 * SIGKILL here with a client still associated can wedge the radio. */
	system("killall -q hostapd 2>/dev/null");
	for (int i = 0; i < 10; i++) {
		if (system("pidof hostapd >/dev/null 2>&1") != 0) break;
		usleep(500 * 1000);
	}
	system("killall -q -9 hostapd 2>/dev/null");
	system("killall -q udhcpd 2>/dev/null");
	system("killall -q -9 udhcpd 2>/dev/null");
	sleep(1);

	/* Reset the interface before handing it to hostapd.
	 *
	 * Killing hostapd does not undo what it did: the interface is left type AP
	 * with the SSID still set and the channel still claimed, and a later hostapd
	 * then fails with "Could not set channel for kernel driver". This only shows
	 * up on the second session - the first works because the interface is clean
	 * - which is exactly how it looked like ad hoc had regressed.
	 *
	 * Note what is *not* here: the client stack is left alone. The host stays on
	 * the existing network, so discovery keeps working and only one device has
	 * to move. */
	char cmd[512];
	drop_monitor_interfaces();
	snprintf(cmd, sizeof(cmd), "ip link set %s down 2>/dev/null", apif);      system(cmd);
	snprintf(cmd, sizeof(cmd), "ip addr flush dev %s 2>/dev/null", apif);     system(cmd);
	snprintf(cmd, sizeof(cmd), "iw dev %s set type managed 2>/dev/null", apif); system(cmd);
	sleep(1);

	/* Not -B. Its exit status only says it forked, which it does even when the
	 * channel is rejected - the old "hostapd would not start" branch could never
	 * fire. Run it in the background and wait for it to say AP-ENABLED. */
	ns_log("starting AP on %s channel %d (station is on %d)\n", apif, ch, station_channel());
	system("hostapd /tmp/netplay_hostapd.conf > /tmp/netplay_hostapd.log 2>&1 &");

	bool up = false;
	for (int i = 0; i < 12 && !up; i++) {
		sleep(1);
		if (system("pidof hostapd >/dev/null 2>&1") != 0) break;
		up = system("grep -q AP-ENABLED /tmp/netplay_hostapd.log 2>/dev/null") == 0;
	}
	if (!up) {
		snprintf(err, errlen, "AP would not start on %s ch%d - see netplay_hostapd.log", apif, ch);
		ns_log("%s\n", err);
		system("sed 's/^/[netplay-app]   hostapd: /' /tmp/netplay_hostapd.log 1>&2 2>/dev/null");
		system("killall -q hostapd 2>/dev/null");
		return false;
	}

	/* hostapd owns the interface now, so the address goes on after it is up. */
	snprintf(cmd, sizeof(cmd), "ip addr flush dev %s 2>/dev/null", apif); system(cmd);
	snprintf(cmd, sizeof(cmd), "ip addr add " NS_HOTSPOT_HOST_IP "/24 dev %s 2>/dev/null", apif);
	system(cmd);

	system("udhcpd /tmp/netplay_udhcpd.conf >/dev/null 2>&1");
	if (system("pidof udhcpd >/dev/null 2>&1") != 0) {
		snprintf(err, errlen, "AP up but udhcpd would not run - clients get no address");
		ns_log("%s\n", err);
		system("killall -q hostapd 2>/dev/null");
		return false;
	}
	ns_log("AP up on %s as %s, serving %s\n", apif, ssid, NS_HOTSPOT_HOST_IP);

	snprintf(ns_ap_if, sizeof(ns_ap_if), "%s", apif);
	hotspot_running = true;
	return true;
}

/* Where the platform's own supplicant listens.
 *
 * The frontend's WiFi status - the signal icon, the SSID, everything
 * PLAT_wifiConnection reports - is read with `wpa_cli -p <dir>`. Replacing the
 * supplicant with one on a different control socket leaves that layer blind: it
 * reports no connection and the icon disappears, while the radio is in fact
 * associated and passing traffic. Reusing the platform's directory keeps the
 * frontend able to see us, in game as well as in this app.
 *
 * Taken from -O on the running command line if present (tg5040), otherwise from
 * ctrl_interface= in the config it was given (my282). */
static bool platform_ctrl_dir(char* out, int len) {
	wifi_plat_out(out, len, "ctrl-dir");
	return out[0] != '\0';
}

bool NS_hotspotJoin(const char* ssid, const char* psk, char* err, int errlen) {
	char ctrl[256];
	if (!platform_ctrl_dir(ctrl, sizeof(ctrl))) {
		snprintf(ctrl, sizeof(ctrl), "/var/run/wpa_supplicant");
		ns_log("no platform ctrl_interface found - frontend wifi status will be blank\n");
	} else {
		ns_log("joining %s, reusing ctrl_interface %s\n", ssid, ctrl);
	}

	FILE* f = fopen("/tmp/netplay_wpa.conf", "w");
	if (!f) { snprintf(err, errlen, "cannot write supplicant config"); return false; }
	fprintf(f,
		"ctrl_interface=%s\nupdate_config=1\n"
		"network={\n\tssid=\"%s\"\n\tpsk=\"%s\"\n\tkey_mgmt=WPA-PSK\n"
		"\tpriority=99\n}\n", ctrl, ssid, psk);
	fclose(f);

	/* Capture how the platform runs its supplicant before killing it - that is
	 * what puts the existing network back afterwards. */
	wifi_client_stack_down();
	joined_hotspot = true;   /* set before the first failure path can return */

	for (int attempt = 1; attempt <= NS_JOIN_ATTEMPTS; attempt++) {
		system("ip link set wlan0 up 2>/dev/null");
		system("wpa_supplicant -B -D nl80211 -i wlan0 -c /tmp/netplay_wpa.conf >/dev/null 2>&1");

		/* Bounded per attempt: five unbounded tries is minutes of black screen. */
		bool assoc = false;
		for (int i = 0; i < NS_JOIN_ASSOC_S && !assoc; i++) {
			if (ns_progress) ns_progress("Connecting", attempt, NS_JOIN_ATTEMPTS);
			sleep(1);
			assoc = wifi_associated(ssid);
		}

		if (assoc) {
			/* iw reports the SSID as soon as it associates, which is before the
			 * 4-way handshake finishes - a DHCP request sent in that gap is
			 * simply dropped. */
			if (ns_progress) ns_progress("Getting an address", attempt, NS_JOIN_ATTEMPTS);
			sleep(1);
			/* Poll for the address rather than trusting the client's exit
			 * status: a platform whose DHCP is driven by the supplicant hook
			 * has no exit status to read, and a client that is simply not
			 * installed returns 127, which is not "no lease". */
			for (int j = 0; j < 2; j++) {
				wifi_plat("dhcp-start");
				bool leased = false;
				for (int w = 0; w < NS_JOIN_DHCP_S && !leased; w++) {
					if (ns_progress)
						ns_progress("Getting an address", attempt, NS_JOIN_ATTEMPTS);
					sleep(1);
					leased = wifi_has_ip();
				}
				if (leased) {
					snprintf(joined_ssid, sizeof(joined_ssid), "%s", ssid);
					snprintf(joined_psk,  sizeof(joined_psk),  "%s", psk);

					/* Guard the window where stranding is possible: from here
					 * until the session ends. Detached, so it outlives this app
					 * - which is the point, since the stranding shows up while
					 * you are somewhere else entirely. */
					char wd[700];
					snprintf(wd, sizeof(wd),
					         "if command -v start-stop-daemon >/dev/null 2>&1; then "
					         "  start-stop-daemon -S -b -m -p /tmp/netplay_watchdog_ssd.pid "
					         "    -x /bin/sh -- "
					         "    '%s/launcher/wifi-watchdog.sh' '%s'; "
					         "else ( sh '%s/launcher/wifi-watchdog.sh' '%s' </dev/null & ); fi",
					         ns_pak, ns_pak, ns_pak, ns_pak);
					system(wd);
					ns_log("started wifi watchdog\n");
					return true;
				}
			}
			snprintf(err, errlen, "joined %s but no address (attempt %d)", ssid, attempt);
		} else {
			snprintf(err, errlen, "could not find %s (attempt %d)", ssid, attempt);
		}

		system("killall -q wpa_supplicant 2>/dev/null");
		if (attempt < NS_JOIN_ATTEMPTS) {
			for (int i = 0; i < NS_JOIN_GAP_S; i++) {
				if (ns_progress) ns_progress("Retrying", attempt + 1, NS_JOIN_ATTEMPTS);
				sleep(1);
			}
		}
	}

	/* Never leave the radio stranded on a network that did not work. */
	NS_hotspotStop();
	return false;
}

void NS_hotspotStop(void) {
	announce_ssid[0] = '\0';
	announce_psk[0]  = '\0';
	/* Recover the AP interface before stopping its owner. A reopened app has no
	 * process-local ns_ap_if, but still must return the interface to managed
	 * mode when ending a broker-owned session. */
	char recovered_ap_if[64];
	if (!ns_ap_if[0] && active_hotspot_interface(recovered_ap_if, sizeof(recovered_ap_if)))
		snprintf(ns_ap_if, sizeof(ns_ap_if), "%s", recovered_ap_if);

	/* Host and client undo different things, and doing the wrong one is how
	 * Turn off used to take a device off WiFi with no way back. */
	if (hotspot_running || hotspot_processes_alive()) {
		/* Let hostapd shut down properly. This is not politeness.
		 *
		 * SIGTERM makes hostapd deauthenticate its stations and hand the
		 * interface back in an orderly way. Sending SIGKILL straight after it -
		 * which is what this used to do, with no wait in between - kills it
		 * before any of that happens, leaving the driver holding an AP with a
		 * station still associated. On a single-radio device that also carries
		 * the station interface, that wedged the whole radio: the Brick froze
		 * hard mid-session and took several reboots and manual WiFi toggling to
		 * recover. Ending a session must never be able to do that.
		 *
		 * udhcpd genuinely does ignore SIGTERM on these images, so it still
		 * needs escalating - but separately, and only it. */
		system("killall -q hostapd 2>/dev/null");
		for (int i = 0; i < 10; i++) {          /* up to ~5s for a clean exit */
			if (system("pidof hostapd >/dev/null 2>&1") != 0) break;
			usleep(500 * 1000);
		}
		if (system("pidof hostapd >/dev/null 2>&1") == 0) {
			ns_log("hostapd did not exit on SIGTERM - escalating\n");
			system("killall -q -9 hostapd 2>/dev/null");
			sleep(1);
		}

		system("killall -q udhcpd 2>/dev/null");
		system("killall -q -9 udhcpd 2>/dev/null");
		/* Put the interface back to a state the next hostapd can configure.
		 * Leaving it type AP with a channel claimed is what made the second
		 * session fail where the first succeeded. */
		drop_monitor_interfaces();
		if (ns_ap_if[0]) {
			char cmd[256];
			snprintf(cmd, sizeof(cmd), "ip addr flush dev %s 2>/dev/null", ns_ap_if);   system(cmd);
			snprintf(cmd, sizeof(cmd), "ip link set %s down 2>/dev/null", ns_ap_if);    system(cmd);
			snprintf(cmd, sizeof(cmd), "iw dev %s set type managed 2>/dev/null", ns_ap_if); system(cmd);
		}
		ns_log("AP stopped\n");
		hotspot_running = false;
		ns_ap_if[0] = '\0';
		/* The host never took the client stack down, so there is nothing to
		 * restore here. */
	}

	/* Restore the client stack if we moved it - judged by what the radio is
	 * actually associated to, not by a flag this process may not have set. */
	wifi_client_leave_adhoc();
}

bool NS_hotspotActive(void) { return hotspot_running || joined_hotspot; }

/* Can this device serve a network at all?
 *
 * Asked before offering the choice, so a device that cannot do it never shows
 * an option that would fail. Platforms differ here and the answer is not
 * knowable in advance - probe rather than assume. */
bool NS_canHostAdhoc(void) {
	/* Cached: this is called from render, several times per frame, and the
	 * uncached version spawns a shell running `iw dev` each time. The set of
	 * wireless interfaces does not change under us during a session - hostapd
	 * adds and removes a monitor vif, which ap_interface already ignores. */
	static int cached = -1;
	if (cached < 0) {
		char apif[64];
		cached = ap_interface(apif, sizeof(apif)) ? 1 : 0;
		ns_log("ad hoc hosting: %s\n", cached ? "supported" : "no AP-capable interface");
	}
	return cached != 0;
}

void NS_setProgressCallback(NS_ProgressFn fn) { ns_progress = fn; }

void NS_wifiRestore(void) {
	char path[512];
	snprintf(path, sizeof(path), "%s/wifi_restore", ns_state);

	/* Stop serving too - on one radio, an AP left running is part of why the
	 * client stack cannot settle. */
	if (hotspot_running || hotspot_processes_alive()) NS_hotspotStop();

	joined_hotspot = false;
	if (wifi_client_stack_up()) {              /* see NS_wifiRecoverIfStranded */
		remove(path);
		char hook[512];
		snprintf(hook, sizeof(hook), "%s/wifi_restore_hook", ns_state);
		remove(hook);
	}
}

/* Who is actually on our network.
 *
 * Associated does not mean reachable - a station can hold the radio link and
 * still have no address - so this reports the address where one is known and
 * says so plainly when it is not, rather than implying a working peer. */
int NS_hotspotClients(NS_Client* out, int max) {
	if (!out || max <= 0) return 0;

	/* A broker or newly opened setup app has no process-local hotspot flags.
	 * Reconstruct the interface from the config owned by the still-live hostapd
	 * instead of making every post-launch guest table look empty. */
	char apif[64] = "";
	active_hotspot_interface(apif, sizeof(apif));
	if (!apif[0]) return 0;

	char cmd[256];
	snprintf(cmd, sizeof(cmd),
	         "iw dev %s station dump 2>/dev/null | sed -n 's/^Station \\([0-9a-f:]*\\).*/\\1/p'",
	         apif);
	FILE* p = popen(cmd, "r");
	if (!p) return 0;

	int n = 0;
	char mac[32];
	while (n < max && fgets(mac, sizeof(mac), p)) {
		char* nl = strpbrk(mac, "\r\n");
		if (nl) *nl = '\0';
		if (!mac[0]) continue;

		snprintf(out[n].mac, sizeof(out[n].mac), "%s", mac);
		out[n].ip[0] = '\0';

		/* The lease lands in the kernel's neighbour table once the client
		 * actually talks to us, which is the thing worth showing. */
		char look[256];
		snprintf(look, sizeof(look),
		         "awk 'tolower($4)==\"%s\" && $6==\"%s\" {print $1; exit}' /proc/net/arp 2>/dev/null",
		         mac, apif);
		FILE* a = popen(look, "r");
		if (a) {
			if (fgets(out[n].ip, sizeof(out[n].ip), a)) {
				char* n2 = strpbrk(out[n].ip, "\r\n");
				if (n2) *n2 = '\0';
			}
			pclose(a);
		}
		n++;
	}
	pclose(p);
	return n;
}
