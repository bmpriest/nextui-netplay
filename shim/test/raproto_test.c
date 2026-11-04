/*
 * raproto's pure helpers against values taken from captures of two spruce
 * devices running RetroArch 1.22.2 (testing/results/retroarch-spruce-20261102,
 * decoded with radecode.py):
 *
 *   header words  52414e50 00040008 00000001 00000007 00000005 00000455
 *   LOAD_SAVESTATE  uncompressed 5616 bytes: "NETPLAY\1", "MEM " 0x15c4
 *                   (fceumm's 5572-byte state), ..., "END " 0
 *
 * With an argument, also unwraps a raw LOAD_SAVESTATE container dumped from a
 * capture (inflated) and checks it holds the expected core length.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../raproto.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } \
                           else { printf("ok   " __VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char** argv) {
	CHECK(RaProto_implMagic("1.22.2", 7) == 0x455, "implementation magic for 1.22.2 is 0x455 (got %#x)",
	      RaProto_implMagic("1.22.2", 7));
	if (sizeof(long) == 8 && sizeof(size_t) == 8)
		CHECK(RaProto_platformMagic() == 0x40008, "platform magic on LP64 is 0x40008 (got %#x)",
		      RaProto_platformMagic());
	CHECK(RaProto_deviceWords(1) == 1 && RaProto_deviceWords(5) == 3 && RaProto_deviceWords(3) == 5 &&
	      RaProto_deviceWords(0x101) == 1, "INPUT words per device type");

	unsigned char core[5572];
	for (size_t i = 0; i < sizeof(core); i++) core[i] = (unsigned char)(i * 31 + 7);
	size_t len = 0;
	unsigned char* c = RaProto_buildContainer(core, sizeof(core), &len);
	CHECK(c && len == 5616, "container for 5572 bytes is 5616, as spruce sends (got %zu)", len);
	CHECK(c && !memcmp(c, "NETPLAY\1MEM \xc4\x15\0\0", 16), "container header and MEM block");
	CHECK(c && !memcmp(c + len - 8, "END \0\0\0\0", 8), "container ends with END");
	const void* inner = NULL;
	size_t inner_len = 0;
	CHECK(c && RaProto_unwrapContainer(c, len, &inner, &inner_len) && inner_len == sizeof(core) &&
	      !memcmp(inner, core, sizeof(core)), "unwrap returns the core state");
	CHECK(RaProto_unwrapContainer(core, sizeof(core), &inner, &inner_len) && inner == (void*)core &&
	      inner_len == sizeof(core), "a bare state (protocol 5/6) passes through");
	unsigned char bad[24] = "NETPLAY\1ACHV\x08\0\0\0";
	CHECK(!RaProto_unwrapContainer(bad, sizeof(bad), &inner, &inner_len), "a container without MEM is refused");
	free(c);

	if (argc > 2) {
		FILE* f = fopen(argv[1], "rb");
		static unsigned char buf[1 << 20];
		size_t n = f ? fread(buf, 1, sizeof(buf), f) : 0;
		if (f) fclose(f);
		size_t want = (size_t)strtoul(argv[2], NULL, 10);
		CHECK(n && RaProto_unwrapContainer(buf, n, &inner, &inner_len) && inner_len == want,
		      "captured spruce container unwraps to %zu core bytes (got %zu)", want, inner_len);
		size_t rebuilt_len = 0;
		unsigned char* rebuilt = inner_len ? RaProto_buildContainer(inner, inner_len, &rebuilt_len) : NULL;
		CHECK(rebuilt && rebuilt_len == n && !memcmp(rebuilt, buf, n),
		      "our container is byte-identical to spruce's for the same core state");
		free(rebuilt);
	}
	printf(failures ? "RESULT: %d failure(s)\n" : "RESULT: ok\n", failures);
	return failures != 0;
}
