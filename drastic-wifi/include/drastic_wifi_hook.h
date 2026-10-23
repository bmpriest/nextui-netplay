#ifndef DRASTIC_WIFI_HOOK_H
#define DRASTIC_WIFI_HOOK_H

#include <stddef.h>
#include <stdint.h>

#define DWIFI_A30_LOAD16_ADDRESS  UINT32_C(0x0800b10c)
#define DWIFI_A30_STORE16_ADDRESS UINT32_C(0x0800b4e0)
#define DWIFI_A30_EXECUTE_EVENTS_ADDRESS UINT32_C(0x08018c74)
#define DWIFI_ARM_JUMP_WORD       UINT32_C(0xe51ff004)

#define DWIFI_BRICK_LOAD16_OFFSET  UINT32_C(0x00013660)
#define DWIFI_BRICK_STORE16_OFFSET UINT32_C(0x000128d0)
#define DWIFI_BRICK_EXECUTE_EVENTS_OFFSET UINT32_C(0x000216e0)
#define DWIFI_A64_LDR_X16_LITERAL  UINT32_C(0x58000050)
#define DWIFI_A64_BR_X16           UINT32_C(0xd61f0200)

enum dwifi_operation {
	DWIFI_OP_LOAD16 = 1,
	DWIFI_OP_STORE16 = 2,
};

struct dwifi_event {
	uint64_t sequence;
	uint64_t host_ns;
	uint32_t address;
	uint16_t value;
	uint8_t operation;
};

uint32_t dwifi_register_offset(uint32_t address);
void dwifi_make_absolute_jump(uint32_t words[2], uintptr_t destination);
void dwifi_make_aarch64_jump(uint32_t words[4], uintptr_t destination);
int dwifi_prologue_matches(const void *actual, const uint32_t expected[2]);
int dwifi_words_match(const void *actual, const uint32_t *expected,
	size_t word_count);
int dwifi_is_drastic_command(const char *cmdline, size_t length);
int dwifi_brick_dispatcher_is_wifi(uint32_t address);
uint32_t dwifi_brick_packet_store_address(uint16_t offset);
int dwifi_is_packet_tx_slot(uint32_t register_offset);

#endif
