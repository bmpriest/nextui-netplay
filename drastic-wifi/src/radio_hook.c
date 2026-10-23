/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Attaches the DS radio (radio/radio.cpp: DSperate's Wi-Fi block and melonDS
 * LAN transport) to DraStic 2.5.2.0 on AArch64 (build ID
 * 5665dbdad6bec617b3f828afb2f69b0a14460764, the "59a7" release).
 *
 * Three DraStic functions are detoured, at the offsets and prologues that the
 * diagnostic hook (hook.c) validated on hardware:
 *
 *   load_wifi_16   every Wi-Fi read goes to the radio;
 *   ARM7 store16   writes in the Wi-Fi window go to the radio, the rest to
 *                  DraStic;
 *   execute_events the elapsed master cycles become radio time, and once per
 *                  emulated frame the LAN session is serviced.
 *
 * The radio uses DraStic's own 8 KB Wi-Fi RAM, which the guest reads and
 * writes directly, so there is no shadow copy to keep in step. Its interrupts
 * reach the ARM7 through DraStic's IF/IE/IME and active-interrupt fields.
 *
 * Nothing is installed unless DRASTIC_WIFI_LAN asks for a session:
 *
 *   DRASTIC_WIFI_LAN=host | join:<IPv4>
 *   DRASTIC_WIFI_LAN_NAME=<player name>          (optional)
 *   DRASTIC_WIFI_LAN_TIMEOUT_MS=<5..250>         (optional; 25 by default)
 *   DRASTIC_WIFI_FIRMWARE=<path>                 (default system/nds_firmware.bin)
 */
#define _GNU_SOURCE

#include "drastic_wifi_hook.h"
#include "drastic_wifi_radio.h"

#include <elf.h>
#include <fcntl.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(__aarch64__)
#error "radio_hook.c supports the AArch64 DraStic 2.5.2.0 build only"
#endif

#define PATCH_WORDS 4
#define TRAMPOLINE_WORDS 8

/* Offsets inside DraStic's ARM7 memory context and system structure, all
 * hardware-validated on this build (see adapter.c and
 * docs/drastic-brick-devbridge-progress.md). */
#define WIFI_RAM_OFFSET          UINT32_C(0x00017070)
#define ARM7_IO_OFFSET           UINT32_C(0x00023070)
#define SYSTEM_CPU_ARM7_OFFSET   UINT32_C(0x025cd340)
#define ARM7_IO_IME              UINT32_C(0x0208)
#define ARM7_IO_IE               UINT32_C(0x0210)
#define ARM7_IO_IF               UINT32_C(0x0214)
#define ARM7_IO_POWCNT2          UINT32_C(0x0304)
#define ARM7_ACTIVE_IRQ          UINT32_C(0x2108)
#define ARM7_STATE               UINT32_C(0x2110)
#define ARM7_WIFI_IRQ            UINT32_C(0x01000000)

#define ARM9_HZ        UINT64_C(67027964)
/* 263 lines x 2130 ARM7 cycles = 560190 per frame, in ARM9 cycles. */
#define FRAME_CYCLES   UINT64_C(1120380)

static const uint32_t load16_prologue[] = {
	UINT32_C(0x12003c22), /* and w2, w1, #0xffff */
	UINT32_C(0x5287ffe3), /* mov w3, #0x3fff */
	UINT32_C(0x51401042), /* sub w2, w2, #4, lsl #12 */
	UINT32_C(0x0a030021), /* and w1, w1, w3 */
};
static const uint32_t store16_prologue[] = {
	UINT32_C(0xa9bc7bfd), /* stp x29, x30, [sp, #-0x40]! */
	UINT32_C(0x910003fd), /* mov x29, sp */
	UINT32_C(0xa90153f3), /* stp x19, x20, [sp, #0x10] */
	UINT32_C(0xaa0003f3), /* mov x19, x0 */
};
static const uint32_t execute_events_prologue[] = {
	UINT32_C(0xa9be7bfd), /* stp x29, x30, [sp, #-0x20]! */
	UINT32_C(0x910003fd), /* mov x29, sp */
	UINT32_C(0xb9401002), /* ldr w2, [x0, #0x10] */
	UINT32_C(0xa90153f3), /* stp x19, x20, [sp, #0x10] */
};

typedef uint16_t (*load16_fn)(void *, uint32_t);
typedef void (*store16_fn)(void *, uint32_t, uint32_t);
typedef void (*execute_events_fn)(void *);

static load16_fn original_load16;
static store16_fn original_store16;
static execute_events_fn original_execute_events;

static uintptr_t load_address, store_address, event_address;

static void *memory_context; /* DraStic's ARM7 memory context */
static void *system_context; /* DraStic's system structure */
static int bound;            /* the radio has DraStic's RAM and IRQ line */
static int bind_failed;
static uint64_t cycle_remainder; /* scaled sub-microsecond cycles */
static uint64_t frame_cycles;
static uint64_t irq_count;

static void log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	int length = vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (length > 0)
		(void)write(STDERR_FILENO, line,
			(size_t)length < sizeof(line) ? (size_t)length : sizeof(line) - 1);
}

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

/* ---- DraStic's side of the radio ---------------------------------------- */

static int powcnt2_on(void *opaque)
{
	uint8_t *io = (uint8_t *)opaque + ARM7_IO_OFFSET;
	uint16_t powcnt2;
	memcpy(&powcnt2, io + ARM7_IO_POWCNT2, sizeof(powcnt2));
	return (powcnt2 & 2) != 0;
}

/* DraStic's own device-interrupt pattern: latch IF, and unless the CPU state
 * owns the transition, refresh the active set (enabled, IME-unmasked). */
static void raise_irq(void *opaque)
{
	uint8_t *io = (uint8_t *)opaque + ARM7_IO_OFFSET;
	if (!system_context)
		return;
	uint8_t *cpu = (uint8_t *)system_context + SYSTEM_CPU_ARM7_OFFSET;
	uint32_t flags = load_u32(io + ARM7_IO_IF) | ARM7_WIFI_IRQ;
	store_u32(io + ARM7_IO_IF, flags);
	if (!(cpu[ARM7_STATE] & UINT8_C(0x06))) {
		uint32_t ime = load_u32(io + ARM7_IO_IME);
		uint32_t enabled = load_u32(io + ARM7_IO_IE);
		store_u32(cpu + ARM7_ACTIVE_IRQ, (UINT32_C(0) - ime) & enabled & flags);
	}
	irq_count++;
}

/* The radio must use the RAM the guest reads. Confirm the build's offset by
 * writing a word through DraStic's own store path (safe in the low half) and
 * finding it there, then restore it. */
static int bind_radio(void *memory)
{
	if (bound)
		return 1;
	if (bind_failed || !memory)
		return 0;
	uint8_t *ram = (uint8_t *)memory + WIFI_RAM_OFFSET;
	uint16_t saved, seen;
	memcpy(&saved, ram + 0x0ffe, sizeof(saved));
	original_store16(memory, UINT32_C(0x00804ffe), UINT32_C(0x5aa5));
	memcpy(&seen, ram + 0x0ffe, sizeof(seen));
	uint16_t loaded = original_load16(memory, UINT32_C(0x4ffe));
	memcpy(ram + 0x0ffe, &saved, sizeof(saved));
	if (seen != 0x5aa5 || loaded != 0x5aa5) {
		bind_failed = 1;
		log_line("drastic-wifi: Wi-Fi RAM not at +0x%x (saw %04x, loaded %04x);"
			" radio disabled\n", (unsigned)WIFI_RAM_OFFSET, seen, loaded);
		return 0;
	}
	memory_context = memory;
	dwr_bind(memory, ram, powcnt2_on, raise_irq);
	bound = 1;
	log_line("drastic-wifi: radio bound (Wi-Fi RAM +0x%x, POWCNT2 %d)\n",
		(unsigned)WIFI_RAM_OFFSET, powcnt2_on(memory));
	return 1;
}

/* ---- the detours ------------------------------------------------------- */

static uint16_t hooked_load16(void *memory, uint32_t address)
{
	if (bind_radio(memory))
		return dwr_read16(UINT32_C(0x04800000) | (address & UINT32_C(0xffff)));
	return original_load16(memory, address);
}

static void hooked_store16(void *memory, uint32_t address, uint32_t value)
{
	/* The ARM7 16-bit I/O dispatcher sees every I/O write, with the address
	 * truncated to 24 bits: Wi-Fi is 0x80xxxx (DS 0x0480xxxx). */
	if ((address & UINT32_C(0xff0000)) == UINT32_C(0x800000) && bind_radio(memory)) {
		dwr_write16(UINT32_C(0x04000000) | address, (uint16_t)value);
		return;
	}
	original_store16(memory, address, value);
}

static void hooked_execute_events(void *system)
{
	uint64_t before, after;
	memcpy(&before, (uint8_t *)system + 8, sizeof(before));
	original_execute_events(system);
	memcpy(&after, (uint8_t *)system + 8, sizeof(after));
	system_context = system;
	/* A reset or state load can move the counter backwards: count no time. */
	uint64_t elapsed = after >= before ? after - before : 0;

	uint64_t scaled = elapsed * UINT64_C(1000000) + cycle_remainder;
	cycle_remainder = scaled % ARM9_HZ;
	uint64_t elapsed_us = scaled / ARM9_HZ;
	if (bound && elapsed_us)
		dwr_advance_us(elapsed_us);

	frame_cycles += elapsed;
	while (frame_cycles >= FRAME_CYCLES) {
		frame_cycles -= FRAME_CYCLES;
		dwr_frame();
	}
}

/* ---- installation ------------------------------------------------------ */

static void make_jump(uint32_t words[PATCH_WORDS], uintptr_t destination)
{
	dwifi_make_aarch64_jump(words, destination);
}

static void *make_trampoline(uintptr_t target, const uint32_t *expected)
{
	uint32_t *code = mmap(NULL, (size_t)sysconf(_SC_PAGESIZE),
		PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (code == MAP_FAILED)
		return NULL;
	memcpy(code, expected, sizeof(uint32_t) * PATCH_WORDS);
	make_jump(&code[PATCH_WORDS], target + sizeof(uint32_t) * PATCH_WORDS);
	__builtin___clear_cache((char *)code, (char *)(code + TRAMPOLINE_WORDS));
	return code;
}

static int install_hook(uintptr_t target, const uint32_t *expected,
	uintptr_t replacement, void **trampoline)
{
	long page_size = sysconf(_SC_PAGESIZE);
	uintptr_t page = target & ~((uintptr_t)page_size - 1);
	uint32_t patch[PATCH_WORDS];
	if (!dwifi_words_match((const void *)target, expected, PATCH_WORDS))
		return -1;
	*trampoline = make_trampoline(target, expected);
	if (!*trampoline)
		return -1;
	if (mprotect((void *)page, (size_t)page_size * 2,
		PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
		return -1;
	make_jump(patch, replacement);
	memcpy((void *)target, patch, sizeof(patch));
	__sync_synchronize();
	__builtin___clear_cache((char *)target, (char *)(target + sizeof(patch)));
	return 0;
}

static uintptr_t find_main_image(void)
{
	const Elf64_Phdr *headers = (const Elf64_Phdr *)(uintptr_t)getauxval(AT_PHDR);
	size_t count = (size_t)getauxval(AT_PHNUM);
	for (size_t index = 0; index < count; ++index) {
		if (headers[index].p_type == PT_PHDR)
			return (uintptr_t)headers - (uintptr_t)headers[index].p_vaddr;
	}
	return 0;
}

/* Advanced DraStic installs its own detours early in main; patching from the
 * constructor crashed it (hook.c's finding). The game does not touch Wi-Fi
 * for seconds after start, so install once the frontend is up. execute_events
 * goes in first so the system pointer is known before the first MMIO access. */
static void *deferred_install(void *unused)
{
	(void)unused;
	sleep(3);
	if (install_hook(event_address, execute_events_prologue,
		(uintptr_t)hooked_execute_events, (void **)&original_execute_events) != 0 ||
	    install_hook(load_address, load16_prologue,
		(uintptr_t)hooked_load16, (void **)&original_load16) != 0 ||
	    install_hook(store_address, store16_prologue,
		(uintptr_t)hooked_store16, (void **)&original_store16) != 0) {
		log_line("drastic-wifi: hook install failed; no local wireless\n");
		return NULL;
	}
	log_line("drastic-wifi: hooks installed\n");
	return NULL;
}

static int read_file(const char *path, uint8_t **data, size_t *size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	size_t capacity = 1024 * 1024, used = 0;
	uint8_t *buffer = malloc(capacity);
	if (!buffer) {
		close(fd);
		return -1;
	}
	for (;;) {
		ssize_t n = read(fd, buffer + used, capacity - used);
		if (n <= 0)
			break;
		used += (size_t)n;
		if (used == capacity)
			break;
	}
	close(fd);
	*data = buffer;
	*size = used;
	return 0;
}

static int running_drastic(void)
{
	char cmdline[4096];
	int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	ssize_t length = read(fd, cmdline, sizeof(cmdline));
	close(fd);
	return length > 0 && dwifi_is_drastic_command(cmdline, (size_t)length);
}

__attribute__((constructor))
static void radio_initialize(void)
{
	if (!running_drastic())
		return;
	const char *lan = getenv("DRASTIC_WIFI_LAN");
	const char *name = getenv("DRASTIC_WIFI_LAN_NAME");
	const char *timeout = getenv("DRASTIC_WIFI_LAN_TIMEOUT_MS");
	const char *firmware_path = getenv("DRASTIC_WIFI_FIRMWARE");
	if (!lan || !*lan)
		return;

	struct dwr_config config;
	memset(&config, 0, sizeof(config));
	char peer[64] = "";
	if (strcmp(lan, "host") == 0) {
		config.host = 1;
	} else if (strncmp(lan, "join:", 5) == 0 && strlen(lan + 5) < sizeof(peer)) {
		strcpy(peer, lan + 5);
		config.peer = peer;
	} else {
		log_line("drastic-wifi: DRASTIC_WIFI_LAN=%s not understood\n", lan);
		return;
	}
	char player[32] = "";
	if (name)
		snprintf(player, sizeof(player), "%s", name);
	config.name = player;
	config.timeout_ms = timeout ? atoi(timeout) : 0;
	uint8_t *firmware = NULL;
	size_t firmware_size = 0;
	if (read_file(firmware_path && *firmware_path ? firmware_path :
		"system/nds_firmware.bin", &firmware, &firmware_size) != 0)
		log_line("drastic-wifi: no firmware image; channels will not decode\n");
	config.firmware = firmware;
	config.firmware_size = firmware_size;

	/* DraStic's UI starts helper processes; they must not inherit this. */
	unsetenv("LD_PRELOAD");
	unsetenv("DRASTIC_WIFI_LAN");
	unsetenv("DRASTIC_WIFI_LAN_NAME");
	unsetenv("DRASTIC_WIFI_LAN_TIMEOUT_MS");
	unsetenv("DRASTIC_WIFI_FIRMWARE");

	uintptr_t image = find_main_image();
	if (!image) {
		log_line("drastic-wifi: main image not found\n");
		return;
	}
	load_address = image + DWIFI_BRICK_LOAD16_OFFSET;
	store_address = image + DWIFI_BRICK_STORE16_OFFSET;
	event_address = image + DWIFI_BRICK_EXECUTE_EVENTS_OFFSET;
	if (!dwifi_words_match((const void *)load_address, load16_prologue, PATCH_WORDS) ||
	    !dwifi_words_match((const void *)store_address, store16_prologue, PATCH_WORDS) ||
	    !dwifi_words_match((const void *)event_address, execute_events_prologue, PATCH_WORDS)) {
		log_line("drastic-wifi: not the supported DraStic build; no local wireless\n");
		return;
	}
	if (dwr_init(&config) != 0) {
		log_line("drastic-wifi: radio init failed\n");
		return;
	}
	free(firmware);
	log_line("drastic-wifi: %s%s, firmware %zu bytes\n",
		config.host ? "hosting" : "joining ", config.host ? "" : peer, firmware_size);

	pthread_t thread;
	if (pthread_create(&thread, NULL, deferred_install, NULL) == 0)
		(void)pthread_detach(thread);
}

__attribute__((destructor))
static void radio_shutdown(void)
{
	if (bound)
		log_line("drastic-wifi: %llu Wi-Fi interrupts raised\n",
			(unsigned long long)irq_count);
	dwr_shutdown();
}
