/*
 * Netplay quick menu - START in NextUI's menus opens a small Netplay menu
 * drawn over NextUI by Varnish.pak, so hosting does not mean leaving the menu
 * you are in to open the Netplay app.
 *
 * Varnish only draws. This process reads the buttons itself, straight from
 * evdev, and holds Varnish's input grab while the menu is open so NextUI does
 * not also scroll or launch underneath it. START is NextUI's one unused button
 * in its menus, and the menu only opens while NextUI itself is in front - not
 * over a game or another pak.
 *
 * Runs detached for as long as the setting is on (launcher/quick-menu.sh
 * starts and stops it). Hosting is the same sequence the app runs; joining
 * still needs the app, which the menu says.
 */

#include "netsetup.h"
#include "varnish.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>

/* Older toolchains' kernel headers (my282's) predate these names. */
#ifndef BTN_SOUTH
#define BTN_SOUTH     0x130
#define BTN_EAST      0x131
#endif
#ifndef BTN_DPAD_UP
#define BTN_DPAD_UP   0x220
#define BTN_DPAD_DOWN 0x221
#endif
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define CLIENT        "netplay-menu"
#define NOTICE        "netplay-note"
#define MAX_DEVICES   12
#define IDLE_CLOSE_S  60     /* an untouched menu closes itself */
#define NOTICE_S      4

typedef enum {
	QB_NONE, QB_UP, QB_DOWN, QB_A, QB_B, QB_START, QB_OTHER
} Button;

static volatile sig_atomic_t stopping;
static void on_signal(int sig) { (void)sig; stopping = 1; }

/* ── input ─────────────────────────────────────────────────────────── */

static int fds[MAX_DEVICES];
static int fd_count;
static unsigned char key_held[KEY_CNT];
static int hat_x, hat_y;
/* Which codes each platform's pad reports, as NextUI's platform.h maps them. */
static enum { LAYOUT_GAMEPAD, LAYOUT_KEYBOARD, LAYOUT_H700 } layout;

static void close_devices(void) {
	for (int i = 0; i < fd_count; i++) close(fds[i]);
	fd_count = 0;
}

/* Every device with keys: the built-in pad, and DevBridge's virtual pad when
 * it is attached. Reopened now and then, so a pad that appears later counts. */
static void open_devices(void) {
	close_devices();
	for (int i = 0; i < 32 && fd_count < MAX_DEVICES; i++) {
		char path[32];
		unsigned long evbits = 0;
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) continue;
		if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0 || !(evbits & (1UL << EV_KEY))) {
			close(fd);
			continue;
		}
		fds[fd_count++] = fd;
	}
	memset(key_held, 0, sizeof(key_held));
	hat_x = hat_y = 0;
}

static bool any_held(void) {
	if (hat_x || hat_y) return true;
	for (int k = 0; k < KEY_CNT; k++)
		if (key_held[k] && k != KEY_POWER) return true;
	return false;
}

static Button key_button(int code) {
	if (layout == LAYOUT_KEYBOARD) {   /* my355: keyboard keys */
		switch (code) {
		case KEY_UP:        return QB_UP;
		case KEY_DOWN:      return QB_DOWN;
		case KEY_SPACE:     return QB_A;
		case KEY_LEFTCTRL:  return QB_B;
		case KEY_ENTER:     return QB_START;
		default:            return QB_OTHER;
		}
	}
	if (layout == LAYOUT_H700) {       /* south is A, the d-pad keyboard keys */
		switch (code) {
		case 304:           return QB_A;
		case 305:           return QB_B;
		case 311:           return QB_START;
		case KEY_UP:        return QB_UP;
		case KEY_DOWN:      return QB_DOWN;
		default:            return QB_OTHER;
		}
	}
	/* NextUI's SDL layout for these pads: east is A, south is B. */
	switch (code) {
	case BTN_EAST:      return QB_A;
	case BTN_SOUTH:     return QB_B;
	case BTN_START:     return QB_START;
	case BTN_DPAD_UP:   return QB_UP;
	case BTN_DPAD_DOWN: return QB_DOWN;
	default:            return QB_OTHER;
	}
}

/* Reads everything pending and returns the first press, if any. Releases and
 * repeats only update what is held. */
static Button read_press(void) {
	Button first = QB_NONE;
	for (int i = 0; i < fd_count; i++) {
		struct input_event ev;
		while (read(fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
			Button b = QB_NONE;
			if (ev.type == EV_KEY && ev.code < KEY_CNT) {
				if (ev.value == 2) continue;
				key_held[ev.code] = ev.value != 0;
				if (ev.value == 1 && ev.code != KEY_POWER) b = key_button(ev.code);
			} else if (ev.type == EV_ABS && ev.code == ABS_HAT0Y) {
				if (ev.value && !hat_y) b = ev.value < 0 ? QB_UP : QB_DOWN;
				hat_y = ev.value;
			} else if (ev.type == EV_ABS && ev.code == ABS_HAT0X) {
				if (ev.value && !hat_x) b = QB_OTHER;
				hat_x = ev.value;
			}
			if (b != QB_NONE && first == QB_NONE) first = b;
		}
	}
	return first;
}

static void wait_input(int timeout_ms) {
	struct pollfd p[MAX_DEVICES];
	for (int i = 0; i < fd_count; i++) {
		p[i].fd = fds[i];
		p[i].events = POLLIN;
	}
	poll(p, fd_count, timeout_ms);
}

/* ── context ───────────────────────────────────────────────────────── */

static bool process_running(const char* name) {
	DIR* d = opendir("/proc");
	struct dirent* e;
	bool found = false;
	while (d && !found && (e = readdir(d))) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
		char path[288], comm[32] = "";
		snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
		FILE* f = fopen(path, "r");
		if (!f) continue;
		if (fgets(comm, sizeof(comm), f)) {
			comm[strcspn(comm, "\n")] = '\0';
			found = !strcmp(comm, name);
		}
		fclose(f);
	}
	if (d) closedir(d);
	return found;
}

/* NextUI in front: its menus, not a game (minarch) or a pak, which replace
 * nextui.elf while they run. */
static bool in_nextui_menus(void) {
	return process_running("nextui.elf") && !process_running("minarch.elf");
}

static bool setting_enabled(void) {
	NS_settingsLoad();
	return NS_settings()->quick_menu;
}

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ── menu ──────────────────────────────────────────────────────────── */

typedef enum { MENU_MAIN, MENU_HOST, MENU_ARMED } MenuKind;

typedef struct {
	MenuKind kind;
	int sel;
	int count;
	const char* items[VARNISH_MENU_MAX_ITEMS];
	char title[64];
} Menu;

enum { MAIN_HOST, MAIN_JOIN };
enum { HOST_ADHOC, HOST_WIFI };

static void build_menu(Menu* m, MenuKind kind) {
	NS_SessionInfo session;
	m->kind = kind;
	m->sel = 0;
	switch (kind) {
	case MENU_MAIN:
		snprintf(m->title, sizeof(m->title), "Netplay");
		m->items[0] = "Host";
		m->items[1] = "Join";
		m->count = 2;
		break;
	case MENU_HOST:
		snprintf(m->title, sizeof(m->title), "Host");
		m->items[0] = "Ad hoc network";
		m->items[1] = "Over WiFi";
		m->count = 2;
		break;
	case MENU_ARMED:
		/* Short: the title pill is one line at the device's scale. */
		if (NS_sessionInfo(&session) && session.role == NS_ROLE_CLIENT)
			snprintf(m->title, sizeof(m->title), "Joined %s", session.host);
		else if (NS_sessionInfo(&session) && session.guest_count > 0)
			snprintf(m->title, sizeof(m->title), "Hosting - %d guest%s",
			         session.guest_count, session.guest_count == 1 ? "" : "s");
		else if (NS_sessionInfo(&session))
			snprintf(m->title, sizeof(m->title), "Hosting");
		else
			snprintf(m->title, sizeof(m->title), "Netplay");
		m->items[0] = "End session";
		m->count = 1;
		break;
	}
}

static void show_menu(const Menu* m) {
	const char* hints = m->kind == MENU_HOST ? "B=BACK,A=HOST" : "B=CLOSE,A=SELECT";
	varnish_menu(CLIENT, m->sel, hints, m->title, m->items, m->count);
}

/* A step that takes seconds (the channel survey, bringing the network up):
 * the title says what is happening, nothing is selectable, no hints. */
static const char* working_title = "Netplay";
static void show_working(const char* what) {
	const char* items[] = { what };
	varnish_menu(CLIENT, -1, "-", working_title, items, 1);
}

static void on_progress(const char* stage, int attempt, int attempts) {
	char line[96];
	if (attempts > 1) snprintf(line, sizeof(line), "%s (%d/%d)", stage, attempt, attempts);
	else snprintf(line, sizeof(line), "%s", stage);
	show_working(line);
}

/* Centred, clear of the status icon at bottom-center. */
static void notice(const char* text) {
	varnish_pill(NOTICE, "center", NOTICE_S, text);
}

/* The app's Host sequence (main.c, SCREEN_HOST_MENU + do_arm), without its
 * screens. Returns a line for the notice. */
static void host(bool adhoc, char* result, int len) {
	char err[96] = "";
	char ssid[NS_SSID_LEN], psk[NS_PSK_LEN];
	NS_Check checks[8];
	NS_CheckResult worst;

	working_title = adhoc ? "Hosting ad hoc" : "Hosting over WiFi";
	if (NS_isArmed()) {
		snprintf(result, len, "A session is already set up.");
		return;
	}

	if (adhoc) {
		NS_hotspotCredentials(ssid, sizeof(ssid), psk, sizeof(psk));
		show_working("Surveying channels...");
		if (!NS_hotspotStart(ssid, psk, err, sizeof(err))) {
			snprintf(result, len, "%s", err[0] ? err : "Could not create the network.");
			return;
		}
		NS_announceHotspot(ssid, psk);
	} else {
		NS_hotspotStop();
		NS_announceHotspot(NULL, NULL);
	}

	show_working("Checking...");
	NS_runChecks(checks, 8, &worst);
	if (worst == NS_CHECK_FAIL) {
		NS_hotspotStop();
		snprintf(result, len, "Checks failed - open Netplay to see why.");
		return;
	}

	show_working("Arming...");
	if (!NS_arm(NS_ROLE_HOST, NULL, err, sizeof(err))) {
		NS_disarm();
		snprintf(result, len, "%s", err[0] ? err : "Could not arm.");
		return;
	}
	if (!NS_brokerStart(err, sizeof(err))) {
		NS_disarm();
		snprintf(result, len, "%s", err[0] ? err : "Could not start the host broker.");
		return;
	}

	char ip[NS_IP_LEN] = "";
	if (adhoc) snprintf(ip, sizeof(ip), "%s", NS_HOTSPOT_HOST_IP);
	else if (!NS_localIP(ip, sizeof(ip))) snprintf(ip, sizeof(ip), "?");
	snprintf(result, len, "Hosting on %s (%s)", ip, adhoc ? ssid : "WiFi");
}

/* Close the menu and give the buttons back. The release waits until NextUI
 * has read and discarded every press made in the menu - it can be seconds
 * behind after hosting, as it stalls while WiFi is reconfigured - so none of
 * them, least of all the A that chose Host, lands on its list. */
static void close_menu(void) {
	varnish_hide(CLIENT);
	varnish_release_input();
	read_press();   /* what was pressed meanwhile was the menu's, not ours */
}

static void run_menu(void) {
	Menu menu;
	char result[128] = "";
	double touched = now_s();

	varnish_grab_input();
	build_menu(&menu, NS_isArmed() ? MENU_ARMED : MENU_MAIN);
	show_menu(&menu);

	while (!stopping) {
		wait_input(500);
		Button b = read_press();
		if (b == QB_NONE) {
			if (now_s() - touched > IDLE_CLOSE_S) break;
			continue;
		}
		touched = now_s();

		if (b == QB_UP || b == QB_DOWN) {
			int step = b == QB_UP ? -1 : 1;
			menu.sel = (menu.sel + step + menu.count) % menu.count;
			show_menu(&menu);
		} else if (b == QB_B || b == QB_START) {
			if (menu.kind == MENU_HOST && b == QB_B) {
				build_menu(&menu, MENU_MAIN);
				show_menu(&menu);
				continue;
			}
			break;
		} else if (b == QB_A) {
			if (menu.kind == MENU_MAIN && menu.sel == MAIN_JOIN) {
				snprintf(result, sizeof(result), "Open the Netplay app to join.");
				break;
			}
			if (menu.kind == MENU_MAIN && menu.sel == MAIN_HOST) {
				if (NS_canHostAdhoc()) {
					build_menu(&menu, MENU_HOST);
					show_menu(&menu);
					continue;
				}
				host(false, result, sizeof(result));
				break;
			}
			if (menu.kind == MENU_HOST) {
				host(menu.sel == HOST_ADHOC, result, sizeof(result));
				break;
			}
			if (menu.kind == MENU_ARMED) {
				working_title = "Netplay";
				show_working("Ending session...");
				NS_endSession();
				snprintf(result, sizeof(result), "Session ended.");
				break;
			}
		}
	}

	close_menu();
	if (result[0]) notice(result);
}

/* ── main ──────────────────────────────────────────────────────────── */

static bool write_pid(const char* path) {
	char tmp[600], pid[32];
	snprintf(tmp, sizeof(tmp), "%s.%d", path, getpid());
	FILE* f = fopen(tmp, "w");
	if (!f) return false;
	snprintf(pid, sizeof(pid), "%d\n", getpid());
	fputs(pid, f);
	fclose(f);
	return rename(tmp, path) == 0;
}

int main(void) {
	char pid_path[512];

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGPIPE, SIG_IGN);

	NS_init();
	layout = !strcmp(NS_platform(), "my355") ? LAYOUT_KEYBOARD
	       : !strcmp(NS_platform(), "h700") ? LAYOUT_H700 : LAYOUT_GAMEPAD;
	snprintf(pid_path, sizeof(pid_path), "%s/quick-menu.pid", NS_statePath());
	write_pid(pid_path);
	NS_setProgressCallback(on_progress);
	/* A previous instance killed mid-menu may have left its parts up. */
	varnish_hide(CLIENT);

	open_devices();
	double reopened = now_s(), checked = now_s();
	while (!stopping) {
		wait_input(2000);
		Button b = read_press();

		if (now_s() - checked > 5) {
			checked = now_s();
			if (!setting_enabled()) break;
		}
		if (now_s() - reopened > 10 && !any_held()) {
			reopened = now_s();
			open_devices();
		}
		if (b == QB_START && in_nextui_menus())
			run_menu();
	}

	varnish_hide(CLIENT);
	varnish_release_input();
	close_devices();
	char recorded[32] = "";
	FILE* f = fopen(pid_path, "r");
	if (f) {
		if (fgets(recorded, sizeof(recorded), f) && atoi(recorded) == getpid()) remove(pid_path);
		fclose(f);
	}
	return 0;
}
