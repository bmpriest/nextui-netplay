#!/bin/sh
# pak_hints.json carries every hint the setup app asks for, in every state, and
# the app's own reader (app/hints.c) parses it. Keys the app builds from
# variables are listed here by hand from main.c's tables; keep them in step.
set -eu
cd "$(dirname "$0")/.."

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/dump.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "hints.h"
int main(int argc, char** argv) {
	if (!HINT_load(argv[1], argc > 2 ? argv[2] : "")) { puts("LOAD FAILED"); return 1; }
	char key[160];
	int missing = 0;
	while (fgets(key, sizeof(key), stdin)) {
		key[strcspn(key, "\n")] = '\0';
		if (!key[0]) continue;
		if (!HINT_get(key)[0]) { printf("missing %s\n", key); missing++; }
	}
	return missing != 0;
}
EOF
gcc -std=gnu99 -Wall -Werror -Iapp "$TMP/dump.c" app/hints.c -o "$TMP/dump"

{
	# Literal keys, straight from the source.
	grep -o 'HINT_get("[^"]*")' app/main.c | sed 's/HINT_get("\(.*\)")/\1/'
	# cores.<tag>.<state>, per kind (main.c's refresh_core).
	for t in FC GBA MD PS SFC 32X FBN FDS GG SEGACD SG1000 SMS SUPA; do
		for st in ready present absent; do echo "cores.$t.$st"; done
	done
	for st in ready dual present absent; do echo "cores.GB.$st"; done
	for st in ready off absent unavailable; do echo "cores.DSP.$st"; done
	for st in instanced installed absent unavailable; do echo "cores.MGBA.$st"; done
	for st in ready present unsupported absent unavailable; do echo "cores.NDS.$st"; done
	for st in off on custom needs_varnish unavailable; do echo "settings.quick_menu.$st"; done
	for st in always session disabled needs_varnish unavailable; do echo "settings.icon.$st"; done
	for k in settings.compat settings.gameswitcher settings.powersave debug.force_compat debug.verbose_logs; do
		echo "$k.on"; echo "$k.off"
	done
	for t in DSP NDS; do echo "remove.netplay.$t"; echo "remove.device.$t"; done
	echo remove.backup.DSP
} > "$TMP/keys"

echo "== pak_hints.json"
"$TMP/dump" pak_hints.json < "$TMP/keys"
echo "  ok   every hint the app asks for is there ($(sort -u "$TMP/keys" | wc -l) keys)"

# A platform sibling wins on its platform only.
printf 'cores.SFC.ready\n' | "$TMP/dump" pak_hints.json my282
cat > "$TMP/plat.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "hints.h"
int main(int argc, char** argv) {
	HINT_load(argv[1], "my282");
	char a30[256];   /* a reload frees the strings */
	snprintf(a30, sizeof(a30), "%s", HINT_get("cores.SFC.ready"));
	HINT_load(argv[1], "tg5040");
	const char* brick = HINT_get("cores.SFC.ready");
	return !(strstr(a30, "Supafaust") && !strstr(brick, "Supafaust"));
}
EOF
gcc -std=gnu99 -Wall -Werror -Iapp "$TMP/plat.c" app/hints.c -o "$TMP/plat"
"$TMP/plat" pak_hints.json
echo "  ok   a .<platform> sibling replaces the hint on that platform only"

printf '{"a": {"b": "x\\"y \\u00e9"}, "n": 3, "l": [1, {"c": "d"}]}' > "$TMP/t.json"
printf 'a.b\n' | "$TMP/dump" "$TMP/t.json"
! printf 'l.c\n' | "$TMP/dump" "$TMP/t.json" >/dev/null
printf '{"a": ' > "$TMP/bad.json"
! "$TMP/dump" "$TMP/bad.json" </dev/null >/dev/null
echo "  ok   escapes decode, numbers and arrays are skipped, a truncated file is refused"
