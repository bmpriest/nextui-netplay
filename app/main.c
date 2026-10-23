/*
 * Netplay.pak - session setup.
 *
 * Replaces hand-edited config files: pick host or join, discover the other
 * device, arm. Session setup lives here rather than in the shim because
 * minarch's GFX_/PAD_ are compiled into the executable and unreachable from a
 * loaded core (see docs/shim-architecture.md).
 */

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <msettings.h>
#include "defines.h"
#include "api.h"
#include "utils.h"

#include "netsetup.h"

typedef enum {
	SCREEN_MENU,
	SCREEN_HOST_MENU,
	SCREEN_TOOLS,
	SCREEN_SETTINGS,
	SCREEN_DEBUG,
	SCREEN_INSTANCED,
	SCREEN_PAK_INSTALL,     /* the three managed-pak screens serve pak_kind */
	SCREEN_PAK_MISMATCH,
	SCREEN_PAK_RESTORE,
	SCREEN_POWERSAVE_CONFIRM,
	SCREEN_CHECKS,
	SCREEN_JOINING,
} Screen;

static int quit = 0;
static bool quick_mode = false;
static void on_signal(int s) { (void)s; quit = 1; }

static Screen screen = SCREEN_MENU;
static int    menu_sel = 0;
static int    sub_sel = 0;
static char   status[128] = "";
/* Appended to the armed line: whether local compatibility cores were selected
 * is exactly the kind of thing that is otherwise invisible until a desync. */
static char   core_note[64] = "";

static NS_Check checks[8];
static int      check_count = 0;
static NS_CheckResult check_worst = NS_CHECK_OK;

static NS_Peer peers[NS_MAX_PEERS];
static int     peer_count = 0;
static int     peer_sel = 0;

/* Two ways to find a host, shown as one list: announcements heard on the
 * network we are already on, and ad hoc networks found by scanning for our SSID
 * prefix. A host that has already moved to its own network can only be found
 * the second way, so neither source alone is sufficient. */
static NS_Peer adhoc[NS_MAX_PEERS];
static int     adhoc_count = 0;
static int     join_total = 0;      /* peer_count + adhoc_count */
static NS_Peer* join_at(int i) { return i < peer_count ? &peers[i] : &adhoc[i - peer_count]; }

/* A hotspot can be visible both through its last broadcast announcement and
 * through the WiFi scan. They are two sightings of one host, not two choices.
 * Prefer the announcement: it carries platform metadata and credentials. Mark
 * that row as scanned, though, so the UI presents the SSID the user just found
 * instead of making the ad-hoc result appear to have vanished into an IP. */
static void dedupe_adhoc(void) {
	int kept = 0;
	for (int i = 0; i < adhoc_count; i++) {
		bool duplicate = false;
		for (int j = 0; j < peer_count; j++) {
			if (peers[j].hotspot && peers[j].ssid[0] &&
			    !strcmp(peers[j].ssid, adhoc[i].ssid)) {
				peers[j].scanned = true;
				duplicate = true;
				break;
			}
		}
		if (!duplicate) {
			if (kept != i) adhoc[kept] = adhoc[i];
			kept++;
		}
	}
	adhoc_count = kept;
}

static char local_ip[NS_IP_LEN] = "?";
static char hs_ssid[NS_SSID_LEN];
static char hs_psk[NS_PSK_LEN];
static int  hosting_hotspot;

//////////////////////////////////////////////////////////////////////////////
// drawing helpers
//////////////////////////////////////////////////////////////////////////////

static void draw_title(SDL_Surface* screen_s, const char* title) {
	SDL_Surface* t = TTF_RenderUTF8_Blended(font.large, title, COLOR_WHITE);
	if (t) {
		SDL_BlitSurface(t, NULL, screen_s,
		                &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), SCALE1(PADDING + 4)});
		SDL_FreeSurface(t);
	}
}

static int draw_line(SDL_Surface* screen_s, const char* text, int y, SDL_Color color, bool small) {
	SDL_Surface* t = TTF_RenderUTF8_Blended(small ? font.small : font.large, text, color);
	if (t) {
		SDL_BlitSurface(t, NULL, screen_s,
		                &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), y, 0, 0});
		y += t->h + SCALE1(4);
		SDL_FreeSurface(t);
	}
	return y;
}

static int draw_row(SDL_Surface* screen_s, const char* text, int y, bool selected) {
	SDL_Color color = selected ? COLOR_BLACK : COLOR_WHITE;
	SDL_Surface* t = TTF_RenderUTF8_Blended(font.large, text, color);
	if (t) {
		/* Pill caps are atlas assets with the platform's fixed PILL_SIZE.
		 * Deriving their height from TTF metrics can request a source rectangle
		 * larger than the asset; SDL then clips its right cap and bottom edge.
		 * Use NextUI's standard row geometry and center the rendered glyphs. */
		int row_h = SCALE1(PILL_SIZE);
		int x = SCALE1(PADDING);
		int text_x = x + SCALE1(BUTTON_PADDING);
		int pill_w = t->w + SCALE1(BUTTON_PADDING * 2);
		int max_w = screen_s->w - x - SCALE1(PADDING);
		if (pill_w > max_w) pill_w = max_w;
		if (selected) {
			SDL_Rect pill = {x, y, pill_w, row_h};
			GFX_blitPill(ASSET_WHITE_PILL, screen_s, &pill);
		}
		SDL_BlitSurface(t, NULL, screen_s,
		                &(SDL_Rect){text_x, y + (row_h - t->h) / 2, 0, 0});
		y += row_h;
		SDL_FreeSurface(t);
	}
	return y;
}

//////////////////////////////////////////////////////////////////////////////
// screens
//////////////////////////////////////////////////////////////////////////////

/* Several short screens' worth of choices, not one long list.
 *
 * The flat menu had grown to seven entries and pushed the WiFi status line off
 * the bottom, with no way to scroll to it - which is precisely the information
 * you need to tell an ad hoc session from a failed one. Submenus keep the top
 * level short enough that the status line is always visible. */
enum { MENU_HOST, MENU_JOIN, MENU_TOOLS, MENU_COUNT };
enum { HOST_ADHOC, HOST_WIFI, HOST_COUNT };
enum { TOOL_SETTINGS, TOOL_DEBUG, TOOL_FIXWIFI, TOOL_OFF, TOOL_COUNT };
/* "Use simple client" (NS_Settings.simple_client) is hidden: it was planned
 * but never implemented, so the row only toggled a stored value. The setting
 * is still read and written, so restoring the row is all it takes. */
enum { SET_COMPAT, SET_INSTANCED, SET_DSPERATE, SET_DRASTIC, SET_GAMESWITCHER, SET_POWERSAVE, SET_COUNT };
enum { DEBUG_FORCE_COMPAT, DEBUG_VERBOSE_LOGS, DEBUG_CHECKS, DEBUG_COUNT };

static const char* menu_label(int item) {
	switch (item) {
	case MENU_HOST:  return "Host";
	case MENU_JOIN:  return "Join";
	case MENU_TOOLS: return "Tools";
	}
	return "";
}

/* Creating our own network needs an AP-capable interface. Offering it on a
 * device that cannot do it produces a failure the user cannot act on, so the
 * entry is simply absent there. */
static int host_menu_count(void) { return NS_canHostAdhoc() ? HOST_COUNT : 1; }

static const char* host_label(int item) {
	if (!NS_canHostAdhoc()) item = HOST_WIFI;   /* only one entry to show */
	switch (item) {
	case HOST_ADHOC: return "Create ad hoc network";
	case HOST_WIFI:  return "Host over WiFi";
	}
	return "";
}

static const char* tool_label(int item) {
	switch (item) {
	case TOOL_SETTINGS: return "Settings";
	case TOOL_DEBUG:    return "Debug";
	case TOOL_FIXWIFI: return "Restore WiFi";
	case TOOL_OFF:     return "Turn off Netplay (Remove Bindings)";
	}
	return "";
}

/* Rendered as "Name: value" on one row, because these are toggles rather than
 * destinations - a submenu per setting would be three button presses to flip a
 * boolean. Instanced cores is the exception: it has a third state that opens a
 * picker, shown as ">" so it reads as "there is more behind this". */
static int settings_sel = 0;
static int debug_sel = 0;
static int inst_sel = 0;
static bool inst_staged[NS_INST_CORES];
static int prompt_sel = 0;
static int restore_sel = 0;
static bool restore_choice[3] = { true, true, true };
static bool restore_then_off = false;
static NS_PakStatus pak_status;
/* Which managed pak the install, mismatch and restore screens are about. */
static NS_ManagedPak pak_kind = NS_PAK_MGBA;
static const struct {
	const char* name;      /* in sentences */
	const char* pak;       /* the Emus/<p>/<pak>.pak directory */
	const char* binary;    /* the file whose build is compared */
	const char* install_title;
	const char* needs;
} PAK_UI[NS_PAK_COUNT] = {
	{ "mGBA", "MGBA", "mgba_libretro.so", "Install instanced mGBA?", "Instanced MGBA requires installation." },
	{ "DSperate", "DSP", "dsperate", "Install DSperate?", "DS local wireless needs DSP.pak (DSperate)." },
	{ "DraStic", "NDS", "libdrastic_wifi.so", "Add DS local wireless?", "Netplay adds its radio and launcher to NDS.pak." },
};

/* mGBA is reached from the instanced-core picker, the DS emulators from Settings. */
static Screen pak_back_screen(void) {
	return pak_kind == NS_PAK_MGBA ? SCREEN_INSTANCED : SCREEN_SETTINGS;
}

static void commit_instanced(void) {
	NS_Settings* c = NS_settings();
	int enabled = 0;
	for (int i = 0; i < NS_INST_CORES; i++) {
		c->inst_core[i] = inst_staged[i];
		if (inst_staged[i]) enabled++;
	}
	c->instanced = enabled ? NS_INST_SELECTED : NS_INST_OFF;
	NS_settingsSave();
}

static void disable_mgba_setting(void) {
	NS_Settings* c = NS_settings();
	if (c->instanced == NS_INST_ALL) {
		c->instanced = NS_INST_SELECTED;
		c->inst_core[0] = true;
	}
	c->inst_core[1] = false;
	if (!c->inst_core[0]) c->instanced = NS_INST_OFF;
	NS_settingsSave();
}

static void stage_instanced(void) {
	NS_Settings* c = NS_settings();
	for (int i = 0; i < NS_INST_CORES; i++)
		inst_staged[i] = c->instanced == NS_INST_ALL ||
		                  (c->instanced == NS_INST_SELECTED && c->inst_core[i]);
}

/* Whether the mgba instanced core (inst_core[1]) is enabled by a settings
 * snapshot. Single source of truth so the startup check, the staged-toggle
 * check, and stage_instanced() cannot silently drift apart. */
static bool mgba_instanced_enabled(const NS_Settings* c) {
	return c->instanced == NS_INST_ALL ||
	       (c->instanced == NS_INST_SELECTED && c->inst_core[1]);
}

static void setting_row(char* out, int len, int item) {
	NS_Settings* c = NS_settings();
	switch (item) {
	case SET_COMPAT:
		snprintf(out, len, "Use compatibility cores:  %s", c->compatibility_cores ? "Yes" : "No");
		break;
	case SET_INSTANCED:
		{ int n = c->instanced == NS_INST_ALL ? NS_INST_CORES : 0;
		  if (c->instanced == NS_INST_SELECTED)
			for (int i = 0; i < NS_INST_CORES; i++) if (c->inst_core[i]) n++;
		  snprintf(out, len, "Use instanced cores:  %d  >", n); }
		break;
	case SET_DSPERATE:
		snprintf(out, len, "Nintendo DS (DSperate):  %s",
		         !NS_pakAvailable(NS_PAK_DSP) ? "Not available" : c->dsperate ? "Yes" : "No");
		break;
	case SET_DRASTIC:
		snprintf(out, len, "Nintendo DS (DraStic):  %s",
		         !NS_pakAvailable(NS_PAK_NDS) ? "Not available" : c->drastic ? "Yes" : "No");
		break;
	case SET_GAMESWITCHER:
		snprintf(out, len, "Add Netplay to GameSwitcher:  %s", c->add_gameswitcher ? "Yes" : "No");
		break;
	case SET_POWERSAVE:
		snprintf(out, len, "Disable Wi-Fi Power Saving:  %s", c->disable_powersave ? "Yes" : "No");
		break;
	default: out[0] = '\0';
	}
}

static void debug_row(char* out, int len, int item) {
	NS_Settings* c = NS_settings();
	switch (item) {
	case DEBUG_FORCE_COMPAT:
		snprintf(out, len, "Force compatibility cores:  %s", c->force_compatibility ? "Yes" : "No");
		break;
	case DEBUG_VERBOSE_LOGS:
		snprintf(out, len, "Verbose debugging logs:  %s", c->verbose_logs ? "Yes" : "No");
		break;
	case DEBUG_CHECKS:
		snprintf(out, len, "Run checks");
		break;
	default: out[0] = '\0';
	}
}

//////////////////////////////////////////////////////////////////////////////
// wifi status
//
// Polled, not read per frame: PLAT_wifiConnection shells out to wpa_cli, ip
// and iw, which is far too expensive to do at render rate.
//////////////////////////////////////////////////////////////////////////////

static char wifi_ssid[SSID_MAX] = "";
static int  wifi_rssi = 0;
static bool wifi_up = false;
static uint32_t wifi_checked_ms = 0;

#define WIFI_POLL_MS 5000

static void wifi_poll(bool force) {
	uint32_t now = SDL_GetTicks();
	if (!force && wifi_checked_ms && now - wifi_checked_ms < WIFI_POLL_MS) return;
	wifi_checked_ms = now;

	struct WIFI_connection c;
	memset(&c, 0, sizeof(c));
	PLAT_wifiConnection(&c);

	wifi_up = c.valid && c.ssid[0];
	wifi_rssi = c.rssi;
	snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", wifi_up ? c.ssid : "not connected");
	if (c.valid && c.ip[0]) snprintf(local_ip, sizeof(local_ip), "%s", c.ip);
}

/* SSID plus what it means for us: the address a peer would dial. */
static int draw_wifi_line(SDL_Surface* s, int y) {
	char line[160];
	if (wifi_up) snprintf(line, sizeof(line), "%s  (%d dBm)  -  %s", wifi_ssid, wifi_rssi, local_ip);
	else         snprintf(line, sizeof(line), "WiFi: %s", wifi_ssid);
	return draw_line(s, line, y, COLOR_GRAY, true);
}

/* Joining blocks for up to a minute inside NS_hotspotJoin. Without a frame in
 * between, the device looks hung at exactly the moment the user is most likely
 * to power-cycle it. The callback lets us draw one per second. */
static SDL_Surface* progress_surface;

/* What the spinner is currently for. Joining is not the only slow operation:
 * ending a session puts the client stack back, which is the same work in
 * reverse and just as slow, and it used to run with nothing on screen at all. */
static const char* progress_title = "Joining";
static const char* progress_note  = "This can take up to a minute.";

static void set_progress(const char* title, const char* note) {
	progress_title = title;
	progress_note  = note;
}

static void turn_off_now(void) {
	set_progress("Turning off", "Putting WiFi back.");
	NS_disarm();
	set_progress("Joining", "This can take up to a minute.");
	NS_announceStop();
	NS_hotspotStop();
	hosting_hotspot = 0;
	snprintf(status, sizeof(status), "Netplay off. Launches are stock again.");
	screen = SCREEN_MENU;
}

/* Turning Netplay off offers each managed pak's restore in turn, from `from`
 * on, then turns off. A status that cannot be read stops the chain rather than
 * guessing, as the mGBA-only version did. */
static void turn_off_from(NS_ManagedPak from) {
	for (int k = from; k < NS_PAK_COUNT; k++) {
		/* Not gated on NS_pakAvailable: a card shared between platforms can
		 * hold another platform's managed install, and status reads that. */
		if (!NS_pakStatus((NS_ManagedPak)k, &pak_status)) {
			snprintf(status, sizeof(status), "Could not check %s status - try again.", PAK_UI[k].name);
			screen = SCREEN_TOOLS;
			return;
		}
		if (!pak_status.managed) continue;
		pak_kind = (NS_ManagedPak)k;
		restore_choice[0] = restore_choice[1] = restore_choice[2] = true;
		restore_sel = 0;
		restore_then_off = true;
		screen = SCREEN_PAK_RESTORE;
		return;
	}
	turn_off_now();
}

/* After a restore screen is done with: the setting it governs follows the pak. */
static void pak_restore_done(bool pak_restored) {
	if (pak_kind == NS_PAK_MGBA) {
		if (!restore_then_off) { inst_staged[1] = false; commit_instanced(); }
		else if (pak_restored) disable_mgba_setting();
	} else if (pak_kind == NS_PAK_DSP && (!restore_then_off || pak_restored)) {
		NS_settings()->dsperate = false;
		NS_settingsSave();
	} else if (pak_kind == NS_PAK_NDS && (!restore_then_off || pak_restored)) {
		NS_settings()->drastic = false;
		NS_settingsSave();
	}
	if (restore_then_off) turn_off_from((NS_ManagedPak)(pak_kind + 1));
	else screen = SCREEN_SETTINGS;
}

static void draw_progress(const char* stage, int step, int steps) {
	static const char* SPINNER[] = { "|", "/", "-", "\\" };
	static int tick;

	if (!progress_surface) return;

	SDL_Surface* s = progress_surface;
	GFX_clear(s);
	GFX_blitHardwareGroup(s, 0);
	draw_title(s, progress_title);

	int y = SCALE1(PADDING + 44);
	char line[128];
	snprintf(line, sizeof(line), "%s %s", SPINNER[tick++ % 4], stage);
	y = draw_line(s, line, y, COLOR_WHITE, false);
	y += SCALE1(6);
	/* Used to name NS_ADHOC_SSID, which is the legacy fixed name and not what
	 * a generated nextui-XXXX session is actually called. */
	snprintf(line, sizeof(line), "step %d of %d", step, steps);
	y = draw_line(s, line, y, COLOR_GRAY, true);
	draw_line(s, progress_note, y, COLOR_GRAY, true);

	GFX_flip(s);
}

static void render(SDL_Surface* s) {
	GFX_clear(s);

	/* Battery and the WiFi signal icon, in the standard top-right position. */
	GFX_blitHardwareGroup(s, 0);

	switch (screen) {
	case SCREEN_MENU: {
		NS_SessionInfo session;
		bool armed = NS_sessionInfo(&session);
		draw_title(s, quick_mode
		           ? (armed ? "Netplay Quick - armed" : "Netplay Quick")
		           : (armed ? "Netplay - armed" : "Netplay"));
		int y = SCALE1(PADDING + 44);
		if (armed) {
			char line[128];
			const char* network = session.network[0] ? session.network
			                    : wifi_up ? wifi_ssid : "WiFi";
			if (session.role == NS_ROLE_HOST) {
				snprintf(line, sizeof(line), "Hosting: %s", network);
				y = draw_line(s, line, y, COLOR_WHITE, false);
				y = draw_line(s, "Connected guests:", y + SCALE1(4), COLOR_GRAY, true);
				if (session.guest_count == 0) {
					y = draw_line(s, "  None yet", y, COLOR_GRAY, true);
				} else {
					for (int i = 0; i < session.guest_count; i++) {
						snprintf(line, sizeof(line), "  %s%s%s",
						         session.guest[i].ip[0] ? session.guest[i].ip : "associated",
						         session.guest[i].id[0] ? "  " : "",
						         session.guest[i].id);
						y = draw_line(s, line, y, COLOR_WHITE, true);
					}
				}
				if (!session.broker_running)
					y = draw_line(s, "Host broker is not running.", y, COLOR_GRAY, true);
			} else {
				snprintf(line, sizeof(line), "Joined: %s", network);
				y = draw_line(s, line, y, COLOR_WHITE, false);
				snprintf(line, sizeof(line), "Connected to host: %s",
				         session.host[0] ? session.host : "unknown");
				y = draw_line(s, line, y + SCALE1(4), COLOR_GRAY, true);
			}
			y = draw_row(s, "Tools", y + SCALE1(8), true);
		} else {
			for (int i = 0; i < MENU_COUNT; i++)
				y = draw_row(s, menu_label(i), y, i == menu_sel);
		}
		y = draw_wifi_line(s, y + SCALE1(8));
		if (status[0]) draw_line(s, status, y, COLOR_GRAY, true);
		GFX_blitButtonGroup(armed
		                    ? (char*[]){"B", "EXIT", "X", "END SESSION", "A", "SELECT", NULL}
		                    : (char*[]){"B", "EXIT", "A", "SELECT", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_HOST_MENU: {
		draw_title(s, "Host");
		int y = SCALE1(PADDING + 44);
		for (int i = 0; i < host_menu_count(); i++)
			y = draw_row(s, host_label(i), y, i == sub_sel);
		if (!NS_canHostAdhoc())
			y = draw_line(s, "This device cannot create a network (no AP interface).",
			              y + SCALE1(6), COLOR_GRAY, true);
		y = draw_wifi_line(s, y + SCALE1(8));
		if (status[0]) draw_line(s, status, y, COLOR_GRAY, true);
		GFX_blitButtonGroup((char*[]){"B", "BACK", "A", "SELECT", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_TOOLS: {
		draw_title(s, "Tools");
		int y = SCALE1(PADDING + 44);
		for (int i = 0; i < TOOL_COUNT; i++)
			y = draw_row(s, tool_label(i), y, i == sub_sel);
		y = draw_wifi_line(s, y + SCALE1(8));
		if (status[0]) draw_line(s, status, y, COLOR_GRAY, true);
		GFX_blitButtonGroup((char*[]){"B", "BACK", "A", "SELECT", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_SETTINGS: {
		draw_title(s, "Settings");
		int y = SCALE1(PADDING + 44);
		char row[128];
		/* Six rows push the hint under the button bar on 4:3 screens, so the
		 * list scrolls: as many rows as leave room for the hint and status. */
		int bottom = s->h - SCALE1(PADDING + PILL_SIZE);
		int text_h = TTF_FontLineSkip(font.small) + SCALE1(4);
		int visible = (bottom - y - SCALE1(6) - (status[0] ? 2 : 1) * text_h) / SCALE1(PILL_SIZE);
		if (visible < 1) visible = 1;
		if (visible > SET_COUNT) visible = SET_COUNT;
		int first = settings_sel - visible + 1;
		if (first < 0) first = 0;
		for (int i = first; i < first + visible; i++) {
			setting_row(row, sizeof(row), i);
			y = draw_row(s, row, y, i == settings_sel);
		}
		y += SCALE1(6);
		const char* hint = "";
		switch (settings_sel) {
		case SET_COMPAT:
			hint = "Use pak cores only when installed builds differ."; break;
		case SET_GAMESWITCHER:
			hint = "While armed, the switcher contains only Netplay."; break;
		case SET_INSTANCED:
			hint = "Both devices, and both carts on each. Else link cable."; break;
		case SET_DSPERATE:
			hint = "Installs DSP.pak. Each device runs its own DS."; break;
		case SET_DRASTIC:
			hint = "Adds local wireless to your NDS.pak (DraStic)."; break;
		case SET_POWERSAVE:
			hint = "Lower latency while armed. Uses more battery."; break;
		}
		y = draw_line(s, hint, y, COLOR_GRAY, true);
		if (status[0]) draw_line(s, status, y, COLOR_GRAY, true);
		GFX_blitButtonGroup((char*[]){"B", "BACK", "A", "CHANGE", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_DEBUG: {
		draw_title(s, "Debug");
		int y = SCALE1(PADDING + 44);
		char row[128];
		for (int i = 0; i < DEBUG_COUNT; i++) {
			debug_row(row, sizeof(row), i);
			y = draw_row(s, row, y, i == debug_sel);
		}
		y += SCALE1(6);
		const char* hint = "";
		switch (debug_sel) {
		case DEBUG_FORCE_COMPAT:
			hint = "Testing: use pak cores even when builds match."; break;
		case DEBUG_VERBOSE_LOGS:
			hint = "Retain a complete log for each game launch."; break;
		case DEBUG_CHECKS:
			hint = "Check bindings, paths, cores, and network support."; break;
		}
		y = draw_line(s, hint, y, COLOR_GRAY, true);
		if (status[0]) draw_line(s, status, y, COLOR_GRAY, true);
		GFX_blitButtonGroup((char*[]){"B", "BACK", "A", "SELECT", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_INSTANCED: {
		draw_title(s, "Instanced cores");
		int y = SCALE1(PADDING + 44);
		for (int i = 0; i < NS_INST_CORES; i++) {
			char row[128];
			bool have = NS_coreInstalled(NS_INST_CORE[i]);
			snprintf(row, sizeof(row), "[%s] %s%s",
			         inst_staged[i] ? "x" : " ", NS_INST_CORE[i],
			         have ? "" : "  (not installed)");
			y = draw_row(s, row, y, i == inst_sel);
		}
		y = draw_row(s, "Save", y, inst_sel == NS_INST_CORES);
		y += SCALE1(6);
		y = draw_line(s, "Changes take effect only after Save.", y, COLOR_GRAY, true);
		GFX_blitButtonGroup((char*[]){"B", "CANCEL", "A", "SELECT", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_PAK_INSTALL: {
		draw_title(s, PAK_UI[pak_kind].install_title);
		int y = SCALE1(PADDING + 44);
		char line[128];
		y = draw_line(s, PAK_UI[pak_kind].needs, y, COLOR_WHITE, true);
		if (pak_kind == NS_PAK_NDS) {
			/* An overlay: the user's DraStic and its settings stay. */
			y = draw_line(s, "Your DraStic settings stay as they are, and", y, COLOR_WHITE, true);
			y = draw_line(s, "the files it replaces are kept for later.", y, COLOR_WHITE, true);
			y = draw_line(s, "Continue?", y, COLOR_WHITE, true);
			y += SCALE1(5);
			y = draw_row(s, "Continue", y, prompt_sel == 0);
			draw_row(s, "Cancel", y, prompt_sel == 1);
			GFX_blitButtonGroup((char*[]){"B", "CANCEL", "A", "SELECT", NULL}, 1, s, 1);
			break;
		}
		if (pak_status.installed) {
			snprintf(line, sizeof(line), "Your current %s.pak, saves and savestates", PAK_UI[pak_kind].pak);
			y = draw_line(s, line, y, COLOR_WHITE, true);
			y = draw_line(s, "will be backed up.", y, COLOR_WHITE, true);
		}
		snprintf(line, sizeof(line), "%s will be installed for every platform", PAK_UI[pak_kind].pak);
		y = draw_line(s, line, y, COLOR_WHITE, true);
		y = draw_line(s, "on your device. Continue?", y, COLOR_WHITE, true);
		y += SCALE1(5);
		y = draw_row(s, "Continue", y, prompt_sel == 0);
		draw_row(s, "Cancel", y, prompt_sel == 1);
		GFX_blitButtonGroup((char*[]){"B", "CANCEL", "A", "SELECT", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_PAK_MISMATCH: {
		char line[128];
		snprintf(line, sizeof(line), "%s build changed", PAK_UI[pak_kind].name);
		draw_title(s, line);
		int y = SCALE1(PADDING + 44);
		snprintf(line, sizeof(line), "The installed %s does not match", PAK_UI[pak_kind].binary);
		y = draw_line(s, line, y, COLOR_WHITE, true);
		y = draw_line(s, "the build supplied by Netplay.", y, COLOR_WHITE, true);
		snprintf(line, sizeof(line), "Reinstall %s?", PAK_UI[pak_kind].pak);
		y = draw_line(s, line, y, COLOR_WHITE, true);
		y += SCALE1(5);
		y = draw_row(s, "Reinstall", y, prompt_sel == 0);
		draw_row(s, "Ignore this build", y, prompt_sel == 1);
		GFX_blitButtonGroup((char*[]){"B", "IGNORE", "A", "SELECT", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_POWERSAVE_CONFIRM: {
		draw_title(s, "Disable Power Saving?");
		int y = SCALE1(PADDING + 44);
		y = draw_line(s, "Disabling Power Saving requires a driver reboot", y, COLOR_WHITE, true);
		y = draw_line(s, "that may take several seconds. Continue?", y, COLOR_WHITE, true);
		y += SCALE1(5);
		draw_line(s, "WiFi drops briefly. Lasts until the device reboots.", y, COLOR_GRAY, true);
		GFX_blitButtonGroup((char*[]){"B", "CANCEL", "A", "CONTINUE", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_PAK_RESTORE: {
		char line[128];
		if (pak_kind == NS_PAK_NDS) {
			/* Nothing but NDS.pak's own files to put back: no saves, no states. */
			draw_title(s, "Remove DS local wireless?");
			int y = SCALE1(PADDING + 44);
			y = draw_line(s, "NDS.pak gets its own launcher back. DraStic,", y, COLOR_WHITE, true);
			y = draw_line(s, "its settings and your saves are not touched.", y, COLOR_WHITE, true);
			y += SCALE1(5);
			y = draw_row(s, "Remove from NDS.pak", y, restore_sel == 0);
			draw_row(s, "Keep it", y, restore_sel == 1);
			GFX_blitButtonGroup((char*[]){"B", "CANCEL", "A", "SELECT", NULL}, 1, s, 1);
			break;
		}
		snprintf(line, sizeof(line), "Restore %s backup?", PAK_UI[pak_kind].name);
		draw_title(s, line);
		int y = SCALE1(PADDING + 42);
		snprintf(line, sizeof(line), "Backup created: %s",
		         pak_status.backup_date[0] ? pak_status.backup_date : "unknown date");
		y = draw_line(s, line, y, COLOR_GRAY, true);
		char pak_q[64];
		snprintf(pak_q, sizeof(pak_q), "Restore %s from backup?", PAK_UI[pak_kind].pak);
		const char* names[3] = { pak_q, "Restore saves from backup?", "Restore save states from backup?" };
		for (int i = 0; i < 3; i++) {
			snprintf(line, sizeof(line), "%s  %s", names[i], restore_choice[i] ? "Yes" : "No");
			y = draw_row(s, line, y, restore_sel == i);
		}
		y = draw_row(s, "Apply", y, restore_sel == 3);
		snprintf(line, sizeof(line), "Keep installed %s", PAK_UI[pak_kind].pak);
		y = draw_row(s, line, y, restore_sel == 4);
		y = draw_line(s, "Restoring saves/save states may overwrite progress.", y + SCALE1(3), COLOR_GRAY, true);
		draw_line(s, "Overwritten files move to .userdata/shared/Netplay.", y, COLOR_GRAY, true);
		GFX_blitButtonGroup((char*[]){"B", "CANCEL", "A", "SELECT", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_CHECKS: {
		draw_title(s, "Checks");
		int y = SCALE1(PADDING + 44);
		for (int i = 0; i < check_count; i++) {
			char line[160];
			const char* mark = checks[i].result == NS_CHECK_OK ? "OK  "
			                 : checks[i].result == NS_CHECK_WARN ? "WARN" : "FAIL";
			snprintf(line, sizeof(line), "%s  %s", mark, checks[i].label);
			y = draw_line(s, line, y,
			              checks[i].result == NS_CHECK_OK ? COLOR_WHITE : COLOR_GRAY, false);
			y = draw_line(s, checks[i].detail, y, COLOR_GRAY, true);
		}
		GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_JOINING: {
		draw_title(s, "Join");
		int y = SCALE1(PADDING + 44);
		if (join_total == 0) {
			y = draw_line(s, "Looking for a host...", y, COLOR_WHITE, false);
			y += SCALE1(4);
			y = draw_line(s, "On WiFi: the host session must still be armed.", y, COLOR_GRAY, true);
			y = draw_line(s, "Ad hoc: press Y to scan again.", y, COLOR_GRAY, true);
		} else {
			for (int i = 0; i < join_total; i++) {
				NS_Peer* pr = join_at(i);
				char line[128];
				if (pr->scanned) {
					/* The SSID carries the host's code, which is what the user
					 * sees on the hosting device - that is the thing to match. */
					snprintf(line, sizeof(line), "%s  (ad hoc)", pr->ssid);
				} else {
					int same = !strcmp(pr->platform, NS_platform());
					snprintf(line, sizeof(line), "%s%s%s", pr->ip,
					         pr->hotspot ? "  ad hoc" : "  WiFi",
					         same ? "" : "  (different device)");
				}
				y = draw_row(s, line, y, i == peer_sel);
			}
		}
		y = draw_wifi_line(s, y + SCALE1(8));
		if (status[0]) draw_line(s, status, y, COLOR_GRAY, true);
		GFX_blitButtonGroup((char*[]){"B", "BACK", "Y", "RESCAN", "A", "SELECT", NULL}, 1, s, 1);
		break;
	}
	}

	GFX_flip(s);
}

//////////////////////////////////////////////////////////////////////////////

static void abort_arm_attempt(bool may_have_activated) {
	/* One rollback path owns everything an arm attempt can start. NS_disarm is
	 * deliberately reserved for attempts that may have created a session or
	 * partial bindings; a failed preflight must not remove bindings retained
	 * from an earlier, deliberately ended session. */
	NS_announceStop();
	NS_compatServeStop();
	if (may_have_activated)
		NS_disarm();
	else
		NS_hotspotStop();
	hosting_hotspot = 0;
}

static void do_arm(NS_Role role, const char* peer) {
	char err[96] = "";

	/* Never overwrite a live session. Re-arming used to orphan the existing
	 * peer and could replace its transport underneath a running game. */
	if (NS_isArmed()) {
		snprintf(status, sizeof(status), "End the current session with X first.");
		screen = SCREEN_MENU;
		return;
	}

	/* These are launch-path invariants, not optional diagnostics. If one fails,
	 * arming would appear successful but games would either bypass the shim or
	 * use the wrong save path. Cross-platform compatibility is decided by the
	 * core manifest exchange below, so the old crude platform check is gone. */
	check_count = NS_runChecks(checks, 8, &check_worst);
	if (check_worst == NS_CHECK_FAIL) {
		abort_arm_attempt(false);
		snprintf(status, sizeof(status), "Checks failed - not arming");
		screen = SCREEN_CHECKS;
		return;
	}

	if (!NS_arm(role, peer, err, sizeof(err))) {
		abort_arm_attempt(true);
		snprintf(status, sizeof(status), "%s", err[0] ? err : "could not arm");
		screen = SCREEN_MENU;
		return;
	}

	// Name the transport and the delay it bought. Which network a session ended
	// up on is the single thing that decides whether it is playable, and it was
	// previously invisible - a session that quietly fell back to the house
	// network looked identical to one on ad hoc until the game stuttered.
	/* Settle the builds now, while the user is still choosing a game, rather
	 * than in front of a launch. The detached host broker serves the exchange;
	 * the client drives it. */
	if (role == NS_ROLE_HOST) {
		if (!NS_brokerStart(err, sizeof(err))) {
			abort_arm_attempt(true);
			snprintf(status, sizeof(status), "%s", err[0] ? err : "Could not start host broker.");
			screen = SCREEN_MENU;
			return;
		}
	} else {
		/* Always compare manifests. With compatibility disabled the exchange
		 * still records differing installed builds so both launchers can warn;
		 * the setting only controls whether a packaged fallback may be chosen. */
		set_progress("Checking cores", "Comparing builds with the host.");
		draw_progress("Comparing builds", 1, 1);
		char cerr[128] = "";
		int selected = NS_compatSync(peer, cerr, sizeof(cerr));
		set_progress("Joining", "This can take up to a minute.");
		if (selected > 0)
			snprintf(core_note, sizeof(core_note), "  -  %d compatibility fallback(s)", selected);
		else if (selected < 0) {
			/* Compatibility selection is part of arming, not an optional status
			 * probe. Continuing after it fails can put the peers on different
			 * cores, which is guaranteed to desynchronize shared-screen play. */
			abort_arm_attempt(true);
			snprintf(status, sizeof(status), "Core check failed: %s", cerr);
			screen = SCREEN_MENU;
			return;
		}
		else
			core_note[0] = '\0';
	}

	int adhoc = NS_hotspotActive();
	if (role == NS_ROLE_HOST)
		snprintf(status, sizeof(status), "Hosting on %s  -  %s, delay %d",
		         local_ip, adhoc ? "ad hoc" : "WiFi",
		         adhoc ? NS_INPUT_DELAY_ADHOC : NS_INPUT_DELAY_WIFI);
	else
		snprintf(status, sizeof(status), "Joined %s  -  %s, delay %d%s",
		         peer, adhoc ? "ad hoc" : "WiFi",
		         adhoc ? NS_INPUT_DELAY_ADHOC : NS_INPUT_DELAY_WIFI, core_note);
	screen = SCREEN_MENU;
}

int main(int argc, char* argv[]) {
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--quick")) quick_mode = true;
	}

	SDL_Surface* s = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();
	/* NextUI launches every pak at the performance ceiling. This setup UI and
	 * its detached broker are low-duty control-plane work; use the platform's
	 * normal menu profile instead. Gameplay still pins performance only while
	 * an armed emulator process is running. */
	PWR_setCPUSpeed(CPU_SPEED_MENU);
	InitSettings();

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	NS_init();
	progress_surface = s;
	NS_setProgressCallback(draw_progress);
	WIFI_init();
	bool cleaned_leftover = NS_cleanupStaleSession();

	// A join that failed - or an AP that vanished after a successful one - used
	// to leave the device associated to nothing, with no way back except the
	// system WiFi menu. Check on every launch, before anything else.
	NS_Recovery recovered = NS_wifiRecoverIfStranded();
	if (recovered == NS_RECOVERY_DONE) {
		snprintf(status, sizeof(status), "Reconnected to WiFi after an ad hoc session.");
	} else if (recovered == NS_RECOVERY_FAILED) {
		snprintf(status, sizeof(status),
		         "Could not reconnect to WiFi - use Restore WiFi, or reboot.");
	} else if (cleaned_leftover) {
		snprintf(status, sizeof(status), "Cleaned up a session left by a previous boot.");
	}
	if (!NS_localIP(local_ip, sizeof(local_ip))) snprintf(local_ip, sizeof(local_ip), "no network");
	wifi_poll(true);
	NS_SessionInfo startup_session;
	if (NS_sessionInfo(&startup_session)) {
		if (startup_session.role == NS_ROLE_HOST && !startup_session.broker_running) {
			char broker_err[96] = "";
			if (NS_brokerStart(broker_err, sizeof(broker_err)))
				snprintf(status, sizeof(status), "Host broker restarted.");
			else
				snprintf(status, sizeof(status), "%s", broker_err);
		} else {
			snprintf(status, sizeof(status), "A session is already set up.");
		}
	}
	/* Where only a driver reload turns power save off, a reboot turns it back
	 * on behind the setting's back. Show what the radio is actually doing. */
	if (NS_settings()->disable_powersave && NS_wifiPowerSaveNeedsReload()) {
		NS_settings()->disable_powersave = false;
		NS_settingsSave();
		snprintf(status, sizeof(status), "WiFi power saving is back on after a reboot.");
	}
	/* An installed pak can be replaced by an updater while the setting remains
	 * enabled. Check once per setup-app launch; an ignored verdict is keyed to
	 * both hashes, so it stays quiet only until either binary changes. */
	NS_Settings* startup_settings = NS_settings();
	bool startup_mgba = mgba_instanced_enabled(startup_settings);
	if (startup_mgba && NS_pakStatus(NS_PAK_MGBA, &pak_status) &&
	    pak_status.mismatch && !pak_status.mismatch_ignored) {
		stage_instanced();
		pak_kind = NS_PAK_MGBA;
		prompt_sel = 0;
		screen = SCREEN_PAK_MISMATCH;
	} else if (startup_settings->dsperate && NS_pakAvailable(NS_PAK_DSP) &&
	           NS_pakStatus(NS_PAK_DSP, &pak_status) &&
	           pak_status.mismatch && !pak_status.mismatch_ignored) {
		pak_kind = NS_PAK_DSP;
		prompt_sel = 0;
		screen = SCREEN_PAK_MISMATCH;
	} else if (startup_settings->drastic && NS_pakAvailable(NS_PAK_NDS) &&
	           NS_pakStatus(NS_PAK_NDS, &pak_status) &&
	           pak_status.mismatch && !pak_status.mismatch_ignored) {
		pak_kind = NS_PAK_NDS;
		prompt_sel = 0;
		screen = SCREEN_PAK_MISMATCH;
	}

	int dirty = 1;
	uint32_t session_checked_ms = 0;
	while (!quit) {
		PAD_poll();

		/* Rate-limited internally; a change in signal or SSID redraws. */
		char was_ssid[SSID_MAX];
		bool was_up = wifi_up;
		snprintf(was_ssid, sizeof(was_ssid), "%s", wifi_ssid);
		wifi_poll(false);
		if (was_up != wifi_up || strcmp(was_ssid, wifi_ssid)) dirty = 1;
		if (NS_isArmed()) {
			uint32_t now = SDL_GetTicks();
			if (!session_checked_ms || now - session_checked_ms >= 1000) {
				session_checked_ms = now;
				dirty = 1;
			}
		}

		/* X ends the session from any screen. It is the thing most often
		 * wanted and it used to be buried in the list. */
		if (PAD_justPressed(BTN_X) && NS_isArmed()) {
			// NS_endSession stops the AP and restores wifi itself; calling
			// NS_hotspotStop again here ran the teardown twice and logged a
			// spurious "nothing recorded to restore wifi with".
			set_progress("Restoring original WiFi", "Leaving the ad hoc network.");
			NS_endSession();
			set_progress("Joining", "This can take up to a minute.");
			core_note[0] = '\0';
			hosting_hotspot = 0;
			/* Forced, not the 5s poll. The cached values are from before the
			 * teardown, so without this the status text reports the network we
			 * were on while the live icon reports the one we are on - which is
			 * how a device claiming to be connected showed no WiFi icon. */
			wifi_poll(true);
			snprintf(status, sizeof(status), wifi_up
			         ? "Session ended. Bindings kept."
			         : "Session ended. Original WiFi recovery continues...");
			screen = SCREEN_MENU;
			dirty = 1;
		}

		switch (screen) {
		case SCREEN_MENU:
			if (NS_isArmed()) {
				/* Session status replaces Host/Join while armed. Tools is the only
				 * selectable row; X remains the explicit lifecycle action. */
				if (PAD_justPressed(BTN_B)) quit = 1;
				if (PAD_justPressed(BTN_A)) {
					status[0] = '\0';
					sub_sel = 0;
					screen = SCREEN_TOOLS;
					dirty = 1;
				}
				break;
			}
			if (PAD_justPressed(BTN_UP))   { menu_sel = (menu_sel + MENU_COUNT - 1) % MENU_COUNT; dirty = 1; }
			if (PAD_justPressed(BTN_DOWN)) { menu_sel = (menu_sel + 1) % MENU_COUNT; dirty = 1; }
			if (PAD_justPressed(BTN_B))    quit = 1;
			if (PAD_justPressed(BTN_A)) {
				status[0] = '\0';
				sub_sel = 0;
				if (NS_isArmed() && (menu_sel == MENU_HOST || menu_sel == MENU_JOIN)) {
					snprintf(status, sizeof(status), "End the current session with X first.");
					dirty = 1;
					break;
				}
				switch (menu_sel) {
				case MENU_HOST:  screen = SCREEN_HOST_MENU; break;
				case MENU_TOOLS: screen = SCREEN_TOOLS; break;
				case MENU_JOIN:
					peer_count = 0; peer_sel = 0; adhoc_count = 0; join_total = 0;
					NS_discoverTick(NULL, 0);
					NS_discoverStart();
					screen = SCREEN_JOINING;
					// One scan on entry so an ad hoc host shows up without the
					// user having to know to press anything.
					snprintf(status, sizeof(status), "Scanning for ad hoc networks...");
					render(s);
					adhoc_count = NS_scanAdhoc(adhoc, NS_MAX_PEERS);
					status[0] = '\0';
					break;
				}
				dirty = 1;
			}
			break;

		case SCREEN_HOST_MENU: {
			int n = host_menu_count();
			if (PAD_justPressed(BTN_UP))   { sub_sel = (sub_sel + n - 1) % n; dirty = 1; }
			if (PAD_justPressed(BTN_DOWN)) { sub_sel = (sub_sel + 1) % n; dirty = 1; }
			if (PAD_justPressed(BTN_B))    { screen = SCREEN_MENU; dirty = 1; }
			if (PAD_justPressed(BTN_A)) {
				int choice = NS_canHostAdhoc() ? sub_sel : HOST_WIFI;
				status[0] = '\0';

				if (choice == HOST_ADHOC) {
					NS_hotspotCredentials(hs_ssid, sizeof(hs_ssid), hs_psk, sizeof(hs_psk));
					// The channel survey takes a couple of seconds; say so.
					snprintf(status, sizeof(status), "Surveying channels...");
					render(s);

					char err[96] = "";
					if (!NS_hotspotStart(hs_ssid, hs_psk, err, sizeof(err))) {
						snprintf(status, sizeof(status), "%s", err);
						dirty = 1;
						break;   // stay here so the reason is readable
					}
					hosting_hotspot = 1;
					snprintf(local_ip, sizeof(local_ip), "%s", NS_HOTSPOT_HOST_IP);
					NS_announceHotspot(hs_ssid, hs_psk);
				} else {
					// Leaving an AP up would share one radio and one channel
					// with the network this session is about to run over.
					hosting_hotspot = 0;
					NS_hotspotStop();
					NS_announceHotspot(NULL, NULL);
				}

				do_arm(NS_ROLE_HOST, NULL);
				dirty = 1;
			}
			break;
		}

		case SCREEN_TOOLS:
			if (PAD_justPressed(BTN_UP))   { sub_sel = (sub_sel + TOOL_COUNT - 1) % TOOL_COUNT; dirty = 1; }
			if (PAD_justPressed(BTN_DOWN)) { sub_sel = (sub_sel + 1) % TOOL_COUNT; dirty = 1; }
			if (PAD_justPressed(BTN_B))    { screen = SCREEN_MENU; dirty = 1; }
			if (PAD_justPressed(BTN_A)) {
				status[0] = '\0';
				switch (sub_sel) {
				case TOOL_SETTINGS:
					settings_sel = 0;
					screen = SCREEN_SETTINGS;
					break;

				case TOOL_DEBUG:
					debug_sel = 0;
					screen = SCREEN_DEBUG;
					break;

				case TOOL_FIXWIFI:
					snprintf(status, sizeof(status), "Restoring WiFi...");
					render(s);
					set_progress("Restoring WiFi", "Reconnecting to your network.");
					NS_wifiRestore();
					set_progress("Joining", "This can take up to a minute.");
					wifi_poll(true);
					snprintf(status, sizeof(status), wifi_up
					         ? "Reconnected to %s." : "Could not reconnect - use system WiFi settings.",
					         wifi_ssid);
					screen = SCREEN_MENU;
					break;

				case TOOL_OFF:
					turn_off_from(NS_PAK_MGBA);
					break;
				}
				dirty = 1;
			}
			break;

		case SCREEN_SETTINGS: {
			NS_Settings* c = NS_settings();
			if (PAD_justPressed(BTN_UP))   { settings_sel = (settings_sel + SET_COUNT - 1) % SET_COUNT; dirty = 1; }
			if (PAD_justPressed(BTN_DOWN)) { settings_sel = (settings_sel + 1) % SET_COUNT; dirty = 1; }
			if (PAD_justPressed(BTN_B))    { screen = SCREEN_TOOLS; dirty = 1; }
			if (PAD_justPressed(BTN_A)) {
				status[0] = '\0';
				switch (settings_sel) {
				case SET_COMPAT:
					c->compatibility_cores = !c->compatibility_cores;
					if (!c->compatibility_cores) c->force_compatibility = false;
					break;
				case SET_GAMESWITCHER: c->add_gameswitcher = !c->add_gameswitcher; break;
				case SET_POWERSAVE:
					/* Applies to an armed session now, not only from the next
					 * game launch: Restore is a no-op when nothing was recorded. */
					if (!c->disable_powersave && NS_wifiPowerSaveNeedsReload()) {
						screen = SCREEN_POWERSAVE_CONFIRM;   /* H700: slow, so ask */
						break;
					}
					c->disable_powersave = !c->disable_powersave;
					if (c->disable_powersave) {
						NS_SessionInfo armed;
						if (NS_sessionInfo(&armed)) NS_wifiPowerSaveDisable();
					} else {
						NS_wifiPowerSaveRestore();
						/* Reloading back would cost the same wait again for no
						 * gain; the next boot loads the driver's default. */
						if (!strcmp(PLATFORM, "h700"))
							snprintf(status, sizeof(status), "Power saving returns after the next reboot.");
					}
					break;
				case SET_INSTANCED:
					stage_instanced();
					inst_sel = 0;
					screen = SCREEN_INSTANCED;
					if (inst_staged[1]) {
						pak_kind = NS_PAK_MGBA;
						if (!NS_pakStatus(NS_PAK_MGBA, &pak_status)) {
							snprintf(status, sizeof(status), "Could not check mGBA status - try again.");
						} else if (pak_status.mismatch && !pak_status.mismatch_ignored) {
							prompt_sel = 0;
							screen = SCREEN_PAK_MISMATCH;
						}
					}
					break;
				case SET_DSPERATE:
					if (!NS_pakAvailable(NS_PAK_DSP)) {
						snprintf(status, sizeof(status), "DSperate is not built for this device.");
						break;
					}
					pak_kind = NS_PAK_DSP;
					if (!NS_pakStatus(NS_PAK_DSP, &pak_status)) {
						snprintf(status, sizeof(status), "Could not check DSP.pak status - try again.");
					} else if (!c->dsperate) {
						if (!pak_status.managed || pak_status.missing || pak_status.mismatch) {
							prompt_sel = 0;
							screen = SCREEN_PAK_INSTALL;
						} else c->dsperate = true;
					} else if (pak_status.managed) {
						restore_choice[0] = restore_choice[1] = restore_choice[2] = true;
						restore_sel = 0; restore_then_off = false;
						screen = SCREEN_PAK_RESTORE;
					} else c->dsperate = false;
					break;
				case SET_DRASTIC:
					if (!NS_pakAvailable(NS_PAK_NDS)) {
						snprintf(status, sizeof(status), "DraStic local wireless is not built for this device.");
						break;
					}
					pak_kind = NS_PAK_NDS;
					if (!NS_pakStatus(NS_PAK_NDS, &pak_status)) {
						snprintf(status, sizeof(status), "Could not check NDS.pak status - try again.");
					} else if (!c->drastic) {
						if (!pak_status.base_installed)
							snprintf(status, sizeof(status), "Install NDS.pak (DraStic) first.");
						else if (pak_status.unsupported && !pak_status.installed)
							snprintf(status, sizeof(status), "This NDS.pak's DraStic build is not supported.");
						else if (!pak_status.managed || pak_status.missing || pak_status.mismatch) {
							prompt_sel = 0;
							screen = SCREEN_PAK_INSTALL;
						} else c->drastic = true;
					} else if (pak_status.managed) {
						restore_sel = 0; restore_then_off = false;
						screen = SCREEN_PAK_RESTORE;
					} else c->drastic = false;
					break;
				}
				if (settings_sel != SET_INSTANCED) NS_settingsSave();
				dirty = 1;
			}
			break;
		}

		case SCREEN_DEBUG: {
			NS_Settings* c = NS_settings();
			if (PAD_justPressed(BTN_UP))   { debug_sel = (debug_sel + DEBUG_COUNT - 1) % DEBUG_COUNT; dirty = 1; }
			if (PAD_justPressed(BTN_DOWN)) { debug_sel = (debug_sel + 1) % DEBUG_COUNT; dirty = 1; }
			if (PAD_justPressed(BTN_B))    { screen = SCREEN_TOOLS; dirty = 1; }
			if (PAD_justPressed(BTN_A)) {
				status[0] = '\0';
				switch (debug_sel) {
				case DEBUG_FORCE_COMPAT:
					c->force_compatibility = !c->force_compatibility;
					if (c->force_compatibility) c->compatibility_cores = true;
					NS_settingsSave();
					break;
				case DEBUG_VERBOSE_LOGS:
					c->verbose_logs = !c->verbose_logs;
					NS_settingsSave();
					break;
				case DEBUG_CHECKS:
					check_count = NS_runChecks(checks, 8, &check_worst);
					screen = SCREEN_CHECKS;
					break;
				}
				dirty = 1;
			}
			break;
		}

		case SCREEN_INSTANCED: {
			if (PAD_justPressed(BTN_UP))   { inst_sel = (inst_sel + NS_INST_CORES) % (NS_INST_CORES + 1); dirty = 1; }
			if (PAD_justPressed(BTN_DOWN)) { inst_sel = (inst_sel + 1) % (NS_INST_CORES + 1); dirty = 1; }
			if (PAD_justPressed(BTN_B))    { screen = SCREEN_SETTINGS; dirty = 1; }
			if (PAD_justPressed(BTN_A)) {
				if (inst_sel < NS_INST_CORES && NS_coreInstalled(NS_INST_CORE[inst_sel])) {
					inst_staged[inst_sel] = !inst_staged[inst_sel];
				} else if (inst_sel < NS_INST_CORES) {
					snprintf(status, sizeof(status), "%s is not installed.", NS_INST_CORE[inst_sel]);
				} else {
					bool old_mgba = mgba_instanced_enabled(NS_settings());
					pak_kind = NS_PAK_MGBA;
					if (!NS_pakStatus(NS_PAK_MGBA, &pak_status)) {
						snprintf(status, sizeof(status), "Could not check mGBA status - try again.");
					} else if (!old_mgba && inst_staged[1] &&
					    (!pak_status.managed || pak_status.missing || pak_status.mismatch)) {
						prompt_sel = 0; screen = SCREEN_PAK_INSTALL;
					} else if (old_mgba && !inst_staged[1] && pak_status.managed) {
						restore_choice[0] = restore_choice[1] = restore_choice[2] = true;
						restore_sel = 0; restore_then_off = false;
						screen = SCREEN_PAK_RESTORE;
					} else {
						commit_instanced(); screen = SCREEN_SETTINGS;
					}
				}
				dirty = 1;
			}
			break;
		}

		case SCREEN_PAK_INSTALL:
			if (PAD_justPressed(BTN_UP) || PAD_justPressed(BTN_DOWN)) { prompt_sel = !prompt_sel; dirty = 1; }
			if (PAD_justPressed(BTN_B)) { screen = pak_back_screen(); dirty = 1; }
			if (PAD_justPressed(BTN_A)) {
				if (prompt_sel == 0) {
					char title[48];
					snprintf(title, sizeof(title), "Installing %s", PAK_UI[pak_kind].name);
					set_progress(title, "Backing up files and verifying builds.");
					if (NS_pakInstall(pak_kind, status, sizeof(status))) {
						if (pak_kind == NS_PAK_MGBA) {
							commit_instanced();
							snprintf(status, sizeof(status), "Instanced mGBA installed.");
						} else if (pak_kind == NS_PAK_DSP) {
							NS_settings()->dsperate = true;
							NS_settingsSave();
							snprintf(status, sizeof(status), "DSperate installed.");
						} else {
							NS_settings()->drastic = true;
							NS_settingsSave();
							snprintf(status, sizeof(status), "DS local wireless added to NDS.pak.");
						}
						screen = SCREEN_SETTINGS;
					} else screen = pak_back_screen();
				} else screen = pak_back_screen();
				dirty = 1;
			}
			break;

		case SCREEN_POWERSAVE_CONFIRM:
			if (PAD_justPressed(BTN_B)) { screen = SCREEN_SETTINGS; dirty = 1; }
			if (PAD_justPressed(BTN_A)) {
				/* Blocking for the whole reload; one progress frame says why. */
				set_progress("Disabling Power Saving", "WiFi reconnects when the driver is back.");
				draw_progress("Restarting the WiFi driver", 1, 1);
				NS_Settings* c = NS_settings();
				status[0] = '\0';
				if (NS_wifiPowerSaveReload(status, sizeof(status))) {
					c->disable_powersave = true;
					NS_settingsSave();
					NS_SessionInfo armed;
					if (NS_sessionInfo(&armed)) NS_wifiPowerSaveDisable();
					if (!status[0])
						snprintf(status, sizeof(status), "Power saving off until the device reboots.");
				}
				set_progress("Joining", "This can take up to a minute.");
				wifi_poll(true);
				screen = SCREEN_SETTINGS;
				dirty = 1;
			}
			break;

		case SCREEN_PAK_MISMATCH:
			if (PAD_justPressed(BTN_UP) || PAD_justPressed(BTN_DOWN)) { prompt_sel = !prompt_sel; dirty = 1; }
			if (PAD_justPressed(BTN_B)) {
				NS_pakIgnoreMismatch(pak_kind, status, sizeof(status)); screen = pak_back_screen(); dirty = 1;
			}
			if (PAD_justPressed(BTN_A)) {
				if (prompt_sel == 0) {
					char title[48];
					snprintf(title, sizeof(title), "Reinstalling %s", PAK_UI[pak_kind].name);
					set_progress(title, "Preserving your original backup.");
					if (NS_pakInstall(pak_kind, status, sizeof(status)))
						snprintf(status, sizeof(status), "%s reinstalled.", PAK_UI[pak_kind].name);
				} else NS_pakIgnoreMismatch(pak_kind, status, sizeof(status));
				screen = pak_back_screen();
				dirty = 1;
			}
			break;

		case SCREEN_PAK_RESTORE:
			if (pak_kind == NS_PAK_NDS) {
				if (PAD_justPressed(BTN_UP) || PAD_justPressed(BTN_DOWN)) { restore_sel = !restore_sel; dirty = 1; }
				if (PAD_justPressed(BTN_B)) {
					if (restore_then_off) turn_off_from((NS_ManagedPak)(pak_kind + 1));
					else screen = pak_back_screen();
					dirty = 1;
				}
				if (PAD_justPressed(BTN_A)) {
					if (restore_sel == 0) {
						set_progress("Removing DS local wireless", "Putting NDS.pak's own files back.");
						if (NS_pakRestore(pak_kind, true, false, false, status, sizeof(status)))
							pak_restore_done(true);
						else screen = SCREEN_PAK_RESTORE;
					} else pak_restore_done(false);
					dirty = 1;
				}
				break;
			}
			if (PAD_justPressed(BTN_UP))   { restore_sel = (restore_sel + 4) % 5; dirty = 1; }
			if (PAD_justPressed(BTN_DOWN)) { restore_sel = (restore_sel + 1) % 5; dirty = 1; }
			if (PAD_justPressed(BTN_B)) {
				if (restore_then_off) turn_off_from((NS_ManagedPak)(pak_kind + 1));
				else screen = pak_back_screen();
				dirty = 1;
			}
			if (PAD_justPressed(BTN_A)) {
				if (restore_sel < 3) restore_choice[restore_sel] = !restore_choice[restore_sel];
				else if (restore_sel == 3) {
					char title[48];
					snprintf(title, sizeof(title), "Restoring %s", PAK_UI[pak_kind].name);
					set_progress(title, "Preserving files that would be overwritten.");
					if (NS_pakRestore(pak_kind, restore_choice[0], restore_choice[1], restore_choice[2], status, sizeof(status)))
						pak_restore_done(restore_choice[0]);
					else screen = SCREEN_PAK_RESTORE;
				} else {
					pak_restore_done(false);
				}
				dirty = 1;
			}
			break;

		case SCREEN_CHECKS:
			/* This screen had no input case at all: it drew "B BACK" and
			 * nothing read B, so opening it was a dead end that needed the
			 * device power-cycled. The render switch handles every screen and
			 * this one did not, which is exactly the asymmetry to watch for
			 * when adding a screen. */
			if (PAD_justPressed(BTN_B) || PAD_justPressed(BTN_A)) {
				screen = SCREEN_DEBUG;
				dirty = 1;
			}
			break;

		case SCREEN_JOINING: {
			int n = NS_discoverTick(peers, NS_MAX_PEERS);
			if (n != peer_count) { peer_count = n; dirty = 1; }
			dedupe_adhoc();
			join_total = peer_count + adhoc_count;
			if (peer_sel >= join_total) peer_sel = join_total ? join_total - 1 : 0;

			if (PAD_justPressed(BTN_UP) && join_total)   { peer_sel = (peer_sel + join_total - 1) % join_total; dirty = 1; }
			if (PAD_justPressed(BTN_DOWN) && join_total) { peer_sel = (peer_sel + 1) % join_total; dirty = 1; }
			if (PAD_justPressed(BTN_B)) { NS_discoverStop(); screen = SCREEN_MENU; dirty = 1; }

			/* A scan costs ~2s and briefly steals the radio, so it is on demand
			 * rather than on a timer - discovery keeps updating meanwhile. */
			if (PAD_justPressed(BTN_Y)) {
				snprintf(status, sizeof(status), "Scanning for ad hoc networks...");
				render(s);
				adhoc_count = NS_scanAdhoc(adhoc, NS_MAX_PEERS);
				int scanned_count = adhoc_count;
				dedupe_adhoc();
				join_total = peer_count + adhoc_count;
				snprintf(status, sizeof(status), "%d ad hoc network(s) found.", scanned_count);
				dirty = 1;
			}

			if (PAD_justPressed(BTN_A) && join_total) {
				NS_Peer* pr = join_at(peer_sel);
				NS_discoverStop();

				if (pr->hotspot) {
					char err[96] = "";
					// Credentials are fixed, so a network found by scan - or an
					// announcement that predates this build - is still joinable.
					const char* ssid = pr->ssid[0] ? pr->ssid : NS_ADHOC_SSID;
					const char* psk  = pr->psk[0]  ? pr->psk  : NS_ADHOC_PSK;

					snprintf(status, sizeof(status), "Joining %s...", ssid);
					render(s);   // this blocks for a while; say so first
					if (!NS_hotspotJoin(ssid, psk, err, sizeof(err))) {
						// NS_hotspotJoin has already put the old network back.
						snprintf(status, sizeof(status), "%s", err);
						screen = SCREEN_MENU;
						dirty = 1;
						break;
					}
					// The host's address on its own network is fixed, so we
					// already know the peer even though its old one is gone.
					do_arm(NS_ROLE_CLIENT, NS_HOTSPOT_HOST_IP);
				} else {
					do_arm(NS_ROLE_CLIENT, pr->ip);
				}
				dirty = 1;
			}
			break;
		}

		}

		PWR_update(&dirty, NULL, NULL, NULL);

		if (dirty) { render(s); dirty = 0; }
		else GFX_sync();
	}

	NS_announceStop();
	NS_compatServeStop();
	NS_discoverStop();

	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	return 0;
}
