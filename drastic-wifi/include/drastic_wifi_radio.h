/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The DS radio the DraStic hook drives: DSperate's port of melonDS's Wi-Fi
 * block and LAN transport, behind a C interface. The hook owns DraStic (its
 * MMIO handlers, event loop, frame RAM and ARM7 interrupt line); this owns the
 * radio and the session. Everything here runs on DraStic's emulation thread.
 */
#ifndef DRASTIC_WIFI_RADIO_H
#define DRASTIC_WIFI_RADIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dwr_config {
	int host;                  /* 1: host the session; 0: join `peer` */
	const char *peer;          /* the host's IPv4 address, when joining */
	const char *name;          /* our player name in the LAN session */
	int timeout_ms;            /* bound on one MP wait; 0: DSperate's 25 ms */
	const uint8_t *firmware;   /* this console's firmware image */
	size_t firmware_size;
};

/* Copies what it needs from `config`. Opens nothing yet: the session starts on
 * the first dwr_frame(), on the emulation thread. Returns 0 on success. */
int dwr_init(const struct dwr_config *config);

/* DraStic's side of the radio, known once the emulator is running. */
void dwr_bind(void *opaque, uint8_t *frame_ram, int (*powcnt2_on)(void *),
	void (*raise_irq)(void *));

/* Wi-Fi MMIO, `address` being the DS address (0x048xxxxx). */
uint16_t dwr_read16(uint32_t address);
void dwr_write16(uint32_t address, uint16_t value);

/* Emulated time since the last call, in microseconds. */
void dwr_advance_us(uint64_t elapsed_us);

/* Once per emulated frame: services the LAN session (ENet, a pending join,
 * the pacing log). */
void dwr_frame(void);

/* Ends the session and writes the summary. */
void dwr_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
