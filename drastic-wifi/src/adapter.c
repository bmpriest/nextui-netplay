#include "drastic_wifi_adapter.h"

#include <stddef.h>
#include <string.h>

#define ARM7_IO_REGION_OFFSET        UINT32_C(0x2080)
#define ARM7_IO_IME_OFFSET           UINT32_C(0x0208)
#define ARM7_IO_IE_OFFSET            UINT32_C(0x0210)
#define ARM7_IO_IF_OFFSET            UINT32_C(0x0214)
#define ARM7_WIFI_IRQ                UINT32_C(0x01000000)

#if defined(__arm__)
#define ARM7_ACTIVE_IRQ_OFFSET       UINT32_C(0x2100)
#define ARM7_STATE_OFFSET            UINT32_C(0x2105)
/* Recovered from DraStic 2.5.0.4 DWARF. The prologue validation in hook.c is
 * the executable-version gate which makes using these offsets safe. */
#define A30_MEMORY_SYSTEM_OFFSET     UINT32_C(0x0fba1c)
#define A30_SYSTEM_CPU_ARM7_OFFSET   UINT32_C(0x1dad188)
#elif defined(__aarch64__)
#define ARM7_ACTIVE_IRQ_OFFSET       UINT32_C(0x2108)
#define ARM7_STATE_OFFSET            UINT32_C(0x2110)
#define BRICK_SYSTEM_CPU_ARM7_OFFSET UINT32_C(0x25cd340)
#define BRICK_ARM7_IO_OFFSET         UINT32_C(0x00023070)
#else
#define ARM7_ACTIVE_IRQ_OFFSET       UINT32_C(0x2100)
#define ARM7_STATE_OFFSET            UINT32_C(0x2105)
#endif

static uint32_t load_u32(const void *address)
{
	uint32_t value;
	memcpy(&value, address, sizeof(value));
	return value;
}

static void store_u32(void *address, uint32_t value)
{
	memcpy(address, &value, sizeof(value));
}

int dwifi_apply_arm7_irq24(void *cpu_pointer, uint32_t *io)
{
	if (!cpu_pointer || !io)
		return -1;
	uint8_t *cpu = cpu_pointer;
	uint32_t flags = load_u32((uint8_t *)io + ARM7_IO_IF_OFFSET);
	flags |= ARM7_WIFI_IRQ;
	store_u32((uint8_t *)io + ARM7_IO_IF_OFFSET, flags);

	/* This is DraStic's event-device pattern. States masked by 0x06 own their
	 * transition; otherwise active_interrupts is the enabled, globally unmasked
	 * subset. A halted ARM7 uses state 1 and therefore reaches this path. */
	if (!(cpu[ARM7_STATE_OFFSET] & UINT8_C(0x06))) {
		uint32_t ime = load_u32((uint8_t *)io + ARM7_IO_IME_OFFSET);
		uint32_t enabled = load_u32((uint8_t *)io + ARM7_IO_IE_OFFSET);
		store_u32(cpu + ARM7_ACTIVE_IRQ_OFFSET,
			(UINT32_C(0) - ime) & enabled & flags);
	}
	return 0;
}

int dwifi_irq_adapter_init(struct dwifi_irq_adapter *adapter, void *memory,
	void *system)
{
	if (!adapter || !memory)
		return -1;
	memset(adapter, 0, sizeof(*adapter));
#if defined(__arm__)
	(void)system;
	uint32_t system_address = load_u32((uint8_t *)memory +
		A30_MEMORY_SYSTEM_OFFSET);
	if (!system_address || (system_address & 3))
		return -1;
	uint8_t *cpu = (uint8_t *)((uintptr_t)system_address +
		(uintptr_t)A30_SYSTEM_CPU_ARM7_OFFSET);
	uint32_t io_address = load_u32(cpu + ARM7_IO_REGION_OFFSET);
	if (!io_address || (io_address & 3))
		return -1;
	adapter->cpu = cpu;
	adapter->io = (uint32_t *)(uintptr_t)io_address;
	return 0;
#elif defined(__aarch64__)
	if (!system || ((uintptr_t)system & 7))
		return -1;
	adapter->cpu = (uint8_t *)system + BRICK_SYSTEM_CPU_ARM7_OFFSET;
	adapter->io = (uint32_t *)((uint8_t *)memory + BRICK_ARM7_IO_OFFSET);
	return 0;
#else
	(void)memory;
	(void)system;
	return -1;
#endif
}

void dwifi_irq_adapter_raise(void *opaque)
{
	struct dwifi_irq_adapter *adapter = opaque;
	if (adapter)
		(void)dwifi_apply_arm7_irq24(adapter->cpu, adapter->io);
}

uint64_t dwifi_cycle_clock_advance(struct dwifi_cycle_clock *clock,
	struct dwifi_model *model, uint64_t elapsed_cycles)
{
	if (!clock || !model)
		return 0;
	uint64_t seconds = elapsed_cycles / DWIFI_ARM9_CYCLES_PER_SECOND;
	uint64_t cycles = elapsed_cycles % DWIFI_ARM9_CYCLES_PER_SECOND;
	uint64_t scaled = cycles * UINT64_C(1000000) +
		clock->scaled_remainder;
	uint64_t elapsed_us = seconds * UINT64_C(1000000) +
		scaled / DWIFI_ARM9_CYCLES_PER_SECOND;
	clock->scaled_remainder = scaled % DWIFI_ARM9_CYCLES_PER_SECOND;
	dwifi_model_advance_us(model, elapsed_us);
	return elapsed_us;
}
