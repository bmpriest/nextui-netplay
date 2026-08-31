#include "drastic_wifi_hook.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_register_offset(void)
{
	assert(dwifi_register_offset(UINT32_C(0x04800000)) == 0);
	assert(dwifi_register_offset(UINT32_C(0x04808184)) == UINT32_C(0x0184));
	assert(dwifi_register_offset(UINT32_C(0x04805ffe)) == UINT32_C(0x1ffe));
}

static void test_brick_dispatcher_wifi_gate(void)
{
	assert(!dwifi_brick_dispatcher_is_wifi(UINT32_C(0x4000)));
	assert(!dwifi_brick_dispatcher_is_wifi(UINT32_C(0x7ffe)));
	assert(dwifi_brick_dispatcher_is_wifi(UINT32_C(0x8000)));
	assert(dwifi_brick_dispatcher_is_wifi(UINT32_C(0xc000)));
	assert(dwifi_brick_packet_store_address(UINT16_C(0x0000)) ==
		UINT32_C(0x00804000));
	assert(dwifi_brick_packet_store_address(UINT16_C(0x0bfc)) ==
		UINT32_C(0x00804bfc));
	assert(dwifi_brick_packet_store_address(UINT16_C(0x3ffe)) ==
		UINT32_C(0x00805ffe));
}

static void test_absolute_jump(void)
{
	uint32_t words[2] = {0};
	dwifi_make_absolute_jump(words, UINT32_C(0x12345678));
	assert(words[0] == DWIFI_ARM_JUMP_WORD);
	assert(words[1] == UINT32_C(0x12345678));
}

static void test_aarch64_jump(void)
{
	uint32_t words[4] = {0};
	dwifi_make_aarch64_jump(words,
		(uintptr_t)UINT64_C(0x1122334455667788));
	assert(words[0] == DWIFI_A64_LDR_X16_LITERAL);
	assert(words[1] == DWIFI_A64_BR_X16);
	assert(words[2] == UINT32_C(0x55667788));
	assert(words[3] == UINT32_C(0x11223344));
}

static void test_prologue_validation(void)
{
	const uint32_t expected[2] = {
		UINT32_C(0xe6ff2071), UINT32_C(0xe1a03000)
	};
	uint32_t actual[2] = {
		UINT32_C(0xe6ff2071), UINT32_C(0xe1a03000)
	};
	assert(dwifi_prologue_matches(actual, expected));
	actual[1] ^= 1;
	assert(!dwifi_prologue_matches(actual, expected));
	assert(!dwifi_prologue_matches(NULL, expected));
}

static void test_drastic_command_gate(void)
{
	static const char direct[] = "/mnt/SDCARD/NDS.pak/drastic/drastic\0game.nds\0";
	static const char loader[] = "./lib/ld-linux-armhf.so.3\0./drastic\0game.nds\0";
	static const char packaged[] = "./lib/ld-linux-armhf.so.3\0--library-path\0./lib\0./drastic\0game.nds\0";
	static const char helper[] = "/bin/sh\0-c\0sync\0";
	static const char misleading[] = "/bin/sh\0./drastic\0";
	assert(dwifi_is_drastic_command(direct, sizeof(direct)));
	assert(dwifi_is_drastic_command(loader, sizeof(loader)));
	assert(dwifi_is_drastic_command(packaged, sizeof(packaged)));
	assert(!dwifi_is_drastic_command(helper, sizeof(helper)));
	assert(!dwifi_is_drastic_command(misleading, sizeof(misleading)));
	assert(!dwifi_is_drastic_command("drastic", 7));
}

int main(void)
{
	test_register_offset();
	test_brick_dispatcher_wifi_gate();
	test_absolute_jump();
	test_aarch64_jump();
	test_prologue_validation();
	test_drastic_command_gate();
	puts("drastic-wifi hook tests passed");
	return 0;
}
