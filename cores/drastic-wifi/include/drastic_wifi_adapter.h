#ifndef DRASTIC_WIFI_ADAPTER_H
#define DRASTIC_WIFI_ADAPTER_H

#include <stdint.h>

#include "drastic_wifi_model.h"

#define DWIFI_ARM9_CYCLES_PER_SECOND UINT64_C(67027964)

struct dwifi_irq_adapter {
	void *cpu;
	uint32_t *io;
};

struct dwifi_cycle_clock {
	uint64_t scaled_remainder;
};

/* Portable part of DraStic's device-IRQ sequence. Exposed for a native test;
 * the version-specific pointer recovery remains target-only. */
int dwifi_apply_arm7_irq24(void *cpu, uint32_t *io);

/* Bind to the exact executable supported by this build. Returns zero only when
 * the target ABI has been recovered and the adapter is ready. */
int dwifi_irq_adapter_init(struct dwifi_irq_adapter *adapter, void *memory,
	void *system);
void dwifi_irq_adapter_raise(void *opaque);

/* Convert DraStic's 67.027964 MHz global/master cycles exactly, retaining the
 * fractional remainder across event callbacks. Returns whole microseconds. */
uint64_t dwifi_cycle_clock_advance(struct dwifi_cycle_clock *clock,
	struct dwifi_model *model, uint64_t elapsed_cycles);

#endif
