#include "onboarding.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Small bounded JSON reader. The document is editable on the SD card, so
 * reject malformed input instead of presenting a partly parsed wizard. */
typedef struct { char* p; char* end; bool bad; } Reader;
static OB_Page pages[OB_MAX_PAGES];
static int count;

static void ws(Reader* r) { while (r->p < r->end && isspace((unsigned char)*r->p)) r->p++; }
static bool take(Reader* r, char c) { ws(r); if (r->p < r->end && *r->p == c) { r->p++; return true; } r->bad = true; return false; }
static bool string(Reader* r, char* dst, size_t cap) {
	ws(r); if (r->p == r->end || *r->p++ != '"') { r->bad = true; return false; }
	size_t n = 0;
	while (r->p < r->end && *r->p != '"') {
		unsigned char c = *r->p++;
		if (c == '\\') {
			if (r->p == r->end) break;
			c = *r->p++;
			switch (c) { case 'n': c = '\n'; break; case 't': c = ' '; break;
			case 'r': c = ' '; break; case '"': case '\\': case '/': break;
			default: r->bad = true; return false; }
		} else if (c < 32) { r->bad = true; return false; }
		if (n + 1 >= cap) { r->bad = true; return false; }
		dst[n++] = c;
	}
	if (r->p == r->end) { r->bad = true; return false; }
	r->p++; dst[n] = 0; return true;
}
static bool number(Reader* r, int* value) {
	ws(r); char* end;
	long v = strtol(r->p, &end, 10);
	if (end == r->p || v < 0 || v > 100 || (end < r->end && *end != ',' && *end != '}' && !isspace((unsigned char)*end))) { r->bad = true; return false; }
	r->p = end; *value = (int)v; return true;
}
static bool options(Reader* r, OB_Page* page) {
	if (!take(r, '[')) return false;
	ws(r); if (r->p < r->end && *r->p == ']') { r->p++; return true; }
	for (;;) {
		if (page->option_count == OB_MAX_OPTIONS || !take(r, '{')) return false;
		OB_Option* opt = &page->options[page->option_count++];
		bool label = false, value = false;
		for (;;) {
			char key[32];
			if (!string(r, key, sizeof(key)) || !take(r, ':')) return false;
			if (!strcmp(key, "label")) label = string(r, opt->label, sizeof(opt->label));
			else if (!strcmp(key, "value")) value = number(r, &opt->value);
			else { r->bad = true; return false; }
			if (r->bad) return false;
			ws(r); if (r->p < r->end && *r->p == '}') { r->p++; break; }
			if (!take(r, ',')) return false;
		}
		if (!label || !value) { r->bad = true; return false; }
		ws(r); if (r->p < r->end && *r->p == ']') { r->p++; return true; }
		if (!take(r, ',')) return false;
	}
}
static bool page(Reader* r, OB_Page* p) {
	if (!take(r, '{')) return false;
	for (;;) {
		char key[32];
		if (!string(r, key, sizeof(key)) || !take(r, ':')) return false;
		if (!strcmp(key, "title")) string(r, p->title, sizeof(p->title));
		else if (!strcmp(key, "description")) string(r, p->description, sizeof(p->description));
		else if (!strcmp(key, "image")) string(r, p->image, sizeof(p->image));
		else if (!strcmp(key, "setting")) string(r, p->setting, sizeof(p->setting));
		else if (!strcmp(key, "options")) options(r, p);
		else { r->bad = true; return false; }
		if (r->bad) return false;
		ws(r); if (r->p < r->end && *r->p == '}') { r->p++; break; }
		if (!take(r, ',')) return false;
	}
	if (!p->title[0] || (p->setting[0] && p->option_count < 2) || (!p->setting[0] && p->option_count)) { r->bad = true; return false; }
	if (p->setting[0]) {
		if (OB_settingValue(p->setting, NS_settings()) < 0) { r->bad = true; return false; }
		for (int i = 0; i < p->option_count; i++) {
			NS_Settings copy = *NS_settings();
			if (!p->options[i].label[0] || !OB_apply(p->setting, p->options[i].value, &copy)) { r->bad = true; return false; }
		}
	}
	return true;
}
bool OB_load(const char* path) {
	count = 0;
	FILE* f = fopen(path, "rb"); if (!f) return false;
	if (fseek(f, 0, SEEK_END)) { fclose(f); return false; }
	long size = ftell(f);
	if (size <= 0 || size > 65536 || fseek(f, 0, SEEK_SET)) { fclose(f); return false; }
	char* buf = malloc(size + 1);
	if (!buf) { fclose(f); return false; }
	bool ok = fread(buf, 1, size, f) == (size_t)size;
	fclose(f); buf[size] = 0;
	Reader r = {buf, buf + size, !ok};
	/* Keep the root deliberately simple: {"pages":[...]}. */
	char key[16];
	if (ok) ok = take(&r, '{') && string(&r, key, sizeof(key)) &&
	             !strcmp(key, "pages") && take(&r, ':') && take(&r, '[');
	if (ok) {
		ws(&r);
		while (r.p < r.end && *r.p != ']' && !r.bad) {
			if (count == OB_MAX_PAGES || !page(&r, &pages[count])) { ok = false; break; }
			count++; ws(&r);
			if (r.p < r.end && *r.p == ']') break;
			if (!take(&r, ',')) { ok = false; break; }
		}
		ok = ok && take(&r, ']') && take(&r, '}'); ws(&r);
		ok = ok && !r.bad && r.p == r.end && count > 0;
	}
	free(buf);
	if (!ok) count = 0;
	return ok;
}
int OB_count(void) { return count; }
const OB_Page* OB_page(int index) { return index >= 0 && index < count ? &pages[index] : NULL; }

/* Explicit allowlist keeps the JSON from writing arbitrary settings. Settings
 * with installation or radio side effects belong to their normal UI flows. */
int OB_settingValue(const char* k, const NS_Settings* s) {
	if (!strcmp(k, "compatibility_cores")) return s->compatibility_cores;
	if (!strcmp(k, "add_gameswitcher")) return s->add_gameswitcher;
	if (!strcmp(k, "adhoc_switch")) return s->adhoc_switch;
	if (!strcmp(k, "verbose_logs")) return s->verbose_logs;
	return -1;
}
bool OB_apply(const char* k, int v, NS_Settings* s) {
	if (v > 1) return false;
	if (!strcmp(k, "compatibility_cores")) {
		s->compatibility_cores = v;
		if (!v) s->force_compatibility = false;
	}
	else if (!strcmp(k, "add_gameswitcher")) s->add_gameswitcher = v;
	else if (!strcmp(k, "adhoc_switch")) s->adhoc_switch = v;
	else if (!strcmp(k, "verbose_logs")) s->verbose_logs = v;
	else return false;
	return true;
}
