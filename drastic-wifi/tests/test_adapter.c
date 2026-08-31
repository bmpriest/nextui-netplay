#include "drastic_wifi_adapter.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CPU_ACTIVE UINT32_C(0x2100)
#define CPU_STATE  UINT32_C(0x2105)
#define IO_IME     UINT32_C(0x0208)
#define IO_IE      UINT32_C(0x0210)
#define IO_IF      UINT32_C(0x0214)
#define WIFI_IRQ   UINT32_C(0x01000000)

static uint32_t read32(const uint8_t *memory, uint32_t offset)
{
	uint32_t value;
	memcpy(&value, memory + offset, sizeof(value));
	return value;
}

static void write32(uint8_t *memory, uint32_t offset, uint32_t value)
{
	memcpy(memory + offset, &value, sizeof(value));
}

static void test_latch_and_active_interrupt(void)
{
	uint8_t cpu[0x2108] = {0};
	uint8_t io[0x218] = {0};
	write32(io, IO_IME, 1);
	write32(io, IO_IE, WIFI_IRQ);
	assert(dwifi_apply_arm7_irq24(cpu, (uint32_t *)io) == 0);
	assert(read32(io, IO_IF) == WIFI_IRQ);
	assert(read32(cpu, CPU_ACTIVE) == WIFI_IRQ);
}

static void test_masking_and_existing_flags(void)
{
	uint8_t cpu[0x2108] = {0};
	uint8_t io[0x218] = {0};
	write32(io, IO_IF, UINT32_C(0x00000008));
	write32(io, IO_IME, 1);
	write32(io, IO_IE, UINT32_C(0x00000008));
	dwifi_apply_arm7_irq24(cpu, (uint32_t *)io);
	assert(read32(io, IO_IF) == (WIFI_IRQ | UINT32_C(0x00000008)));
	assert(read32(cpu, CPU_ACTIVE) == UINT32_C(0x00000008));

	write32(io, IO_IME, 0);
	dwifi_apply_arm7_irq24(cpu, (uint32_t *)io);
	assert(read32(cpu, CPU_ACTIVE) == 0);
}

static void test_owned_cpu_state_is_not_overwritten(void)
{
	uint8_t cpu[0x2108] = {0};
	uint8_t io[0x218] = {0};
	cpu[CPU_STATE] = 2;
	write32(cpu, CPU_ACTIVE, UINT32_C(0xfeedbeef));
	dwifi_apply_arm7_irq24(cpu, (uint32_t *)io);
	assert(read32(io, IO_IF) == WIFI_IRQ);
	assert(read32(cpu, CPU_ACTIVE) == UINT32_C(0xfeedbeef));
}

static void test_exact_cycle_conversion(void)
{
	struct dwifi_cycle_clock clock = {0};
	struct dwifi_model model;
	dwifi_model_reset(&model);
	assert(dwifi_cycle_clock_advance(&clock, &model,
		DWIFI_ARM9_CYCLES_PER_SECOND) == UINT64_C(1000000));
	assert(model.us_timestamp == UINT64_C(1000000));
	assert(clock.scaled_remainder == 0);
}

static void test_chunked_cycle_conversion_retains_fraction(void)
{
	struct dwifi_cycle_clock clock = {0};
	struct dwifi_model model;
	uint64_t converted = 0;
	dwifi_model_reset(&model);
	for (unsigned index = 0; index < 1000; ++index)
		converted += dwifi_cycle_clock_advance(&clock, &model,
			DWIFI_ARM9_CYCLES_PER_SECOND / 1000);
	converted += dwifi_cycle_clock_advance(&clock, &model,
		DWIFI_ARM9_CYCLES_PER_SECOND % 1000);
	assert(converted == UINT64_C(1000000));
	assert(model.us_timestamp == UINT64_C(1000000));
	assert(clock.scaled_remainder == 0);
}

int main(void)
{
	test_latch_and_active_interrupt();
	test_masking_and_existing_flags();
	test_owned_cpu_state_is_not_overwritten();
	test_exact_cycle_conversion();
	test_chunked_cycle_conversion_retains_fraction();
	assert(dwifi_apply_arm7_irq24(NULL, NULL) == -1);
	puts("drastic-wifi adapter tests passed");
	return 0;
}
