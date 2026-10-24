/*
 * pak_hints.json reader. A small recursive-descent parser that flattens the
 * file's nested objects into "a.b.c" keys; only string values are kept, and
 * numbers, booleans, nulls and arrays are skipped. The file is a few KB, read
 * once when the app starts.
 */

#include "hints.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HINTS 256
#define MAX_KEY   96

static struct {
	char  key[MAX_KEY];
	char* text;
} hints[MAX_HINTS];
static int  hint_count;
static char hint_platform[32];

typedef struct {
	const char* p;
	const char* end;
	bool        bad;
} Parser;

static void skip_ws(Parser* ps) {
	while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
		ps->p++;
}

static void put_utf8(char* out, int* n, int cap, unsigned cp) {
	char b[4];
	int len;
	if (cp < 0x80)       { b[0] = cp; len = 1; }
	else if (cp < 0x800) { b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3F); len = 2; }
	else { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3F); b[2] = 0x80 | (cp & 0x3F); len = 3; }
	for (int i = 0; i < len && *n < cap - 1; i++) out[(*n)++] = b[i];
}

/* A string at ps->p (on its opening quote) into out, decoded. */
static bool parse_string(Parser* ps, char* out, int cap) {
	int n = 0;
	if (ps->p >= ps->end || *ps->p != '"') return false;
	ps->p++;
	while (ps->p < ps->end && *ps->p != '"') {
		char c = *ps->p++;
		if (c == '\\' && ps->p < ps->end) {
			char e = *ps->p++;
			switch (e) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'u': {
				unsigned cp = 0;
				for (int i = 0; i < 4 && ps->p < ps->end; i++) {
					char h = *ps->p++;
					cp = cp * 16 + (h >= '0' && h <= '9' ? h - '0'
					              : h >= 'a' && h <= 'f' ? h - 'a' + 10
					              : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
				}
				put_utf8(out, &n, cap, cp);
				continue;
			}
			default: c = e; break;   /* \" \\ \/ */
			}
		}
		if (n < cap - 1) out[n++] = c;
	}
	if (ps->p >= ps->end) return false;
	ps->p++;
	out[n] = '\0';
	return true;
}

static void parse_value(Parser* ps, const char* key);

static void parse_object(Parser* ps, const char* prefix) {
	ps->p++;   /* { */
	for (;;) {
		skip_ws(ps);
		if (ps->p < ps->end && *ps->p == '}') { ps->p++; return; }
		char name[MAX_KEY], key[MAX_KEY];
		if (!parse_string(ps, name, sizeof(name))) { ps->bad = true; return; }
		snprintf(key, sizeof(key), "%s%s%s", prefix, prefix[0] ? "." : "", name);
		skip_ws(ps);
		if (ps->p >= ps->end || *ps->p != ':') { ps->bad = true; return; }
		ps->p++;
		skip_ws(ps);
		parse_value(ps, key);
		if (ps->bad) return;
		skip_ws(ps);
		if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
		if (ps->p < ps->end && *ps->p == '}') { ps->p++; return; }
		ps->bad = true;
		return;
	}
}

static void parse_value(Parser* ps, const char* key) {
	if (ps->p >= ps->end) { ps->bad = true; return; }
	if (*ps->p == '{') { parse_object(ps, key); return; }
	if (*ps->p == '"') {
		char text[512];
		if (!parse_string(ps, text, sizeof(text))) { ps->bad = true; return; }
		if (hint_count < MAX_HINTS) {
			snprintf(hints[hint_count].key, MAX_KEY, "%s", key);
			hints[hint_count].text = strdup(text);
			hint_count++;
		}
		return;
	}
	if (*ps->p == '[') {   /* not used for hints; skip it whole */
		int depth = 0;
		bool in_str = false;
		for (; ps->p < ps->end; ps->p++) {
			char c = *ps->p;
			if (in_str) { if (c == '\\') ps->p++; else if (c == '"') in_str = false; continue; }
			if (c == '"') in_str = true;
			else if (c == '[') depth++;
			else if (c == ']' && --depth == 0) { ps->p++; return; }
		}
		ps->bad = true;
		return;
	}
	/* number, true, false, null */
	while (ps->p < ps->end && *ps->p != ',' && *ps->p != '}' && *ps->p != ']') ps->p++;
}

static void clear(void) {
	for (int i = 0; i < hint_count; i++) free(hints[i].text);
	hint_count = 0;
}

bool HINT_load(const char* path, const char* platform) {
	clear();
	snprintf(hint_platform, sizeof(hint_platform), "%s", platform ? platform : "");
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || size > 256 * 1024) { fclose(f); return false; }
	char* buf = malloc(size);
	if (!buf || fread(buf, 1, size, f) != (size_t)size) { free(buf); fclose(f); return false; }
	fclose(f);

	Parser ps = { buf, buf + size, false };
	skip_ws(&ps);
	if (ps.p < ps.end && *ps.p == '{') parse_object(&ps, "");
	else ps.bad = true;
	free(buf);
	if (ps.bad) { clear(); return false; }
	return true;
}

static const char* lookup(const char* key) {
	for (int i = 0; i < hint_count; i++)
		if (!strcmp(hints[i].key, key)) return hints[i].text;
	return NULL;
}

const char* HINT_get(const char* key) {
	if (hint_platform[0]) {
		char k[MAX_KEY + 40];
		snprintf(k, sizeof(k), "%s.%s", key, hint_platform);
		const char* t = lookup(k);
		if (t) return t;
	}
	const char* t = lookup(key);
	return t ? t : "";
}

const char* HINT_getf(const char* fmt, ...) {
	char key[MAX_KEY];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(key, sizeof(key), fmt, ap);
	va_end(ap);
	return HINT_get(key);
}
