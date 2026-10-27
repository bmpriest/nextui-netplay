/*
 * Hint text for the setup app's menus, from pak_hints.json at the pak's root,
 * so the wording can be changed on the card without a rebuild.
 *
 * The file is nested objects of strings. A key names the path through them:
 * "cores.GB.dual" is {"cores": {"GB": {"dual": "..."}}}. A sibling named
 * after the platform overrides a hint there: "ready.my282" next to "ready"
 * is what the A30 shows.
 */

#ifndef HINTS_H
#define HINTS_H

#include <stdbool.h>

/* Reads the file; safe to call again to re-read it. False if it is missing or
 * malformed, in which case every hint is empty. */
bool HINT_load(const char* path, const char* platform);

/* The hint for key, or "" when the file has none. */
const char* HINT_get(const char* key);

/* HINT_get with the key built printf-style. */
const char* HINT_getf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
