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

#include "chordcap.h"
#include "hints.h"
#include "netsetup.h"

typedef enum {
	SCREEN_MENU,
	SCREEN_HOST_MENU,
	SCREEN_CONFIG,
	SCREEN_CORES,
	SCREEN_CORE_LIST,       /* Base or Extras, per cores_extras */
	SCREEN_CORE_OPTIONS,    /* Gambatte's or mGBA's, per opt_core */
	SCREEN_SETTINGS,
	SCREEN_DEBUG,
	SCREEN_CHORD,
	SCREEN_PAK_INSTALL,     /* the managed-pak screens serve pak_kind */
	SCREEN_PAK_MISMATCH,
	SCREEN_PAK_RESTORE,
	SCREEN_PAK_REMOVE,
	SCREEN_REMOVE_CONFIRM,
	SCREEN_POWERSAVE_CONFIRM,
	SCREEN_VARNISH_CONFIRM,
	SCREEN_VARNISH_REBOOT,
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

/* Text with its vertical centre at cy; returns its width. */
static int blit_text(SDL_Surface* s, TTF_Font* f, const char* text, int x, int cy, SDL_Color color) {
	if (!text[0]) return 0;
	SDL_Surface* t = TTF_RenderUTF8_Blended(f, text, color);
	if (!t) return 0;
	int w = t->w;
	SDL_BlitSurface(t, NULL, s, &(SDL_Rect){x, cy - t->h / 2, 0, 0});
	SDL_FreeSurface(t);
	return w;
}

static int text_w(TTF_Font* f, const char* text) {
	int w = 0;
	if (text[0]) TTF_SizeUTF8(f, text, &w, NULL);
	return w;
}

/* Small text wrapped to the screen width, at most max_lines lines. With
 * lines non-NULL nothing is drawn; *lines gets how many it would take. */
static int wrap_text(SDL_Surface* s, const char* text, int y, SDL_Color color, int max_lines, int* lines) {
	int x = SCALE1(PADDING + BUTTON_PADDING);
	int width = s->w - x - SCALE1(PADDING);
	char line[256];
	const char* p = text;
	for (int n = 0; *p && n < max_lines; n++) {
		int len = 0, fit = 0;
		while (p[len]) {
			int next = len;
			while (p[next] && p[next] != ' ') next++;
			snprintf(line, sizeof(line), "%.*s", next, p);
			if (fit && text_w(font.small, line) > width) break;
			fit = next;
			len = next;
			if (!p[len]) break;
			len++;
		}
		if (!fit) fit = (int)strlen(p);
		snprintf(line, sizeof(line), "%.*s", fit, p);
		if (lines) ++*lines;
		else y = draw_line(s, line, y, color, true);
		p += fit;
		while (*p == ' ') p++;
	}
	return y;
}

static int draw_wrapped(SDL_Surface* s, const char* text, int y, SDL_Color color, int max_lines) {
	return wrap_text(s, text, y, color, max_lines, NULL);
}

static int wrapped_lines(SDL_Surface* s, const char* text, int max_lines) {
	int n = 0;
	wrap_text(s, text, 0, COLOR_GRAY, max_lines, &n);
	return n;
}

//////////////////////////////////////////////////////////////////////////////
// two-column rows
//
// A label on the left and, on the right, what it is set to: a value, status
// marks, ">" for more behind it. The selection pill sits on the right column
// when the row has options to step through, and on the label when the row is
// something to do or somewhere to go.
//////////////////////////////////////////////////////////////////////////////

typedef enum { ROW_ACTION, ROW_SUBMENU, ROW_VALUE } RowKind;
enum { MARK_NONE = 0, MARK_CHECK = 1, MARK_DOUBLE = 2, MARK_CIRCLE = -1, MARK_DASH = -2 };

typedef struct {
	RowKind     kind;
	int         id;
	char        label[64];
	char        value[40];
	int         mark;
	bool        more;      /* ">" after the value */
	bool        dim;
	const char* hint;
} Row;

#define MAX_ROWS 16
static Row rows[MAX_ROWS];
static int row_count;

static Row* add_row(RowKind kind, int id, const char* label, const char* hint) {
	if (row_count >= MAX_ROWS) return &rows[MAX_ROWS - 1];
	Row* r = &rows[row_count++];
	memset(r, 0, sizeof(*r));
	r->kind = kind;
	r->id = id;
	snprintf(r->label, sizeof(r->label), "%s", label);
	r->hint = hint ? hint : "";
	if (kind == ROW_SUBMENU) r->more = true;
	return r;
}

static void fill(SDL_Surface* s, int x, int y, int w, int h, SDL_Color c) {
	SDL_FillRect(s, &(SDL_Rect){x, y, w, h}, SDL_MapRGB(s->format, c.r, c.g, c.b));
}

static void segment(SDL_Surface* s, int x0, int y0, int x1, int y1, int t, SDL_Color c) {
	int dx = abs(x1 - x0), dy = abs(y1 - y0);
	int steps = dx > dy ? dx : dy;
	if (steps < 1) steps = 1;
	for (int i = 0; i <= steps; i++)
		fill(s, x0 + (x1 - x0) * i / steps - t / 2, y0 + (y1 - y0) * i / steps - t / 2, t, t, c);
}

/* Drawn rather than typeset: NextUI's fonts need not carry these glyphs. */
static void draw_mark(SDL_Surface* s, int mark, int x, int cy, int size, SDL_Color c) {
	int t = SCALE1(2);
	if (mark == MARK_CHECK) {
		segment(s, x + t, cy, x + size * 2 / 5, cy + size * 2 / 5 - t, t, c);
		segment(s, x + size * 2 / 5, cy + size * 2 / 5 - t, x + size - t, cy - size / 2 + t, t, c);
	} else if (mark == MARK_CIRCLE) {
		int r = size / 2 - t / 2, cx = x + size / 2;
		for (int py = -r - t; py <= r + t; py++)
			for (int px = -r - t; px <= r + t; px++) {
				int d2 = px * px + py * py;
				if (d2 <= (r + t / 2) * (r + t / 2) && d2 >= (r - t / 2 - 1) * (r - t / 2 - 1))
					fill(s, cx + px, cy + py, 1, 1, c);
			}
	} else if (mark == MARK_DASH) {
		fill(s, x + size / 4, cy - t / 2, size / 2, t, c);
	}
}

static int draw_two_col(SDL_Surface* s, const Row* r, int y, bool selected) {
	int row_h = SCALE1(PILL_SIZE);
	int pad = SCALE1(BUTTON_PADDING);
	int x = SCALE1(PADDING);
	int right = s->w - SCALE1(PADDING);
	int cy = y + row_h / 2;
	int gap = SCALE1(4);
	SDL_Color base = r->dim ? COLOR_GRAY : COLOR_WHITE;

	/* The right column, measured first: marks, value, ">". */
	int size = TTF_FontHeight(font.large) * 3 / 5;
	int marks = r->mark == MARK_DOUBLE ? 2 : r->mark ? 1 : 0;
	int mw = marks ? marks * size + (marks - 1) * gap : 0;
	int vw = text_w(font.large, r->value);
	int gw = r->more ? text_w(font.large, ">") : 0;
	int group = mw + vw + gw;
	if (mw && vw) group += gap * 2;
	if (gw && (mw || vw)) group += gap * 2;
	int gx = right - pad - group;

	bool pill_right = selected && r->kind == ROW_VALUE && group;
	bool pill_left = selected && !pill_right;

	/* The label shrinks to the medium font rather than run into the value. */
	TTF_Font* lf = font.large;
	int lw = text_w(lf, r->label);
	if (x + pad + lw + SCALE1(8) > gx - (pill_right ? pad : 0)) {
		lf = font.medium;
		lw = text_w(lf, r->label);
	}
	if (pill_left) {
		int pw = lw + pad * 2;
		if (pw > right - x) pw = right - x;
		GFX_blitPill(ASSET_WHITE_PILL, s, &(SDL_Rect){x, y, pw, row_h});
	}
	blit_text(s, lf, r->label, x + pad, cy, pill_left ? COLOR_BLACK : base);

	if (pill_right)
		GFX_blitPill(ASSET_WHITE_PILL, s, &(SDL_Rect){gx - pad, y, group + pad * 2, row_h});
	SDL_Color rc = pill_right ? COLOR_BLACK : base;
	int cx = gx;
	for (int i = 0; i < marks; i++) {
		draw_mark(s, r->mark == MARK_DOUBLE ? MARK_CHECK : r->mark, cx, cy, size, rc);
		cx += size + gap;
	}
	if (marks) cx += gap - (vw ? 0 : gap);
	if (vw) cx += blit_text(s, font.large, r->value, cx, cy, rc) + gap * 2;
	if (gw) blit_text(s, font.large, ">", cx, cy, rc);
	return y + row_h;
}

/* The rows, scrolled so the selection is visible above footer_lines lines of
 * small text (hint, status) and the button bar. */
static int draw_rows(SDL_Surface* s, int sel, int* top, int y, int footer_lines) {
	/* Down to the top of the button bar, less the footer's lines. */
	int line_h = TTF_FontHeight(font.small) + SCALE1(4);
	int bottom = s->h - SCALE1(PADDING + PILL_SIZE) - SCALE1(2) - line_h * footer_lines;
	int visible = (bottom - y) / SCALE1(PILL_SIZE);
	if (visible < 1) visible = 1;
	if (visible > row_count) visible = row_count;
	if (sel < *top) *top = sel;
	if (sel >= *top + visible) *top = sel - visible + 1;
	if (*top > row_count - visible) *top = row_count - visible;
	if (*top < 0) *top = 0;
	for (int i = *top; i < *top + visible; i++)
		y = draw_two_col(s, &rows[i], y, i == sel);
	return y;
}

/* Hint for the selected row and the status line, under the list. */
static int draw_footer(SDL_Surface* s, int sel, int y);

//////////////////////////////////////////////////////////////////////////////
// screens
//////////////////////////////////////////////////////////////////////////////

/* Several short screens' worth of choices, not one long list.
 *
 * The flat menu had grown to seven entries and pushed the WiFi status line off
 * the bottom, with no way to scroll to it - which is precisely the information
 * you need to tell an ad hoc session from a failed one. Submenus keep the top
 * level short enough that the status line is always visible. */
enum { MENU_HOST, MENU_JOIN, MENU_CONFIG, MENU_COUNT };
enum { HOST_ADHOC, HOST_WIFI, HOST_COUNT };
enum { CONF_CORES, CONF_SETTINGS, CONF_DEBUG, CONF_OFF };
enum { CORES_BASE, CORES_EXTRAS };
/* "Use simple client" (NS_Settings.simple_client) stays hidden: it was planned
 * but never implemented. The setting is still read and written. */
enum { SET_QUICK_MENU, SET_STATUS_ICON, SET_COMPAT, SET_GAMESWITCHER, SET_POWERSAVE };
enum { DEBUG_FORCE_COMPAT, DEBUG_VERBOSE_LOGS, DEBUG_CHECKS, DEBUG_RESTORE_WIFI };
enum { OPT_DUAL, OPT_INSTANCED, OPT_REMOVE };
enum { REM_NETPLAY, REM_DEVICE, REM_BACKUP, REM_CANCEL };

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

static int config_sel = 0, config_top = 0;
static int cores_sel = 0, cores_top = 0;
static int list_sel = 0, list_top = 0;
static int settings_sel = 0, settings_top = 0;
static int debug_sel = 0, debug_top = 0;
static int opt_sel = 0, opt_top = 0;
static int remove_sel = 0, remove_top = 0;
static int menu_top = 0;
static int prompt_sel = 0;
static int restore_sel = 0;
static bool restore_choice[3] = { true, true, true };
static bool restore_then_off = false;
static NS_PakStatus pak_status;

/* The status icon and quick menu are drawn by Varnish.pak. Switching either on
 * offers to install (or update) the copy Netplay carries; the setting changes
 * only once it is in place. */
static int varnish_for;                 /* the setting waiting on it */
static NS_VarnishState varnish_state;

static bool needs_varnish(int setting) {
	varnish_state = NS_varnishStatus();
	if (varnish_state == NS_VARNISH_CURRENT) return false;
	if (varnish_state == NS_VARNISH_UNAVAILABLE) {
		snprintf(status, sizeof(status), "Needs Varnish.pak, which is not available here.");
		return true;
	}
	varnish_for = setting;
	screen = SCREEN_VARNISH_CONFIRM;
	return true;
}

/* Which managed pak the install, mismatch, restore and remove screens are
 * about, and where they go back to. */
static NS_ManagedPak pak_kind = NS_PAK_MGBA;
static Screen pak_return = SCREEN_MENU;
static const struct {
	const char* name;      /* in sentences */
	const char* pak;       /* the Emus/<p>/<pak>.pak directory */
	const char* binary;    /* the file whose build is compared */
	const char* install_title;
	const char* needs;
} PAK_UI[NS_PAK_COUNT] = {
	{ "mGBA", "MGBA", "mgba_libretro.so", "Install instanced mGBA?", "Instanced mGBA needs Netplay's MGBA.pak." },
	{ "DSperate", "DSP", "dsperate", "Install DSperate?", "DS local wireless needs DSP.pak (DSperate)." },
	{ "DraStic", "NDS", "libdrastic_wifi.so", "Add DS local wireless?", "Netplay adds its radio and launcher to NDS.pak." },
};

//////////////////////////////////////////////////////////////////////////////
// cores
//
// What each system's row shows: a check when it plays over Netplay here, two
// when it plays in instanced (dual) mode, a hollow circle when its pak is here
// but not set up for Netplay, a dash when there is no pak. Status only, except
// where Netplay installs or configures something (">" or a pak to add).
//////////////////////////////////////////////////////////////////////////////

typedef enum { CK_SHIM, CK_GAMBATTE, CK_DSP, CK_MGBA, CK_NDS } CoreKind;

typedef struct {
	const char* tag;        /* the pak, and the key into pak_hints.json */
	const char* tag2;       /* a second pak the row covers (GBC) */
	const char* label;      /* when the row's name is not the tag */
	const char* core;       /* the core it names when its pak is absent */
	CoreKind    kind;
} CoreDef;

static const CoreDef BASE_CORES[] = {
	{ "FC",  NULL,  NULL,     "fceumm",       CK_SHIM },
	{ "GB",  "GBC", "GB/GBC", "gambatte",     CK_GAMBATTE },
	{ "GBA", NULL,  NULL,     "gpsp",         CK_SHIM },
	{ "MD",  NULL,  NULL,     "picodrive",    CK_SHIM },
	{ "PS",  NULL,  NULL,     "pcsx_rearmed", CK_SHIM },
	{ "SFC", NULL,  NULL,     "snes9x",       CK_SHIM },
};
static const CoreDef EXTRA_CORES[] = {
	{ "32X",    NULL, NULL, "picodrive",          CK_SHIM },
	{ "DSP",    NULL, NULL, "dsperate",           CK_DSP },
	{ "FBN",    NULL, NULL, "fbneo",              CK_SHIM },
	{ "FDS",    NULL, NULL, "fceumm",             CK_SHIM },
	{ "GG",     NULL, NULL, "picodrive",          CK_SHIM },
	{ "MGBA",   NULL, NULL, "mgba",               CK_MGBA },
	{ "NDS",    NULL, NULL, "drastic",            CK_NDS },
	{ "SEGACD", NULL, NULL, "picodrive",          CK_SHIM },
	{ "SG1000", NULL, NULL, "picodrive",          CK_SHIM },
	{ "SMS",    NULL, NULL, "picodrive",          CK_SHIM },
	{ "SUPA",   NULL, NULL, "mednafen_supafaust", CK_SHIM },
};
#define BASE_COUNT  (int)(sizeof(BASE_CORES) / sizeof(BASE_CORES[0]))
#define EXTRA_COUNT (int)(sizeof(EXTRA_CORES) / sizeof(EXTRA_CORES[0]))

static const char* core_name(const char* core) {
	static const struct { const char* id; const char* name; } NAMES[] = {
		{ "fceumm", "FCEUmm" }, { "gambatte", "Gambatte" }, { "gpsp", "gpSP" },
		{ "picodrive", "PicoDrive" }, { "pcsx_rearmed", "PCSX ReARMed" },
		{ "snes9x", "Snes9x" }, { "snes9x2005", "Snes9x 2005" },
		{ "mednafen_supafaust", "Supafaust" }, { "fbneo", "FinalBurn Neo" },
		{ "mgba", "mGBA" }, { "dsperate", "DSperate" }, { "drastic", "DraStic" },
	};
	for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
		if (!strcmp(core, NAMES[i].id)) return NAMES[i].name;
	return core;
}

typedef struct {
	int         mark;
	bool        more;
	bool        dim;
	const char* state;      /* pak_hints.json's cores.<tag>.<state> */
	char        core[32];
	NS_PakStatus pak;       /* managed paks only */
	bool        pak_ok;     /* pak status could be read */
} CoreState;

static bool cores_extras;
static CoreState core_state[16];
static bool cores_fresh;    /* core_state matches the card */

static const CoreDef* list_defs(int* n) {
	*n = cores_extras ? EXTRA_COUNT : BASE_COUNT;
	return cores_extras ? EXTRA_CORES : BASE_CORES;
}

static bool gambatte_dual(void) {
	return NS_settings()->inst_core[0] && NS_gambatteDualAvailable();
}

static bool mgba_instanced(void) {
	return NS_settings()->inst_core[1];
}

/* Store the instanced choices in the settings' existing shape. */
static void set_instanced(int core, bool on) {
	NS_Settings* c = NS_settings();
	if (c->instanced == NS_INST_ALL)
		for (int i = 0; i < NS_INST_CORES; i++) c->inst_core[i] = true;
	c->inst_core[core] = on;
	bool any = false;
	for (int i = 0; i < NS_INST_CORES; i++) any = any || c->inst_core[i];
	c->instanced = any ? NS_INST_SELECTED : NS_INST_OFF;
	NS_settingsSave();
}

static void refresh_core(const CoreDef* d, CoreState* st) {
	memset(st, 0, sizeof(*st));
	snprintf(st->core, sizeof(st->core), "%s", d->core);
	switch (d->kind) {
	case CK_SHIM:
	case CK_GAMBATTE: {
		char core[32];
		NS_SystemState sys = NS_systemStatus(d->tag, core, sizeof(core));
		if (d->tag2 && sys != NS_SYS_READY) {
			char core2[32];
			NS_SystemState sys2 = NS_systemStatus(d->tag2, core2, sizeof(core2));
			if (sys2 > sys) { sys = sys2; snprintf(core, sizeof(core), "%s", core2); }
		}
		if (core[0]) snprintf(st->core, sizeof(st->core), "%s", core);
		if (sys == NS_SYS_READY) {
			bool dual = d->kind == CK_GAMBATTE && gambatte_dual();
			st->mark = dual ? MARK_DOUBLE : MARK_CHECK;
			st->state = dual ? "dual" : "ready";
		} else if (sys == NS_SYS_PRESENT) {
			st->mark = MARK_CIRCLE;
			st->state = "present";
		} else {
			st->mark = MARK_DASH;
			st->dim = true;
			st->state = "absent";
		}
		st->more = d->kind == CK_GAMBATTE && sys == NS_SYS_READY;
		break;
	}
	case CK_DSP:
	case CK_MGBA:
	case CK_NDS: {
		NS_ManagedPak k = d->kind == CK_DSP ? NS_PAK_DSP : d->kind == CK_MGBA ? NS_PAK_MGBA : NS_PAK_NDS;
		if (!NS_pakAvailable(k)) {
			st->mark = MARK_DASH;
			st->dim = true;
			st->state = "unavailable";
			break;
		}
		st->pak_ok = NS_pakStatus(k, &st->pak);
		const NS_PakStatus* p = &st->pak;
		bool ours = p->managed && p->installed && !p->missing;
		if (d->kind == CK_DSP) {
			if (ours && NS_settings()->dsperate) { st->mark = MARK_CHECK; st->state = "ready"; }
			else if (p->present) { st->mark = MARK_CIRCLE; st->state = "off"; }
			else { st->mark = MARK_DASH; st->state = "absent"; }
		} else if (d->kind == CK_MGBA) {
			st->more = true;
			if (ours && mgba_instanced()) { st->mark = MARK_DOUBLE; st->state = "instanced"; }
			else if (p->present) { st->mark = MARK_CIRCLE; st->state = "installed"; }
			else { st->mark = MARK_DASH; st->state = "absent"; }
		} else {
			if (ours) { st->mark = MARK_CHECK; st->state = "ready"; }
			else if (p->base_installed && p->unsupported) { st->mark = MARK_CIRCLE; st->state = "unsupported"; }
			else if (p->base_installed) { st->mark = MARK_CIRCLE; st->state = "present"; }
			else { st->mark = MARK_DASH; st->state = "absent"; }
		}
		break;
	}
	}
}

/* Reading every pak's status costs a few hashes and launch.sh reads, so it is
 * done on entering the list and after a change, never per frame. */
static void refresh_cores(void) {
	int n;
	const CoreDef* defs = list_defs(&n);
	for (int i = 0; i < n; i++) refresh_core(&defs[i], &core_state[i]);
	cores_fresh = true;
}

static void open_core_list(bool extras) {
	cores_extras = extras;
	list_sel = list_top = 0;
	status[0] = '\0';
	refresh_cores();
	screen = SCREEN_CORE_LIST;
}

static void build_core_rows(void) {
	int n;
	const CoreDef* defs = list_defs(&n);
	if (!cores_fresh) refresh_cores();
	row_count = 0;
	for (int i = 0; i < n; i++) {
		const CoreDef* d = &defs[i];
		const CoreState* st = &core_state[i];
		char label[64];
		snprintf(label, sizeof(label), "%s (%s)", d->label ? d->label : d->tag, core_name(st->core));
		Row* r = add_row(ROW_VALUE, i, label, HINT_getf("cores.%s.%s", d->tag, st->state));
		r->mark = st->mark;
		r->more = st->more;
		r->dim = st->dim;
	}
}

/* Gambatte's and mGBA's own screen. */
static CoreKind opt_core = CK_GAMBATTE;

static void build_option_rows(void) {
	row_count = 0;
	if (opt_core == CK_GAMBATTE) {
		bool avail = NS_gambatteDualAvailable();
		bool on = gambatte_dual();
		Row* r = add_row(ROW_VALUE, OPT_DUAL, "Dual instance",
		                 HINT_get(!avail ? "gambatte.dual.unavailable" : on ? "gambatte.dual.on" : "gambatte.dual.off"));
		snprintf(r->value, sizeof(r->value), "%s", !avail ? "Unavailable" : on ? "On" : "Off");
		r->dim = !avail;
	} else {
		bool on = mgba_instanced() && pak_status.managed && pak_status.installed && !pak_status.missing;
		bool ready = pak_status.managed && pak_status.installed && !pak_status.missing;
		Row* r = add_row(ROW_VALUE, OPT_INSTANCED, "Instanced link",
		                 HINT_get(on ? "mgba.instanced.on" : ready ? "mgba.instanced.off" : "mgba.instanced.install"));
		snprintf(r->value, sizeof(r->value), "%s", on ? "On" : "Off");
		if (pak_status.managed)
			add_row(ROW_ACTION, OPT_REMOVE, "Remove MGBA.pak", HINT_get("mgba.remove"));
	}
}

static void build_remove_rows(void) {
	const char* tag = PAK_UI[pak_kind].pak;
	row_count = 0;
	add_row(ROW_ACTION, REM_NETPLAY, "Remove from Netplay", HINT_getf("remove.netplay.%s", tag));
	add_row(ROW_ACTION, REM_DEVICE, "Remove from device", HINT_getf("remove.device.%s", tag));
	if (pak_kind == NS_PAK_DSP && pak_status.has_backup)
		add_row(ROW_ACTION, REM_BACKUP, "Restore backup", HINT_getf("remove.backup.%s", tag));
	add_row(ROW_ACTION, REM_CANCEL, "Cancel", HINT_get("remove.cancel"));
}

//////////////////////////////////////////////////////////////////////////////
// settings, debug, configuration
//////////////////////////////////////////////////////////////////////////////

static const char* const ICON_NAMES[] = { "Disabled", "Active Session", "Always" };

static void build_config_rows(void) {
	row_count = 0;
	add_row(ROW_SUBMENU, CONF_CORES, "Cores", HINT_get("config.cores"));
	add_row(ROW_SUBMENU, CONF_SETTINGS, "Settings", HINT_get("config.settings"));
	add_row(ROW_SUBMENU, CONF_DEBUG, "Debug", HINT_get("config.debug"));
	add_row(ROW_ACTION, CONF_OFF, "Turn Off Netplay", HINT_get("config.off"));
}

static void build_cores_rows(void) {
	row_count = 0;
	add_row(ROW_SUBMENU, CORES_BASE, "Base", HINT_get("cores.base"));
	add_row(ROW_SUBMENU, CORES_EXTRAS, "Extras", HINT_get("cores.extras"));
}

static const char* on_off(bool on) { return on ? "On" : "Off"; }

static void build_settings_rows(void) {
	NS_Settings* c = NS_settings();
	bool varnish = NS_varnishInstalled();
	const char* need = NS_varnishBundled() ? "needs_varnish" : "unavailable";
	row_count = 0;

	Row* r = add_row(ROW_VALUE, SET_QUICK_MENU, "Quick Menu",
	                 HINT_getf("settings.quick_menu.%s",
	                           c->quick_menu ? (c->quick_keys[0] ? "custom" : "on")
	                           : varnish ? "off" : need));
	if (c->quick_menu) {
		snprintf(r->value, sizeof(r->value), "%s", c->quick_label[0] ? c->quick_label : "START");
		r->more = true;
	} else snprintf(r->value, sizeof(r->value), "Off");

	static const char* const ICON_HINT[] = { "disabled", "session", "always" };
	r = add_row(ROW_VALUE, SET_STATUS_ICON, "Netplay Icon in NextUI",
	            HINT_getf("settings.icon.%s",
	                      c->status_icon == NS_ICON_OFF && !varnish ? need : ICON_HINT[c->status_icon]));
	snprintf(r->value, sizeof(r->value), "%s", ICON_NAMES[c->status_icon]);

	r = add_row(ROW_VALUE, SET_COMPAT, "Use compatibility cores",
	            HINT_getf("settings.compat.%s", c->compatibility_cores ? "on" : "off"));
	snprintf(r->value, sizeof(r->value), "%s", on_off(c->compatibility_cores));

	r = add_row(ROW_VALUE, SET_GAMESWITCHER, "Add Netplay to GameSwitcher",
	            HINT_getf("settings.gameswitcher.%s", c->add_gameswitcher ? "on" : "off"));
	snprintf(r->value, sizeof(r->value), "%s", on_off(c->add_gameswitcher));

	r = add_row(ROW_VALUE, SET_POWERSAVE, "Disable Wi-Fi Power Saving",
	            HINT_getf("settings.powersave.%s", c->disable_powersave ? "on" : "off"));
	snprintf(r->value, sizeof(r->value), "%s", on_off(c->disable_powersave));
}

static void build_debug_rows(void) {
	NS_Settings* c = NS_settings();
	row_count = 0;
	Row* r = add_row(ROW_VALUE, DEBUG_FORCE_COMPAT, "Force compatibility cores",
	                 HINT_getf("debug.force_compat.%s", c->force_compatibility ? "on" : "off"));
	snprintf(r->value, sizeof(r->value), "%s", on_off(c->force_compatibility));
	r = add_row(ROW_VALUE, DEBUG_VERBOSE_LOGS, "Verbose debugging logs",
	            HINT_getf("debug.verbose_logs.%s", c->verbose_logs ? "on" : "off"));
	snprintf(r->value, sizeof(r->value), "%s", on_off(c->verbose_logs));
	add_row(ROW_ACTION, DEBUG_CHECKS, "Run checks", HINT_get("debug.checks"));
	add_row(ROW_ACTION, DEBUG_RESTORE_WIFI, "Restore Wi-Fi", HINT_get("debug.restore_wifi"));
}

static void build_menu_rows(void) {
	row_count = 0;
	add_row(ROW_ACTION, MENU_HOST, "Host", HINT_get("menu.host"));
	add_row(ROW_ACTION, MENU_JOIN, "Join", HINT_get("menu.join"));
	add_row(ROW_ACTION, MENU_CONFIG, "Configuration", HINT_get("menu.config"));
}

/* The rows of the screen being shown, for drawing and for input alike. */
static void build_rows(void) {
	row_count = 0;
	switch (screen) {
	case SCREEN_MENU:         build_menu_rows(); break;
	case SCREEN_CONFIG:       build_config_rows(); break;
	case SCREEN_CORES:        build_cores_rows(); break;
	case SCREEN_CORE_LIST:    build_core_rows(); break;
	case SCREEN_CORE_OPTIONS: build_option_rows(); break;
	case SCREEN_SETTINGS:     build_settings_rows(); break;
	case SCREEN_DEBUG:        build_debug_rows(); break;
	case SCREEN_PAK_REMOVE:   build_remove_rows(); break;
	default: break;
	}
}

static int draw_footer(SDL_Surface* s, int sel, int y) {
	y += SCALE1(2);
	if (sel >= 0 && sel < row_count && rows[sel].hint[0])
		y = draw_wrapped(s, rows[sel].hint, y, COLOR_GRAY, 2);
	if (status[0]) y = draw_wrapped(s, status, y, COLOR_GRAY, 1);
	return y;
}

/* Steps a list selection, wrapping, with held-button repeat. */
static bool step_sel(int* sel, int n) {
	if (n <= 0) return false;
	if (PAD_justRepeated(BTN_UP))   { *sel = (*sel + n - 1) % n; return true; }
	if (PAD_justRepeated(BTN_DOWN)) { *sel = (*sel + 1) % n; return true; }
	if (*sel >= n) { *sel = n - 1; return true; }
	return false;
}

//////////////////////////////////////////////////////////////////////////////
// quick menu buttons
//
// Held, then released: whatever was held at once is the combination. The
// screen ignores the buttons it is capturing, so A and B mean something only
// once a combination is on screen; MENU, which can never be part of one, backs
// out.
//////////////////////////////////////////////////////////////////////////////

typedef enum { CS_WAIT_RELEASE, CS_CAPTURE, CS_CONFIRM } ChordStep;
static ChordStep chord_step;
static int  chord_codes[8];
static int  chord_code_count;
static uint32_t chord_mask;
static bool chord_axis;
static const char* chord_note = "";   /* why the last one was refused */
static char chord_label[32];

static const struct { int btn; const char* name; } CHORD_BUTTONS[] = {
	{ BTN_SELECT, "SELECT" }, { BTN_START, "START" },
	{ BTN_L1, "L1" }, { BTN_R1, "R1" }, { BTN_L2, "L2" }, { BTN_R2, "R2" },
	{ BTN_L3, "L3" }, { BTN_R3, "R3" },
	{ BTN_X, "X" }, { BTN_Y, "Y" }, { BTN_A, "A" }, { BTN_B, "B" },
	{ BTN_DPAD_UP, "UP" }, { BTN_DPAD_DOWN, "DOWN" }, { BTN_DPAD_LEFT, "LEFT" }, { BTN_DPAD_RIGHT, "RIGHT" },
	{ BTN_MENU, "MENU" }, { BTN_POWER, "POWER" }, { BTN_PLUS, "VOL+" }, { BTN_MINUS, "VOL-" },
};
#define CHORD_BUTTON_COUNT (int)(sizeof(CHORD_BUTTONS) / sizeof(CHORD_BUTTONS[0]))

static void open_chord(void) {
	CHORD_open();
	chord_step = CS_WAIT_RELEASE;
	chord_note = "";
	chord_code_count = 0;
	chord_mask = 0;
	chord_axis = false;
	screen = SCREEN_CHORD;
}

static void close_chord(void) {
	CHORD_close();
	screen = SCREEN_SETTINGS;
}

/* What the combination held is, or why it cannot be one (NULL if fine). */
static const char* chord_verdict(void) {
	int buttons = 0;
	bool only_nav = true;
	for (int i = 0; i < CHORD_BUTTON_COUNT; i++) {
		if (!(chord_mask & CHORD_BUTTONS[i].btn)) continue;
		buttons++;
		if (CHORD_BUTTONS[i].btn & (BTN_MENU | BTN_POWER | BTN_PLUS | BTN_MINUS))
			return HINT_get("chord.refused_system");
		if (!(CHORD_BUTTONS[i].btn & (BTN_A | BTN_B | BTN_DPAD_UP | BTN_DPAD_DOWN |
		                              BTN_DPAD_LEFT | BTN_DPAD_RIGHT)))
			only_nav = false;
	}
	for (int i = 0; i < chord_code_count; i++)
		if (CHORD_isSystemKey(chord_codes[i])) return HINT_get("chord.refused_system");
	if (chord_axis || buttons > chord_code_count || buttons == 0) return HINT_get("chord.refused_axis");
	if (buttons > 3) return HINT_get("chord.refused_many");
	if (buttons == 1 && only_nav) return HINT_get("chord.refused_single");
	return NULL;
}

static void chord_tick(void) {
	CHORD_poll();
	int codes[8];
	int n = CHORD_held(codes, 8);
	bool axis = CHORD_axisHeld();
	uint32_t mask = 0;
	for (int i = 0; i < CHORD_BUTTON_COUNT; i++)
		if (PAD_isPressed(CHORD_BUTTONS[i].btn)) mask |= CHORD_BUTTONS[i].btn;
	bool any = n || axis || mask;

	switch (chord_step) {
	case CS_WAIT_RELEASE:
		if (!any) chord_step = CS_CAPTURE;
		break;
	case CS_CAPTURE:
		if (any) {
			for (int i = 0; i < n; i++) {
				bool seen = false;
				for (int j = 0; j < chord_code_count; j++) seen = seen || chord_codes[j] == codes[i];
				if (!seen && chord_code_count < 8) chord_codes[chord_code_count++] = codes[i];
			}
			chord_mask |= mask;
			chord_axis = chord_axis || axis;
		} else if (chord_mask || chord_code_count || chord_axis) {
			if (chord_mask == BTN_MENU) { close_chord(); break; }   /* MENU alone backs out */
			const char* why = chord_verdict();
			if (why) {
				chord_note = why;
			} else {
				chord_label[0] = '\0';
				for (int i = 0; i < CHORD_BUTTON_COUNT; i++) {
					if (!(chord_mask & CHORD_BUTTONS[i].btn)) continue;
					if (chord_label[0]) strncat(chord_label, "+", sizeof(chord_label) - strlen(chord_label) - 1);
					strncat(chord_label, CHORD_BUTTONS[i].name, sizeof(chord_label) - strlen(chord_label) - 1);
				}
				chord_note = "";
				chord_step = CS_CONFIRM;
			}
			if (chord_step != CS_CONFIRM) {
				chord_code_count = 0;
				chord_mask = 0;
				chord_axis = false;
			}
		}
		break;
	case CS_CONFIRM:
		if (PAD_justPressed(BTN_A)) {
			NS_Settings* c = NS_settings();
			if (!strcmp(chord_label, "START")) {
				/* START is the default, kept per platform by the quick menu itself. */
				c->quick_keys[0] = c->quick_label[0] = '\0';
			} else {
				char keys[32] = "";
				for (int i = 0; i < chord_code_count; i++) {
					char one[12];
					snprintf(one, sizeof(one), "%s%d", i ? "," : "", chord_codes[i]);
					strncat(keys, one, sizeof(keys) - strlen(keys) - 1);
				}
				snprintf(c->quick_keys, sizeof(c->quick_keys), "%s", keys);
				snprintf(c->quick_label, sizeof(c->quick_label), "%s", chord_label);
			}
			NS_settingsSave();
			snprintf(status, sizeof(status), "The quick menu opens with %s.", chord_label);
			close_chord();
		} else if (PAD_justPressed(BTN_B)) {
			chord_step = CS_WAIT_RELEASE;
			chord_code_count = 0;
			chord_mask = 0;
			chord_axis = false;
		}
		break;
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
			screen = SCREEN_CONFIG;
			return;
		}
		if (!pak_status.managed) continue;
		pak_kind = (NS_ManagedPak)k;
		restore_choice[0] = restore_choice[1] = restore_choice[2] = true;
		restore_sel = 0;
		restore_then_off = true;
		pak_return = SCREEN_CONFIG;
		screen = SCREEN_PAK_RESTORE;
		return;
	}
	turn_off_now();
}

static void pak_done(void) {
	cores_fresh = false;
	if (pak_return == SCREEN_CORE_OPTIONS) NS_pakStatus(pak_kind, &pak_status);
	screen = pak_return;
}

/* After a restore screen is done with: the setting it governs follows the pak. */
static void pak_restore_done(bool pak_restored) {
	if (!restore_then_off || pak_restored) {
		if (pak_kind == NS_PAK_MGBA) set_instanced(1, false);
		else if (pak_kind == NS_PAK_DSP) { NS_settings()->dsperate = false; NS_settingsSave(); }
		else { NS_settings()->drastic = false; NS_settingsSave(); }
	}
	if (restore_then_off) turn_off_from((NS_ManagedPak)(pak_kind + 1));
	else pak_done();
}

/* B on a restore screen cancels, as its button says: when turning Netplay off
 * that means not turning it off at all. "Keep installed" is how to go on
 * without restoring one pak. */
static void cancel_restore(void) {
	if (restore_then_off) {
		restore_then_off = false;
		snprintf(status, sizeof(status), "Netplay is still on.");
		screen = SCREEN_CONFIG;
	} else pak_done();
}

/* A on a DS row: set it up for Netplay, or offer to take it out. */
static void ds_row_action(NS_ManagedPak k, const CoreState* st) {
	pak_kind = k;
	pak_return = SCREEN_CORE_LIST;
	if (!st->pak_ok) {
		snprintf(status, sizeof(status), "Could not check %s.pak - try again.", PAK_UI[k].pak);
		return;
	}
	pak_status = st->pak;
	if (st->mark == MARK_CHECK) {
		remove_sel = remove_top = 0;
		screen = SCREEN_PAK_REMOVE;
		return;
	}
	if (k == NS_PAK_DSP) {
		/* Netplay's own DSP.pak, only switched off: switch it back on. */
		if (pak_status.managed && pak_status.installed && !pak_status.missing && !pak_status.mismatch) {
			NS_settings()->dsperate = true;
			NS_settingsSave();
			snprintf(status, sizeof(status), "DSperate plays over Netplay again.");
			cores_fresh = false;
			return;
		}
	} else if (pak_status.base_installed && pak_status.unsupported) {
		snprintf(status, sizeof(status), "This NDS.pak's DraStic build is not supported.");
		return;
	} else if (!pak_status.base_installed && !pak_status.base_bundled) {
		snprintf(status, sizeof(status), "Install NDS.pak (DraStic) first.");
		return;
	}
	prompt_sel = 0;
	screen = SCREEN_PAK_INSTALL;
}

static void draw_progress(const char* stage, int step, int steps);

static void run_pak_install(void) {
	char title[48];
	snprintf(title, sizeof(title), "Installing %s", PAK_UI[pak_kind].name);
	set_progress(title, pak_kind == NS_PAK_NDS && !pak_status.base_installed
	                    ? "Copying NDS.pak. This takes a minute."
	                    : "Backing up files and verifying builds.");
	draw_progress("Installing", 1, 1);
	if (NS_pakInstall(pak_kind, status, sizeof(status))) {
		if (pak_kind == NS_PAK_MGBA) {
			set_instanced(1, true);
			snprintf(status, sizeof(status), "Instanced mGBA installed.");
		} else if (pak_kind == NS_PAK_DSP) {
			NS_settings()->dsperate = true;
			NS_settingsSave();
			snprintf(status, sizeof(status), "DSperate installed.");
		} else {
			NS_settings()->drastic = true;
			NS_settingsSave();
			snprintf(status, sizeof(status), pak_status.base_installed
			         ? "DS local wireless added to NDS.pak." : "NDS.pak installed with local wireless.");
		}
	}
	set_progress("Joining", "This can take up to a minute.");
	pak_done();
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

/* A centred-ish two-choice prompt's rows: Continue / Cancel and the like. */
static int draw_choice(SDL_Surface* s, int y, const char* a, const char* b, int sel) {
	Row r = { .kind = ROW_ACTION };
	snprintf(r.label, sizeof(r.label), "%s", a);
	y = draw_two_col(s, &r, y, sel == 0);
	snprintf(r.label, sizeof(r.label), "%s", b);
	return draw_two_col(s, &r, y, sel == 1);
}

static void render(SDL_Surface* s) {
	GFX_clear(s);

	/* Battery and the WiFi signal icon, in the standard top-right position. */
	GFX_blitHardwareGroup(s, 0);
	build_rows();

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
			Row conf = { .kind = ROW_ACTION };
			snprintf(conf.label, sizeof(conf.label), "Configuration");
			y = draw_two_col(s, &conf, y + SCALE1(8), true);
			y = draw_wifi_line(s, y + SCALE1(8));
			if (status[0]) draw_line(s, status, y, COLOR_GRAY, true);
		} else {
			/* A one-line hint and the WiFi line, plus any status. */
			y = draw_rows(s, menu_sel, &menu_top, y, status[0] ? 3 : 2);
			y = draw_footer(s, menu_sel, y);
			draw_wifi_line(s, y + SCALE1(2));
		}
		GFX_blitButtonGroup(armed
		                    ? (char*[]){"B", "EXIT", "X", "END SESSION", "A", "SELECT", NULL}
		                    : (char*[]){"B", "EXIT", "A", "SELECT", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_HOST_MENU: {
		draw_title(s, "Host");
		int y = SCALE1(PADDING + 44);
		for (int i = 0; i < host_menu_count(); i++) {
			Row r = { .kind = ROW_ACTION };
			snprintf(r.label, sizeof(r.label), "%s", host_label(i));
			y = draw_two_col(s, &r, y, i == sub_sel);
		}
		if (!NS_canHostAdhoc())
			y = draw_line(s, "This device cannot create a network (no AP interface).",
			              y + SCALE1(6), COLOR_GRAY, true);
		y = draw_wifi_line(s, y + SCALE1(8));
		if (status[0]) draw_line(s, status, y, COLOR_GRAY, true);
		GFX_blitButtonGroup((char*[]){"B", "BACK", "A", "SELECT", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_CONFIG:
	case SCREEN_CORES:
	case SCREEN_CORE_LIST:
	case SCREEN_CORE_OPTIONS:
	case SCREEN_SETTINGS:
	case SCREEN_DEBUG:
	case SCREEN_PAK_REMOVE: {
		int* sel; int* top; const char* title; char buf[48];
		bool values = false;
		switch (screen) {
		case SCREEN_CONFIG:   sel = &config_sel; top = &config_top; title = "Configuration"; break;
		case SCREEN_CORES:    sel = &cores_sel; top = &cores_top; title = "Cores"; break;
		case SCREEN_CORE_LIST: sel = &list_sel; top = &list_top; title = cores_extras ? "Extras" : "Base"; break;
		case SCREEN_CORE_OPTIONS:
			sel = &opt_sel; top = &opt_top; title = opt_core == CK_GAMBATTE ? "Gambatte" : "mGBA";
			values = true;
			break;
		case SCREEN_SETTINGS: sel = &settings_sel; top = &settings_top; title = "Settings"; values = true; break;
		case SCREEN_DEBUG:    sel = &debug_sel; top = &debug_top; title = "Debug"; values = true; break;
		default:
			snprintf(buf, sizeof(buf), "Remove %s?", PAK_UI[pak_kind].name);
			sel = &remove_sel; top = &remove_top; title = buf;
			break;
		}
		draw_title(s, title);
		int y = SCALE1(PADDING + 44);
		if (*sel >= row_count) *sel = row_count ? row_count - 1 : 0;
		/* Room for the longest hint on this screen, so the list does not
		 * change length as the selection moves. */
		int hint_lines = 0;
		for (int i = 0; i < row_count; i++) {
			int n = wrapped_lines(s, rows[i].hint, 2);
			if (n > hint_lines) hint_lines = n;
		}
		y = draw_rows(s, *sel, top, y, hint_lines + (status[0] ? 1 : 0));
		draw_footer(s, *sel, y);
		bool lr = values && *sel < row_count && rows[*sel].kind == ROW_VALUE;
		GFX_blitButtonGroup(lr ? (char*[]){"B", "BACK", "A", "CHANGE", NULL}
		                       : (char*[]){"B", "BACK", "A", "SELECT", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_CHORD: {
		draw_title(s, "Quick Menu Buttons");
		int y = SCALE1(PADDING + 44);
		if (chord_step == CS_CONFIRM) {
			y = draw_line(s, chord_label, y, COLOR_WHITE, false);
			y += SCALE1(4);
			y = draw_wrapped(s, HINT_get("chord.warning"), y, COLOR_GRAY, 2);
			GFX_blitButtonGroup((char*[]){"B", "TRY AGAIN", "A", "SAVE", NULL}, 1, s, 1);
		} else {
			y = draw_wrapped(s, HINT_get("chord.intro"), y, COLOR_WHITE, 2);
			y += SCALE1(4);
			y = draw_wrapped(s, HINT_get("chord.warning"), y, COLOR_GRAY, 2);
			if (chord_note[0]) {
				y += SCALE1(4);
				y = draw_wrapped(s, chord_note, y, COLOR_WHITE, 2);
			}
			GFX_blitButtonGroup((char*[]){"MENU", "CANCEL", NULL}, 1, s, 1);
		}
		break;
	}
	case SCREEN_PAK_INSTALL: {
		bool nds_base = pak_kind == NS_PAK_NDS && !pak_status.base_installed;
		draw_title(s, nds_base ? "Install NDS.pak?" : PAK_UI[pak_kind].install_title);
		int y = SCALE1(PADDING + 44);
		char line[128];
		if (nds_base) {
			y = draw_line(s, "Netplay installs NDS.pak (DraStic 2.5.2.0)", y, COLOR_WHITE, true);
			y = draw_line(s, "with DS local wireless for every platform", y, COLOR_WHITE, true);
			y = draw_line(s, "on your device. Continue?", y, COLOR_WHITE, true);
		} else if (pak_kind == NS_PAK_NDS) {
			/* An overlay: the user's DraStic and its settings stay. */
			y = draw_line(s, PAK_UI[pak_kind].needs, y, COLOR_WHITE, true);
			y = draw_line(s, "Your DraStic settings stay as they are, and", y, COLOR_WHITE, true);
			y = draw_line(s, "the files it replaces are kept for later.", y, COLOR_WHITE, true);
			y = draw_line(s, "Continue?", y, COLOR_WHITE, true);
		} else {
			y = draw_line(s, PAK_UI[pak_kind].needs, y, COLOR_WHITE, true);
			if (pak_status.installed || pak_status.present) {
				snprintf(line, sizeof(line), "Your current %s.pak, saves and savestates", PAK_UI[pak_kind].pak);
				y = draw_line(s, line, y, COLOR_WHITE, true);
				y = draw_line(s, "will be backed up.", y, COLOR_WHITE, true);
			}
			snprintf(line, sizeof(line), "%s will be installed for every platform", PAK_UI[pak_kind].pak);
			y = draw_line(s, line, y, COLOR_WHITE, true);
			y = draw_line(s, "on your device. Continue?", y, COLOR_WHITE, true);
		}
		y += SCALE1(5);
		draw_choice(s, y, "Continue", "Cancel", prompt_sel);
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
		draw_choice(s, y, "Reinstall", "Ignore this build", prompt_sel);
		GFX_blitButtonGroup((char*[]){"B", "IGNORE", "A", "SELECT", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_REMOVE_CONFIRM: {
		char line[128];
		snprintf(line, sizeof(line), "Delete %s.pak?", PAK_UI[pak_kind].pak);
		draw_title(s, line);
		int y = SCALE1(PADDING + 44);
		y = draw_wrapped(s, HINT_getf("remove.device.%s", PAK_UI[pak_kind].pak), y, COLOR_WHITE, 2);
		y += SCALE1(5);
		draw_choice(s, y, "Delete", "Cancel", prompt_sel);
		GFX_blitButtonGroup((char*[]){"B", "CANCEL", "A", "SELECT", NULL}, 1, s, 1);
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
	case SCREEN_VARNISH_CONFIRM: {
		bool update = varnish_state == NS_VARNISH_OUTDATED;
		draw_title(s, update ? "Update Varnish?" : "Install Varnish?");
		int y = SCALE1(PADDING + 44);
		if (update) {
			y = draw_line(s, "The status icon and quick menu need a newer", y, COLOR_WHITE, true);
			y = draw_line(s, "Varnish.pak. Update it from Netplay now?", y, COLOR_WHITE, true);
		} else {
			y = draw_line(s, "The status icon and quick menu are drawn by", y, COLOR_WHITE, true);
			y = draw_line(s, "Varnish.pak. Install it to Tools now?", y, COLOR_WHITE, true);
		}
		y += SCALE1(5);
		draw_line(s, "It starts drawing after the next reboot.", y, COLOR_GRAY, true);
		GFX_blitButtonGroup((char*[]){"B", "CANCEL", "A", update ? "UPDATE" : "INSTALL", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_VARNISH_REBOOT: {
		draw_title(s, "Varnish installed");
		int y = SCALE1(PADDING + 44);
		y = draw_line(s, "NextUI loads it when it starts, so it", y, COLOR_WHITE, true);
		y = draw_line(s, "begins drawing after a reboot. Reboot now?", y, COLOR_WHITE, true);
		GFX_blitButtonGroup((char*[]){"B", "LATER", "A", "REBOOT", NULL}, 1, s, 1);
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
			draw_choice(s, y, "Remove from NDS.pak", "Keep it", restore_sel);
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
		snprintf(pak_q, sizeof(pak_q), "Restore %s.pak", PAK_UI[pak_kind].pak);
		const char* names[3] = { pak_q, "Restore saves", "Restore save states" };
		for (int i = 0; i < 3; i++) {
			Row r = { .kind = ROW_VALUE };
			snprintf(r.label, sizeof(r.label), "%s", names[i]);
			snprintf(r.value, sizeof(r.value), "%s", restore_choice[i] ? "Yes" : "No");
			y = draw_two_col(s, &r, y, restore_sel == i);
		}
		Row apply = { .kind = ROW_ACTION };
		snprintf(apply.label, sizeof(apply.label), "Apply");
		y = draw_two_col(s, &apply, y, restore_sel == 3);
		snprintf(apply.label, sizeof(apply.label), "Keep installed %s", PAK_UI[pak_kind].pak);
		y = draw_two_col(s, &apply, y, restore_sel == 4);
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
				Row r = { .kind = ROW_ACTION };
				if (pr->scanned) {
					/* The SSID carries the host's code, which is what the user
					 * sees on the hosting device - that is the thing to match. */
					snprintf(r.label, sizeof(r.label), "%s", pr->ssid);
					snprintf(r.value, sizeof(r.value), "ad hoc");
				} else {
					int same = !strcmp(pr->platform, NS_platform());
					snprintf(r.label, sizeof(r.label), "%s", pr->ip);
					snprintf(r.value, sizeof(r.value), "%s%s", pr->hotspot ? "ad hoc" : "WiFi",
					         same ? "" : ", other device");
				}
				y = draw_two_col(s, &r, y, i == peer_sel);
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

/* LEFT/RIGHT step through a value's options; A steps forward too, except
 * where A means something else (the quick menu's buttons). */
static int lr_step(void) {
	if (PAD_justPressed(BTN_RIGHT)) return 1;
	if (PAD_justPressed(BTN_LEFT)) return -1;
	return 0;
}

static void change_setting(int id, int dir) {
	NS_Settings* c = NS_settings();
	status[0] = '\0';
	switch (id) {
	case SET_QUICK_MENU:
		if (!c->quick_menu) {
			if (needs_varnish(SET_QUICK_MENU)) return;
			c->quick_menu = true;
			/* On means START; other buttons are chosen afresh each time. */
			c->quick_keys[0] = c->quick_label[0] = '\0';
		} else c->quick_menu = false;
		break;
	case SET_STATUS_ICON: {
		/* Disabled < Active Session < Always, stepping either way. */
		int v = ((int)c->status_icon + dir + 3) % 3;
		if (c->status_icon == NS_ICON_OFF && needs_varnish(SET_STATUS_ICON)) return;
		c->status_icon = (NS_IconMode)v;
		break;
	}
	case SET_COMPAT:
		c->compatibility_cores = !c->compatibility_cores;
		if (!c->compatibility_cores) c->force_compatibility = false;
		break;
	case SET_GAMESWITCHER:
		c->add_gameswitcher = !c->add_gameswitcher;
		break;
	case SET_POWERSAVE:
		/* Applies to an armed session now, not only from the next game
		 * launch: Restore is a no-op when nothing was recorded. */
		if (!c->disable_powersave && NS_wifiPowerSaveNeedsReload()) {
			screen = SCREEN_POWERSAVE_CONFIRM;   /* H700: slow, so ask */
			return;
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
	}
	NS_settingsSave();
}

static void change_option(int id) {
	status[0] = '\0';
	if (id == OPT_DUAL) {
		if (!NS_gambatteDualAvailable()) {
			snprintf(status, sizeof(status), "Dual instance is not built for this device.");
			return;
		}
		set_instanced(0, !gambatte_dual());
		cores_fresh = false;
	} else if (id == OPT_INSTANCED) {
		bool ready = pak_status.managed && pak_status.installed && !pak_status.missing;
		if (mgba_instanced() && ready) {
			set_instanced(1, false);
		} else if (ready && !pak_status.mismatch) {
			set_instanced(1, true);
		} else {
			pak_kind = NS_PAK_MGBA;
			pak_return = SCREEN_CORE_OPTIONS;
			prompt_sel = 0;
			screen = SCREEN_PAK_INSTALL;
			return;
		}
		cores_fresh = false;
	}
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
	{
		char hints_path[512];
		snprintf(hints_path, sizeof(hints_path), "%s/pak_hints.json", NS_pakPath());
		HINT_load(hints_path, NS_platform());
	}
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
		NS_statusIconSync();   /* restart the icon if its watcher died */
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
	if (mgba_instanced() && NS_pakStatus(NS_PAK_MGBA, &pak_status) &&
	    pak_status.mismatch && !pak_status.mismatch_ignored) {
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
		 * wanted and it used to be buried in the list. Not while X might be
		 * one of the quick menu's buttons being held. */
		if (PAD_justPressed(BTN_X) && NS_isArmed() && screen != SCREEN_CHORD) {
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

		build_rows();

		switch (screen) {
		case SCREEN_MENU:
			if (NS_isArmed()) {
				/* Session status replaces Host/Join while armed. Configuration
				 * is the only selectable row; X remains the explicit lifecycle
				 * action. */
				if (PAD_justPressed(BTN_B)) quit = 1;
				if (PAD_justPressed(BTN_A)) {
					status[0] = '\0';
					config_sel = 0;
					screen = SCREEN_CONFIG;
					dirty = 1;
				}
				break;
			}
			if (step_sel(&menu_sel, MENU_COUNT)) dirty = 1;
			if (PAD_justPressed(BTN_B))    quit = 1;
			if (PAD_justPressed(BTN_A)) {
				status[0] = '\0';
				sub_sel = 0;
				switch (menu_sel) {
				case MENU_HOST:   screen = SCREEN_HOST_MENU; break;
				case MENU_CONFIG: config_sel = 0; screen = SCREEN_CONFIG; break;
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

		case SCREEN_CONFIG:
			if (step_sel(&config_sel, row_count)) dirty = 1;
			if (PAD_justPressed(BTN_B)) { status[0] = '\0'; screen = SCREEN_MENU; dirty = 1; }
			if (PAD_justPressed(BTN_A)) {
				status[0] = '\0';
				switch (rows[config_sel].id) {
				case CONF_CORES:    cores_sel = 0; screen = SCREEN_CORES; break;
				case CONF_SETTINGS: settings_sel = 0; screen = SCREEN_SETTINGS; break;
				case CONF_DEBUG:    debug_sel = 0; screen = SCREEN_DEBUG; break;
				case CONF_OFF:      turn_off_from(NS_PAK_MGBA); break;
				}
				dirty = 1;
			}
			break;

		case SCREEN_CORES:
			if (step_sel(&cores_sel, row_count)) dirty = 1;
			if (PAD_justPressed(BTN_B)) { status[0] = '\0'; screen = SCREEN_CONFIG; dirty = 1; }
			if (PAD_justPressed(BTN_A)) {
				open_core_list(rows[cores_sel].id == CORES_EXTRAS);
				dirty = 1;
			}
			break;

		case SCREEN_CORE_LIST: {
			if (step_sel(&list_sel, row_count)) dirty = 1;
			if (PAD_justPressed(BTN_B)) { status[0] = '\0'; screen = SCREEN_CORES; dirty = 1; }
			if (PAD_justPressed(BTN_A) && list_sel < row_count) {
				int n;
				const CoreDef* d = &list_defs(&n)[rows[list_sel].id];
				const CoreState* st = &core_state[rows[list_sel].id];
				status[0] = '\0';
				switch (d->kind) {
				case CK_SHIM:
					break;   /* status only */
				case CK_GAMBATTE:
					if (st->more) { opt_core = CK_GAMBATTE; opt_sel = 0; screen = SCREEN_CORE_OPTIONS; }
					break;
				case CK_MGBA:
					if (!strcmp(st->state, "unavailable")) break;
					if (!st->pak_ok) {
						snprintf(status, sizeof(status), "Could not check MGBA.pak - try again.");
						break;
					}
					pak_status = st->pak;
					opt_core = CK_MGBA;
					opt_sel = 0;
					screen = SCREEN_CORE_OPTIONS;
					break;
				case CK_DSP:
				case CK_NDS:
					if (!strcmp(st->state, "unavailable")) break;
					ds_row_action(d->kind == CK_DSP ? NS_PAK_DSP : NS_PAK_NDS, st);
					break;
				}
				dirty = 1;
			}
			break;
		}

		case SCREEN_CORE_OPTIONS: {
			if (step_sel(&opt_sel, row_count)) dirty = 1;
			if (PAD_justPressed(BTN_B)) { status[0] = '\0'; screen = SCREEN_CORE_LIST; dirty = 1; }
			if (opt_sel >= row_count) break;
			int id = rows[opt_sel].id;
			if (rows[opt_sel].kind == ROW_VALUE && (lr_step() || PAD_justPressed(BTN_A))) {
				change_option(id);
				dirty = 1;
			} else if (id == OPT_REMOVE && PAD_justPressed(BTN_A)) {
				pak_kind = NS_PAK_MGBA;
				pak_return = SCREEN_CORE_OPTIONS;
				restore_choice[0] = restore_choice[1] = restore_choice[2] = true;
				restore_sel = 0; restore_then_off = false;
				screen = SCREEN_PAK_RESTORE;
				dirty = 1;
			}
			break;
		}

		case SCREEN_SETTINGS: {
			if (step_sel(&settings_sel, row_count)) dirty = 1;
			if (PAD_justPressed(BTN_B)) { status[0] = '\0'; screen = SCREEN_CONFIG; dirty = 1; }
			int id = rows[settings_sel].id;
			int dir = lr_step();
			if (dir) {
				change_setting(id, dir);
				dirty = 1;
			} else if (PAD_justPressed(BTN_A)) {
				if (id == SET_QUICK_MENU && NS_settings()->quick_menu) open_chord();
				else change_setting(id, 1);
				dirty = 1;
			}
			break;
		}

		case SCREEN_CHORD:
			chord_tick();
			dirty = 1;
			break;

		case SCREEN_DEBUG: {
			NS_Settings* c = NS_settings();
			if (step_sel(&debug_sel, row_count)) dirty = 1;
			if (PAD_justPressed(BTN_B)) { status[0] = '\0'; screen = SCREEN_CONFIG; dirty = 1; }
			int id = rows[debug_sel].id;
			bool change = rows[debug_sel].kind == ROW_VALUE ? (lr_step() || PAD_justPressed(BTN_A))
			                                               : PAD_justPressed(BTN_A);
			if (!change) break;
			status[0] = '\0';
			switch (id) {
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
			case DEBUG_RESTORE_WIFI:
				snprintf(status, sizeof(status), "Restoring WiFi...");
				render(s);
				set_progress("Restoring WiFi", "Reconnecting to your network.");
				NS_wifiRestore();
				set_progress("Joining", "This can take up to a minute.");
				wifi_poll(true);
				snprintf(status, sizeof(status), wifi_up
				         ? "Reconnected to %s." : "Could not reconnect - use system WiFi settings.",
				         wifi_ssid);
				break;
			}
			dirty = 1;
			break;
		}

		case SCREEN_PAK_INSTALL:
			if (PAD_justPressed(BTN_UP) || PAD_justPressed(BTN_DOWN)) { prompt_sel = !prompt_sel; dirty = 1; }
			if (PAD_justPressed(BTN_B)) { pak_done(); dirty = 1; }
			if (PAD_justPressed(BTN_A)) {
				if (prompt_sel == 0) run_pak_install();
				else pak_done();
				dirty = 1;
			}
			break;

		case SCREEN_PAK_REMOVE: {
			if (step_sel(&remove_sel, row_count)) dirty = 1;
			if (PAD_justPressed(BTN_B)) { pak_done(); dirty = 1; }
			if (!PAD_justPressed(BTN_A)) break;
			status[0] = '\0';
			switch (rows[remove_sel].id) {
			case REM_NETPLAY:
				if (pak_kind == NS_PAK_DSP) {
					NS_settings()->dsperate = false;
					NS_settingsSave();
					snprintf(status, sizeof(status), "DSP.pak kept; its games play offline now.");
				} else {
					set_progress("Removing DS local wireless", "Putting NDS.pak's own files back.");
					draw_progress("Restoring", 1, 1);
					if (NS_pakRestore(pak_kind, true, false, false, status, sizeof(status))) {
						NS_settings()->drastic = false;
						NS_settingsSave();
						snprintf(status, sizeof(status), "NDS.pak has its own launcher back.");
					}
					set_progress("Joining", "This can take up to a minute.");
				}
				pak_done();
				break;
			case REM_DEVICE:
				prompt_sel = 1;   /* deleting is never the default */
				screen = SCREEN_REMOVE_CONFIRM;
				break;
			case REM_BACKUP:
				restore_choice[0] = restore_choice[1] = restore_choice[2] = true;
				restore_sel = 0; restore_then_off = false;
				screen = SCREEN_PAK_RESTORE;
				break;
			default:
				pak_done();
				break;
			}
			dirty = 1;
			break;
		}

		case SCREEN_REMOVE_CONFIRM:
			if (PAD_justPressed(BTN_UP) || PAD_justPressed(BTN_DOWN)) { prompt_sel = !prompt_sel; dirty = 1; }
			if (PAD_justPressed(BTN_B)) { screen = SCREEN_PAK_REMOVE; dirty = 1; }
			if (PAD_justPressed(BTN_A)) {
				if (prompt_sel == 0) {
					char title[48];
					snprintf(title, sizeof(title), "Removing %s", PAK_UI[pak_kind].pak);
					set_progress(title, "Deleting the pak from the card.");
					draw_progress("Deleting", 1, 1);
					if (NS_pakRemove(pak_kind, status, sizeof(status))) {
						if (pak_kind == NS_PAK_DSP) NS_settings()->dsperate = false;
						else NS_settings()->drastic = false;
						NS_settingsSave();
						snprintf(status, sizeof(status), "%s.pak removed.", PAK_UI[pak_kind].pak);
					}
					set_progress("Joining", "This can take up to a minute.");
					pak_done();
				} else screen = SCREEN_PAK_REMOVE;
				dirty = 1;
			}
			break;

		case SCREEN_VARNISH_CONFIRM:
			if (PAD_justPressed(BTN_B)) { screen = SCREEN_SETTINGS; dirty = 1; }
			if (PAD_justPressed(BTN_A)) {
				set_progress(varnish_state == NS_VARNISH_OUTDATED ? "Updating Varnish" : "Installing Varnish",
				             "Copying Varnish.pak to Tools.");
				draw_progress("Installing", 1, 1);
				status[0] = '\0';
				screen = SCREEN_SETTINGS;
				if (NS_varnishInstall(status, sizeof(status))) {
					NS_Settings* c = NS_settings();
					if (varnish_for == SET_STATUS_ICON) c->status_icon = NS_ICON_PERSISTENT;
					else { c->quick_menu = true; c->quick_keys[0] = c->quick_label[0] = '\0'; }
					NS_settingsSave();
					screen = SCREEN_VARNISH_REBOOT;
				}
				dirty = 1;
			}
			break;

		case SCREEN_VARNISH_REBOOT:
			if (PAD_justPressed(BTN_B)) {
				snprintf(status, sizeof(status), "Varnish starts after the next reboot.");
				screen = SCREEN_SETTINGS;
				dirty = 1;
			}
			if (PAD_justPressed(BTN_A)) {
				/* NextUI's launcher reboots when a pak exits leaving this. */
				FILE* f = fopen("/tmp/reboot", "w");
				if (f) { fclose(f); sync(); quit = 1; }
				else {
					snprintf(status, sizeof(status), "Could not reboot - restart the device.");
					screen = SCREEN_SETTINGS;
				}
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
				NS_pakIgnoreMismatch(pak_kind, status, sizeof(status)); pak_done(); dirty = 1;
			}
			if (PAD_justPressed(BTN_A)) {
				if (prompt_sel == 0) {
					char title[48];
					snprintf(title, sizeof(title), "Reinstalling %s", PAK_UI[pak_kind].name);
					set_progress(title, "Preserving your original backup.");
					if (NS_pakInstall(pak_kind, status, sizeof(status)))
						snprintf(status, sizeof(status), "%s reinstalled.", PAK_UI[pak_kind].name);
				} else NS_pakIgnoreMismatch(pak_kind, status, sizeof(status));
				pak_done();
				dirty = 1;
			}
			break;

		case SCREEN_PAK_RESTORE:
			if (pak_kind == NS_PAK_NDS) {
				if (PAD_justPressed(BTN_UP) || PAD_justPressed(BTN_DOWN)) { restore_sel = !restore_sel; dirty = 1; }
				if (PAD_justPressed(BTN_B)) { cancel_restore(); dirty = 1; }
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
			if (PAD_justPressed(BTN_B)) { cancel_restore(); dirty = 1; }
			if (restore_sel < 3 && (lr_step() || PAD_justPressed(BTN_A))) {
				restore_choice[restore_sel] = !restore_choice[restore_sel];
				dirty = 1;
			} else if (PAD_justPressed(BTN_A)) {
				if (restore_sel == 3) {
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
