#define _GNU_SOURCE

#include "drastic_wifi_hook.h"
#include "drastic_wifi_adapter.h"
#include "drastic_wifi_model.h"
#include "drastic_wifi_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#if defined(__aarch64__)
#include <sys/auxv.h>
#endif
#include <time.h>
#include <unistd.h>

#define DWIFI_TRACE_LIMIT UINT64_C(16384)
#define DWIFI_ARM_PATCH_WORDS 2
#define DWIFI_ARM_TRAMPOLINE_WORDS 4
#define DWIFI_A64_PATCH_WORDS 4
#define DWIFI_A64_TRAMPOLINE_WORDS 8

#ifndef DWIFI_NO_RUNTIME
#if defined(__arm__)
#define DWIFI_PATCH_WORDS DWIFI_ARM_PATCH_WORDS
static const uint32_t load16_prologue[] = {
	UINT32_C(0xe6ff2071), /* uxth r2, r1 */
	UINT32_C(0xe1a03000), /* mov  r3, r0 */
};

static const uint32_t store16_prologue[] = {
	UINT32_C(0xe6ff3071), /* uxth r3, r1 */
	UINT32_C(0xe7ed1051), /* ubfx r1, r1, #0, #14 */
};

static const uint32_t execute_events_prologue[] = {
	UINT32_C(0xe5902010), /* ldr  r2, [r0, #16] */
	UINT32_C(0xe16d41f0), /* strd r4, [sp, #-16]! */
};
#elif defined(__aarch64__)
#define DWIFI_PATCH_WORDS DWIFI_A64_PATCH_WORDS
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
#else
#error "The DraStic Wi-Fi runtime hook supports ARMv7 and AArch64 only"
#endif
#endif

uint32_t dwifi_register_offset(uint32_t address)
{
	return address & UINT32_C(0x3fff);
}

int dwifi_brick_dispatcher_is_wifi(uint32_t address)
{
	return (address & UINT32_C(0x8000)) != 0;
}

uint32_t dwifi_brick_packet_store_address(uint16_t offset)
{
	/* The dispatchers take the guest address with different truncation: load16
	 * receives it masked to 16 bits and store16 to 24. Registers are traced as
	 * read 0x00008038 / write 0x00808038, i.e. DS 0x04808038. Wi-Fi RAM lives
	 * below the registers at DS 0x04804000, so it is 0x4000 for loads and
	 * 0x00804000 for stores. Base 0x0080c000 reads back consistently but is a
	 * separate region the guest never sees: a host beacon at packet offset
	 * 0x0238 reads correctly at 0x4238 and as zero at 0xc238. */
	return UINT32_C(0x00804000) + (offset & UINT16_C(0x1ffe));
}

void dwifi_make_absolute_jump(uint32_t words[2], uintptr_t destination)
{
	words[0] = DWIFI_ARM_JUMP_WORD;
	words[1] = (uint32_t)destination;
}

void dwifi_make_aarch64_jump(uint32_t words[4], uintptr_t destination)
{
	uint64_t address = (uint64_t)destination;
	words[0] = DWIFI_A64_LDR_X16_LITERAL;
	words[1] = DWIFI_A64_BR_X16;
	memcpy(&words[2], &address, sizeof(address));
}

int dwifi_words_match(const void *actual, const uint32_t *expected,
	size_t word_count)
{
	return actual && expected &&
		memcmp(actual, expected, sizeof(uint32_t) * word_count) == 0;
}

int dwifi_prologue_matches(const void *actual, const uint32_t expected[2])
{
	return dwifi_words_match(actual, expected, 2);
}

static int argument_is_basename(const char *argument, size_t length,
	const char *expected)
{
	const char *base = argument;
	for (size_t index = 0; index < length; ++index) {
		if (argument[index] == '/')
			base = argument + index + 1;
	}
	return strlen(expected) == (size_t)(argument + length - base) &&
		memcmp(base, expected, (size_t)(argument + length - base)) == 0;
}

int dwifi_is_drastic_command(const char *cmdline, size_t length)
{
	if (!cmdline || !length)
		return 0;
	const char *first_end = memchr(cmdline, '\0', length);
	if (!first_end)
		return 0;
	size_t first_length = (size_t)(first_end - cmdline);
	if (argument_is_basename(cmdline, first_length, "drastic"))
		return 1;

	const char *first_base = cmdline;
	for (size_t index = 0; index < first_length; ++index) {
		if (cmdline[index] == '/')
			first_base = cmdline + index + 1;
	}
	size_t base_length = (size_t)(first_end - first_base);
	if (base_length < strlen("ld-linux") ||
	    memcmp(first_base, "ld-linux", strlen("ld-linux")) != 0)
		return 0;

	/* The packaged invocation places --library-path and its value between the
	 * loader and ./drastic. Accept the target anywhere after a verified loader. */
	const char *argument = first_end + 1;
	while (argument < cmdline + length) {
		const char *argument_end = memchr(argument, '\0',
			(size_t)(cmdline + length - argument));
		if (!argument_end)
			return 0;
		if (argument_is_basename(argument,
			(size_t)(argument_end - argument), "drastic"))
			return 1;
		argument = argument_end + 1;
	}
	return 0;
}

#ifndef DWIFI_NO_RUNTIME

typedef uint16_t (*load16_fn)(void *, uint32_t);
typedef void (*store16_fn)(void *, uint32_t, uint32_t);
typedef void (*execute_events_fn)(void *);

static load16_fn original_load16;
static store16_fn original_store16;
static execute_events_fn original_execute_events;
static int trace_fd = -1;
static uint64_t event_sequence;
static uint64_t load_count;
static uint64_t store_count;
static uint64_t dropped_count;
static uint64_t poll_019c_count;
static struct dwifi_model wifi_model;
static struct dwifi_irq_adapter irq_adapter;
static struct dwifi_cycle_clock cycle_clock;
static int model_enabled;
static int cycle_enabled;
static int irq_enabled;
static int adapter_attempted;
static void *observed_system;
static void *observed_memory;
static uint64_t event_call_count;
static uint64_t emulated_cycle_count;
static uint64_t irq_injection_count;
static uint64_t last_transport_poll_us;
static uint64_t last_beacon_refresh_us;
static int load_entry_traced;
static int load_return_traced;
static uint64_t packet_sync_count;
static int packet_writer_ready;
static uint32_t last_beacon_hash;
static unsigned beacon_snapshot_count;
static int verbose_trace;

static void hooked_raise_irq(void *opaque);
static void store_trace_hex16(char *output, uint16_t value);

static uint64_t monotonic_ns(void)
{
	struct timespec now = {0};
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void trace_text(const char *message)
{
	if (trace_fd >= 0 && message)
		(void)write(trace_fd, message, strlen(message));
}

#if defined(__aarch64__)
typedef int (*main_fn)(int, char **, char **);
typedef int (*libc_start_main_fn)(main_fn, int, char **, void (*)(void),
	void (*)(void), void (*)(void), void *);
static main_fn drastic_main;
static void start_deferred_hooks(void);

static int traced_main(int argc, char **argv, char **envp)
{
	trace_text("drastic-wifi stage=main-entered\n");
	start_deferred_hooks();
	return drastic_main(argc, argv, envp);
}

int __libc_start_main(main_fn main_function, int argc, char **argv,
	void (*init)(void), void (*fini)(void), void (*rtld_fini)(void),
	void *stack_end)
{
	libc_start_main_fn real_start = (libc_start_main_fn)dlsym(RTLD_NEXT,
		"__libc_start_main");
	if (!real_start)
		_exit(127);
	drastic_main = main_function;
	return real_start(traced_main, argc, argv, init, fini, rtld_fini,
		stack_end);
}
#endif

#if defined(__aarch64__)
static void trace_addresses(const char *stage, uintptr_t first,
	uintptr_t second)
{
	char line[144];
	int length = snprintf(line, sizeof(line),
		"drastic-wifi stage=%s first=%llx second=%llx\n", stage,
		(unsigned long long)first, (unsigned long long)second);
	if (length > 0)
		(void)write(trace_fd, line, (size_t)length);
}
#endif

static void trace_event(enum dwifi_operation operation, uint32_t address,
	uint16_t value)
{
	char line[160];
	if (operation == DWIFI_OP_LOAD16)
		__sync_fetch_and_add(&load_count, 1);
	else
		__sync_fetch_and_add(&store_count, 1);
	if (!verbose_trace)
		return;

	/* Mario Kart polls 0x019c hundreds of thousands of times and otherwise
	 * consumes the complete trace before later register activity is visible.
	 * Preserve the startup burst and periodic evidence while counting every
	 * access in the summary. */
	if (operation == DWIFI_OP_LOAD16 &&
	    dwifi_register_offset(address) == UINT32_C(0x019c)) {
		uint64_t polls = __sync_add_and_fetch(&poll_019c_count, 1);
		if (polls > 16 && polls % 1024 != 0)
			return;
	}

	uint64_t sequence = __sync_fetch_and_add(&event_sequence, 1);

	if (sequence >= DWIFI_TRACE_LIMIT) {
		__sync_fetch_and_add(&dropped_count, 1);
		return;
	}

	int length = snprintf(line, sizeof(line),
		"wifi16 seq=%llu host_ns=%llu op=%c addr=%08x off=%04x value=%04x\n",
		(unsigned long long)sequence,
		(unsigned long long)monotonic_ns(),
		operation == DWIFI_OP_LOAD16 ? 'R' : 'W', address,
		dwifi_register_offset(address), value);
	if (length > 0)
		(void)write(trace_fd, line, (size_t)length);
}

static void ensure_model_adapter(void *memory)
{
	if (!model_enabled || adapter_attempted)
		return;
	#if defined(__aarch64__)
	if (!observed_system)
		return;
	#endif
	adapter_attempted = 1;
	if (dwifi_irq_adapter_init(&irq_adapter, memory, observed_system) == 0) {
		if (irq_enabled) {
			dwifi_model_set_irq_callback(&wifi_model,
				hooked_raise_irq, &irq_adapter);
			trace_text("drastic-wifi arm7-irq-adapter=ready irq=24 injection=enabled\n");
		} else {
			trace_text("drastic-wifi arm7-irq-adapter=ready irq=24 injection=disabled\n");
		}
	} else {
		trace_text("drastic-wifi arm7-irq-adapter=unavailable\n");
	}
}

static void hooked_raise_irq(void *opaque)
{
	__sync_fetch_and_add(&irq_injection_count, 1);
	dwifi_irq_adapter_raise(opaque);
	if (irq_injection_count <= 8)
		trace_text("drastic-wifi arm7-irq-injected\n");
}

static unsigned mirror_probe_count;


/* The Brick trace shows the guest never reaches packet RAM through the hooked
 * 16-bit dispatchers: every observed access is a register in the 0x8000/0x808000
 * window. Mirror stores into that same dispatcher are therefore unverified.
 * Read the word back through the load path already proven to return real packet
 * RAM (refresh_beacon_packet uses base 0x4000) and record whether it landed. */
/* Emitted before the mirror store so a faulting store is attributable to a
 * specific packet offset. Reading first proves the load dispatcher accepts the
 * same offset, isolating a store-side gap from an out-of-range address. */
static void probe_packet_store(void *opaque, uint16_t offset)
{
	if (mirror_probe_count >= 8 || !original_load16 || trace_fd < 0)
		return;
	static char line[] = "drastic-wifi packet-store off=0000 pre=0000\n";
	store_trace_hex16(strstr(line, "off=") + 4, offset);
	store_trace_hex16(strstr(line, "pre=") + 4,
		original_load16(opaque, UINT32_C(0x4000) + offset));
	(void)write(trace_fd, line, sizeof(line) - 1);
}

static void probe_packet_mirror(void *opaque, uint16_t offset, uint16_t value)
{
	if (mirror_probe_count++ >= 8 || !original_load16 || trace_fd < 0)
		return;
	static char line[] =
		"drastic-wifi mirror off=0000 wrote=0000 r4000=0000 rc000=0000\n";
	store_trace_hex16(strstr(line, "off=") + 4, offset);
	store_trace_hex16(strstr(line, "wrote=") + 6, value);
	store_trace_hex16(strstr(line, "r4000=") + 6,
		original_load16(opaque, UINT32_C(0x4000) + offset));
	store_trace_hex16(strstr(line, "rc000=") + 6,
		original_load16(opaque, UINT32_C(0xc000) + offset));
	(void)write(trace_fd, line, sizeof(line) - 1);
}

#if defined(__aarch64__)
static uint8_t *guest_packet_ram;
static int guest_packet_ram_attempted;
/* Signature read through the load dispatcher, which only reaches the low half
 * of Wi-Fi RAM. It holds the guest's transmit buffers, including the live
 * beacon, so it is unique within DraStic's memory context. */
static uint8_t packet_ram_signature[UINT32_C(0x1000)];

static const char *parse_hex_pointer(const char *text, uintptr_t *value)
{
	uintptr_t parsed = 0;
	int digits = 0;
	for (; *text; ++text) {
		unsigned digit;
		if (*text >= '0' && *text <= '9')
			digit = (unsigned)(*text - '0');
		else if (*text >= 'a' && *text <= 'f')
			digit = (unsigned)(*text - 'a') + 10;
		else
			break;
		parsed = (parsed << 4) | digit;
		digits++;
	}
	if (!digits)
		return NULL;
	*value = parsed;
	return text;
}

/* Hand-rolled so this stays usable from DraStic's small emulator stacks, where
 * scanf-family parsing is not safe. */
static int memory_region_bounds(const void *pointer, uint8_t **start,
	uint8_t **end)
{
	int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	static char maps[262144];
	size_t used = 0;
	ssize_t chunk;
	while (used + 1 < sizeof(maps) &&
	       (chunk = read(fd, maps + used, sizeof(maps) - 1 - used)) > 0)
		used += (size_t)chunk;
	(void)close(fd);
	maps[used] = 0;
	uintptr_t target = (uintptr_t)pointer;
	for (const char *line = maps; line && *line; ) {
		uintptr_t low = 0;
		uintptr_t high = 0;
		const char *cursor = parse_hex_pointer(line, &low);
		if (cursor && *cursor == '-' &&
		    (cursor = parse_hex_pointer(cursor + 1, &high)) != NULL &&
		    target >= low && target < high && cursor[1] == 'r') {
			*start = (uint8_t *)low;
			*end = (uint8_t *)high;
			return 1;
		}
		line = strchr(line, '\n');
		if (line)
			line++;
	}
	return 0;
}

/* DraStic's fast ARM7 path reads and writes Wi-Fi RAM directly, so the guest
 * never exercises the 16-bit dispatcher there and its upper half faults on both
 * load and store (the host's RX ring at packet offset 0x10c4 killed the
 * process). Locate the emulator's own packet RAM once by matching the low half
 * the dispatcher can still read, then mirror received frames straight into it. */
static void locate_guest_packet_ram(void *memory)
{
	if (guest_packet_ram_attempted || !memory || !original_load16)
		return;
	guest_packet_ram_attempted = 1;
	for (uint32_t offset = 0; offset < sizeof(packet_ram_signature);
	     offset += 2) {
		uint16_t word = original_load16(memory,
			UINT32_C(0x4000) + offset);
		packet_ram_signature[offset] = (uint8_t)word;
		packet_ram_signature[offset + 1] = (uint8_t)(word >> 8);
	}
	uint8_t *start = NULL;
	uint8_t *end = NULL;
	if (!memory_region_bounds(memory, &start, &end))
		return;
	/* The beacon-bearing transmit area is the distinctive part of the window. */
	const uint8_t *needle = packet_ram_signature + UINT32_C(0x0200);
	size_t needle_length = 128;
	uint8_t *cursor = start;
	while ((size_t)(end - cursor) >= needle_length) {
		uint8_t *hit = memmem(cursor, (size_t)(end - cursor), needle,
			needle_length);
		if (!hit)
			break;
		uint8_t *candidate = hit - UINT32_C(0x0200);
		if (candidate >= start &&
		    (size_t)(end - candidate) >= DWIFI_PACKET_RAM_SIZE &&
		    memcmp(candidate, packet_ram_signature,
			sizeof(packet_ram_signature)) == 0) {
			guest_packet_ram = candidate;
			if (trace_fd >= 0) {
				static char line[] = "drastic-wifi packet-ram=direct"
					" memory_offset=0000000000000000\n";
				uintptr_t delta = (uintptr_t)(candidate -
					(uint8_t *)memory);
				char *digits = strstr(line, "memory_offset=") + 14;
				for (unsigned index = 0; index < 4; ++index)
					store_trace_hex16(digits + index * 4,
						(uint16_t)(delta >> (48 - index * 16)));
				(void)write(trace_fd, line, sizeof(line) - 1);
			}
			return;
		}
		cursor = hit + 2;
	}
	trace_text("drastic-wifi packet-ram=not-found\n");
}
#endif

static void hooked_write_packet(void *opaque, uint16_t offset, uint16_t value)
{
#if defined(__aarch64__)
	if (guest_packet_ram) {
		uint16_t word = value;
		memcpy(guest_packet_ram + (offset & UINT16_C(0x1ffe)), &word,
			sizeof(word));
		return;
	}
	/* The dispatcher fallback is only safe for the low half. */
	if ((offset & UINT16_C(0x1ffe)) >= UINT32_C(0x1000))
		return;
	probe_packet_store(opaque, offset);
	original_store16(opaque, dwifi_brick_packet_store_address(offset), value);
	probe_packet_mirror(opaque, offset, value);
#else
	original_store16(opaque, UINT32_C(0x4000) + offset, value);
	probe_packet_store(opaque, offset);
	probe_packet_mirror(opaque, offset, value);
#endif
}

static unsigned receive_trace_count;
static unsigned receive_stage_trace_count;
static unsigned mp_receive_trace_count;
static uint16_t receive_header_offset;
static int join_trace_armed;
#if defined(__aarch64__)
static unsigned stored_dump_count;
#endif

static void store_trace_hex16(char *output, uint16_t value)
{
	static const char digits[] = "0123456789abcdef";
	for (unsigned index = 0; index < 4; ++index)
		output[index] = digits[(value >> (12 - index * 4)) & 15];
}

static void hooked_trace_receive(void *opaque, const char *stage,
	const uint8_t *frame, size_t length, uint16_t address)
{
	struct dwifi_model *model = opaque;
	if (receive_stage_trace_count++ < 12) {
		trace_text("drastic-wifi rx-stage=");
		trace_text(stage);
		trace_text("\n");
	}
	/* The address at "begin" is where this frame's RX header starts; by
	 * "rx-complete-irq" the cursor has moved past the body. */
	if (strcmp(stage, "begin") == 0) {
		receive_header_offset = address & UINT16_C(0x1ffe);
		/* Wi-Fi setup consumes the whole MMIO trace budget long before the
		 * join. The first non-beacon frame starts the association exchange,
		 * which is the part worth recording, so grant a second budget there. */
		if (!join_trace_armed && length >= 14) {
			uint16_t control = (uint16_t)(frame[12] |
				((uint16_t)frame[13] << 8));
			if (((control >> 2) & 3) != 0 || ((control >> 4) & 15) != 8) {
				join_trace_armed = 1;
				trace_text("drastic-wifi trace=rearmed-for-join\n");
				event_sequence = 0;
				dropped_count = 0;
			}
		}
	}
#if defined(__aarch64__)
	/* Read the stored frame back out of DraStic's packet RAM. The guest parses
	 * exactly these bytes, so comparing them with the frame the transport
	 * logged separates a delivery fault from a game-level rejection. */
	if (strcmp(stage, "body-written") == 0 && guest_packet_ram &&
	    stored_dump_count < 4 && trace_fd >= 0) {
		stored_dump_count++;
		static char line[16 + 2 * 108];
		static const char digits[] = "0123456789abcdef";
		size_t used = 0;
		const char *label = "rx-stored=";
		while (*label)
			line[used++] = *label++;
		size_t bytes = length < 108 ? length : 108;
		for (size_t index = 0; index < bytes; ++index) {
			uint8_t value = guest_packet_ram[(receive_header_offset + index) &
				(DWIFI_PACKET_RAM_SIZE - 1)];
			line[used++] = digits[value >> 4];
			line[used++] = digits[value & 15];
		}
		line[used++] = '\n';
		(void)write(trace_fd, line, used);
	}
#endif
	/* DraStic's event thread has almost no spare stack. Emit one fixed-buffer
	 * state snapshot per completed frame, avoiding stdio formatting entirely. */
	if (strcmp(stage, "rx-complete-irq") != 0 || receive_trace_count++ >= 64)
		goto trace_mp;
	static char line[] =
		"[drastic-wifi] rx-complete w_if=0000 w_ie=0000 irq=0 writer=0"
		" status=0000 rxlen=0000 at=0000\n";
	char *flags = strstr(line, "w_if=") + 5;
	char *enabled = strstr(line, "w_ie=") + 5;
	store_trace_hex16(flags,
		model ? dwifi_model_read16(model, UINT32_C(0x0010)) : 0);
	store_trace_hex16(enabled,
		model ? dwifi_model_read16(model, UINT32_C(0x0012)) : 0);
	/* The RX header the guest is about to parse: status word and the length it
	 * advertises, read back from the ring where the frame was stored. */
	store_trace_hex16(strstr(line, "status=") + 7, model ?
		dwifi_model_read16(model, UINT32_C(0x4000) + receive_header_offset) : 0);
	store_trace_hex16(strstr(line, "rxlen=") + 6, model ?
		dwifi_model_read16(model,
			UINT32_C(0x4000) + ((receive_header_offset + 8) & UINT16_C(0x1ffe))) : 0);
	store_trace_hex16(strstr(line, "at=") + 3, receive_header_offset);
	strstr(line, "irq=")[4] = model && dwifi_model_irq_pending(model) ? '1' : '0';
	strstr(line, "writer=")[7] = model && model->write_packet ? '1' : '0';
	ssize_t result = write(STDERR_FILENO, line, sizeof(line) - 1);
	(void)result;

trace_mp:
	/* Association traffic consumes the generic RX trace budget. Keep a small,
	 * independent record of the packets that decide MP membership, including
	 * the exact status the guest sees after the completion interrupt. */
	if (strcmp(stage, "rx-complete-irq") != 0 || length < 40 ||
		mp_receive_trace_count++ >= 48)
		return;
	const uint8_t *destination = frame + 16;
	uint16_t control = (uint16_t)(frame[12] | ((uint16_t)frame[13] << 8));
	if (((control >> 2) & 3) != 2)
		return;
	char mp_line[160];
	uint16_t status = model ? dwifi_model_read16(model,
		UINT32_C(0x4000) + receive_header_offset) : 0;
	int mp_length = snprintf(mp_line, sizeof(mp_line),
		"drastic-wifi data-rx fc=%04x dst=%02x%02x%02x%02x%02x%02x rate=%02x seq=%04x count=%04x fail=%04x status=%04x\n",
		(unsigned)control, (unsigned)destination[0], (unsigned)destination[1],
		(unsigned)destination[2], (unsigned)destination[3],
		(unsigned)destination[4], (unsigned)destination[5], (unsigned)frame[8],
		(unsigned)(frame[34] | ((uint16_t)frame[35] << 8)),
		(unsigned)(frame[36] | ((uint16_t)frame[37] << 8)),
		(unsigned)(frame[38] | ((uint16_t)frame[39] << 8)), (unsigned)status);
	if (mp_length > 0)
		(void)write(STDERR_FILENO, mp_line, (size_t)mp_length);
}

/* Packet RAM reads take the direct pointer when it is known and otherwise the
 * load dispatcher, which only reaches the low half of Wi-Fi RAM. */
static uint16_t guest_packet_ram_load16(void *memory, uint32_t offset)
{
#if defined(__aarch64__)
	if (guest_packet_ram) {
		uint16_t word;
		memcpy(&word, guest_packet_ram + (offset & UINT32_C(0x1ffe)),
			sizeof(word));
		return word;
	}
#endif
	return original_load16(memory, UINT32_C(0x4000) + offset);
}

static void refresh_beacon_packet(void *memory)
{
	if (!memory || !original_load16)
		return;
	uint16_t slot = dwifi_model_read16(&wifi_model, UINT32_C(0x0080));
	if (!(slot & UINT16_C(0x8000)))
		return;
	uint32_t packet_offset = (uint32_t)(slot & UINT16_C(0x0fff)) << 1;
	uint16_t frame_length = guest_packet_ram_load16(memory,
		packet_offset + 10) & UINT16_C(0x3fff);
	if (frame_length > UINT16_C(2036) ||
	    packet_offset + 12 + frame_length > DWIFI_PACKET_RAM_SIZE)
		return;
	uint32_t hash = UINT32_C(2166136261);
	for (uint32_t index = 0; index < UINT32_C(12) + frame_length; index += 2) {
		uint16_t word = guest_packet_ram_load16(memory,
			packet_offset + index);
		dwifi_model_write16(&wifi_model,
			UINT32_C(0x4000) + packet_offset + index, word);
		hash = (hash ^ (uint8_t)word) * UINT32_C(16777619);
		hash = (hash ^ (uint8_t)(word >> 8)) * UINT32_C(16777619);
	}
	if (hash != last_beacon_hash && beacon_snapshot_count < 4 && trace_fd >= 0) {
		last_beacon_hash = hash;
		beacon_snapshot_count++;
		char line[2 * (12 + 2036) + 96];
		int used = snprintf(line, sizeof(line),
			"drastic-wifi beacon-snapshot=%u len=%u hash=%08x hex=",
			beacon_snapshot_count, (unsigned)frame_length, hash);
		for (uint32_t index = 0;
		     index < UINT32_C(12) + frame_length && used > 0 &&
		     (size_t)used + 3 < sizeof(line); ++index) {
			uint16_t word = dwifi_model_read16(&wifi_model,
				UINT32_C(0x4000) + packet_offset + (index & ~UINT32_C(1)));
			uint8_t byte = index & 1 ? (uint8_t)(word >> 8) : (uint8_t)word;
			used += snprintf(line + used, sizeof(line) - (size_t)used,
				"%02x", byte);
		}
		if (used > 0 && (size_t)used + 1 < sizeof(line)) {
			line[used++] = '\n';
			(void)write(trace_fd, line, (size_t)used);
		}
	}
}

static uint16_t hooked_load16(void *memory, uint32_t address)
{
	observed_memory = memory;
	if (!load_entry_traced) {
		load_entry_traced = 1;
		trace_text("drastic-wifi load16=entered\n");
	}
	uint16_t value = original_load16(memory, address);
	if (!load_return_traced) {
		load_return_traced = 1;
		trace_text("drastic-wifi load16=original-returned\n");
	}
	/* Until the portable model has the complete power-on/register behavior,
	 * preserve DraStic's hardware-visible result. Keep a shadow read so traces
	 * and model access accounting still describe the guest's MMIO traffic. */
#if defined(__aarch64__)
	int wifi_access = dwifi_brick_dispatcher_is_wifi(address);
#else
	int wifi_access = 1;
#endif
	if (model_enabled && wifi_access) {
		uint16_t modeled = dwifi_model_read16(&wifi_model, address);
		if (dwifi_transport_is_active() && dwifi_model_owns_read(address))
			value = modeled;
	}
	if (wifi_access)
		trace_event(DWIFI_OP_LOAD16, address, value);
	return value;
}

static void hooked_store16(void *memory, uint32_t address, uint32_t value)
{
	observed_memory = memory;
	/* The Brick target is the ARM7 16-bit I/O dispatcher, not the raw Wi-Fi
	 * fallback. Only the 0x8000-0xbfff Wi-Fi window belongs in this trace. */
#if defined(__aarch64__)
	int wifi_access = dwifi_brick_dispatcher_is_wifi(address);
#else
	int wifi_access = 1;
#endif
	if (wifi_access)
		trace_event(DWIFI_OP_STORE16, address, (uint16_t)value);
	original_store16(memory, address, value);
	if (model_enabled && wifi_access) {
		uint32_t offset = dwifi_register_offset(address);
		if (dwifi_transport_is_active() &&
		    (offset == UINT32_C(0x0080) || offset == UINT32_C(0x0090) ||
		     offset == UINT32_C(0x00a0) || offset == UINT32_C(0x00a4) ||
		     offset == UINT32_C(0x00a8)) && (value & UINT32_C(0x8000))) {
			uint32_t packet_offset = (value & UINT32_C(0x0fff)) << 1;
			uint16_t frame_length = original_load16(memory,
				UINT32_C(0x4000) + packet_offset + 10) & UINT16_C(0x3fff);
			if (frame_length <= UINT16_C(2036) &&
			    packet_offset + 12 + frame_length <= DWIFI_PACKET_RAM_SIZE) {
				for (uint32_t index = 0;
				     index < UINT32_C(12) + (uint32_t)frame_length; index += 2) {
					uint16_t word = original_load16(memory,
						UINT32_C(0x4000) + packet_offset + index);
					dwifi_model_write16(&wifi_model,
						UINT32_C(0x4000) + packet_offset + index, word);
				}
				packet_sync_count++;
				char line[160];
				int length = snprintf(line, sizeof(line),
					"drastic-wifi packet-sync=%llu reg=%04x slot=%04x"
					" addr=%04x len=%u\n",
					(unsigned long long)packet_sync_count, (unsigned)offset,
					(unsigned)value, (unsigned)packet_offset,
					(unsigned)frame_length);
				if (length > 0)
					(void)write(trace_fd, line, (size_t)length);
			}
		}
		/* A read-only MMIO burst cannot raise a modeled interrupt, so avoid
		 * touching DraStic's ARM7 state until the guest actually writes Wi-Fi. */
		ensure_model_adapter(memory);
		dwifi_model_write16(&wifi_model, address, (uint16_t)value);
	}
}

static uint64_t load_u64(const void *address)
{
	uint64_t value;
	memcpy(&value, address, sizeof(value));
	return value;
}

static void hooked_execute_events(void *system)
{
	observed_system = system;
	uint64_t before = load_u64((uint8_t *)system + 8);
	original_execute_events(system);
	uint64_t after = load_u64((uint8_t *)system + 8);
	/* A reset or savestate load may move the absolute counter backwards during
	 * an event callback. Treat that boundary as zero elapsed instead of wrapping. */
	uint64_t elapsed = after >= before ? after - before : 0;
	event_call_count++;
	emulated_cycle_count += elapsed;
	/* ARM7 packet RAM is ordinary emulated memory and can be modified without
	 * passing through the Wi-Fi I/O dispatcher. Refresh the active beacon at a
	 * low rate so transmit-time contents include those direct guest writes. */
	if (dwifi_transport_is_active() &&
	    wifi_model.us_timestamp - last_beacon_refresh_us >= UINT64_C(10000)) {
		last_beacon_refresh_us = wifi_model.us_timestamp;
		refresh_beacon_packet(observed_memory);
	}
	(void)dwifi_cycle_clock_advance(&cycle_clock, &wifi_model, elapsed);
#if defined(__aarch64__)
	/* execute_events is the outermost emulator frame, so the one-time packet
	 * RAM search runs with a full stack rather than inside the receive
	 * callback. The guest must have populated the low half first, so this
	 * follows the beacon refresh above. */
	if (dwifi_transport_is_active())
		locate_guest_packet_ram(observed_memory);
#endif
	if (dwifi_transport_is_active() && !packet_writer_ready && original_store16) {
		dwifi_model_set_packet_write_callback(&wifi_model,
			hooked_write_packet, observed_memory);
		trace_text("drastic-wifi packet-writer=ready\n");
		packet_writer_ready = 1;
	}
	/* MP ACKs can finish in 128--176 us and are immediately followed by a
	 * command.  Polling every 256 us leaves the medium idle after those short
	 * frames and slowly accumulates an unbounded queue.  A 64 us service tick
	 * still avoids per-event locking while promptly starting the next frame. */
	if (wifi_model.us_timestamp - last_transport_poll_us >= UINT64_C(64)) {
		last_transport_poll_us = wifi_model.us_timestamp;
		dwifi_transport_poll(&wifi_model);
	}
}

#if defined(__arm__)
static void *make_trampoline(uintptr_t target, const uint32_t *expected)
{
	uint32_t *code = mmap(NULL, (size_t)sysconf(_SC_PAGESIZE),
		PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (code == MAP_FAILED)
		return NULL;

	memcpy(code, expected, sizeof(uint32_t) * DWIFI_ARM_PATCH_WORDS);
	dwifi_make_absolute_jump(&code[DWIFI_ARM_PATCH_WORDS],
		target + sizeof(uint32_t) * DWIFI_ARM_PATCH_WORDS);
	__builtin___clear_cache((char *)code,
		(char *)(code + DWIFI_ARM_TRAMPOLINE_WORDS));
	return code;
}

static int install_hook(uintptr_t target, const uint32_t *expected,
	uintptr_t replacement, void **trampoline)
{
	long page_size = sysconf(_SC_PAGESIZE);
	uintptr_t page = target & ~((uintptr_t)page_size - 1);
	uint32_t patch[DWIFI_ARM_PATCH_WORDS];

	if (!dwifi_words_match((const void *)target, expected,
		DWIFI_ARM_PATCH_WORDS))
		return -1;
	*trampoline = make_trampoline(target, expected);
	if (!*trampoline)
		return -1;
	if (mprotect((void *)page, (size_t)page_size,
		PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
		return -1;

	dwifi_make_absolute_jump(patch, replacement);
	memcpy((void *)target, patch, sizeof(patch));
	__sync_synchronize();
	__builtin___clear_cache((char *)target, (char *)(target + sizeof(patch)));
	return 0;
}
#elif defined(__aarch64__)
static void *make_trampoline(uintptr_t target, const uint32_t *expected)
{
	uint32_t *code = mmap(NULL, (size_t)sysconf(_SC_PAGESIZE),
		PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (code == MAP_FAILED)
		return NULL;

	memcpy(code, expected, sizeof(uint32_t) * DWIFI_A64_PATCH_WORDS);
	dwifi_make_aarch64_jump(&code[DWIFI_A64_PATCH_WORDS],
		target + sizeof(uint32_t) * DWIFI_A64_PATCH_WORDS);
	__builtin___clear_cache((char *)code,
		(char *)(code + DWIFI_A64_TRAMPOLINE_WORDS));
	return code;
}

static int install_hook(uintptr_t target, const uint32_t *expected,
	uintptr_t replacement, void **trampoline)
{
	long page_size = sysconf(_SC_PAGESIZE);
	uintptr_t page = target & ~((uintptr_t)page_size - 1);
	uint32_t patch[DWIFI_A64_PATCH_WORDS];

	trace_addresses("validate", target, replacement);
	if (!dwifi_words_match((const void *)target, expected,
		DWIFI_A64_PATCH_WORDS))
		return -1;
	trace_addresses("validated", target, 0);
	*trampoline = make_trampoline(target, expected);
	if (!*trampoline)
		return -1;
	trace_addresses("trampoline", target, (uintptr_t)*trampoline);
	if (mprotect((void *)page, (size_t)page_size,
		PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
		return -1;
	trace_addresses("writable", page, (uintptr_t)page_size);

	dwifi_make_aarch64_jump(patch, replacement);
	memcpy((void *)target, patch, sizeof(patch));
	__sync_synchronize();
	__builtin___clear_cache((char *)target, (char *)(target + sizeof(patch)));
	trace_addresses("patched", target, replacement);
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

static uintptr_t deferred_load_address;
static uintptr_t deferred_store_address;
static uintptr_t deferred_event_address;

static void *deferred_hook_worker(void *unused)
{
	(void)unused;
	/* Advanced DraStic installs its own executable detours during early main.
	 * Patching these prologues in our constructor makes that initialization
	 * crash before either Wi-Fi handler is called. Wi-Fi MMIO is not used until
	 * well after ROM startup, so let the frontend finish before touching it. */
	sleep(3);
	trace_text("drastic-wifi stage=deferred-install-begin\n");
	if (install_hook(deferred_load_address, load16_prologue,
		(uintptr_t)hooked_load16, (void **)&original_load16) != 0) {
		trace_text("drastic-wifi error=deferred-load16-install\n");
		return NULL;
	}
	if (install_hook(deferred_store_address, store16_prologue,
		(uintptr_t)hooked_store16, (void **)&original_store16) != 0) {
		trace_text("drastic-wifi error=deferred-store16-install\n");
		return NULL;
	}
	if (cycle_enabled && install_hook(deferred_event_address,
		execute_events_prologue, (uintptr_t)hooked_execute_events,
		(void **)&original_execute_events) != 0) {
		trace_text("drastic-wifi error=deferred-event-cycle-install\n");
		return NULL;
	}
	trace_text("drastic-wifi hooks=load16,store16 status=ready-deferred\n");
	if (cycle_enabled)
		trace_text("drastic-wifi event-cycle-hook=ready hz=67027964\n");
	return NULL;
}

static void start_deferred_hooks(void)
{
	pthread_t thread;
	if (pthread_create(&thread, NULL, deferred_hook_worker, NULL) == 0)
		(void)pthread_detach(thread);
	else
		trace_text("drastic-wifi error=deferred-thread\n");
}
#endif

static int running_supported_command(void)
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
static void dwifi_initialize(void)
{
	if (!running_supported_command())
		return;
	const char *requested_trace_path = getenv("DRASTIC_WIFI_TRACE");
	const char *mode = getenv("DRASTIC_WIFI_MODE");
	const char *cycle = getenv("DRASTIC_WIFI_CYCLE");
	const char *irq = getenv("DRASTIC_WIFI_IRQ");
	const char *session = getenv("DRASTIC_WIFI_SESSION");
	const char *verbose = getenv("DRASTIC_WIFI_VERBOSE");
	if (!requested_trace_path || !*requested_trace_path)
		return;
	char trace_path[4096];
	size_t trace_length = strlen(requested_trace_path);
	if (trace_length >= sizeof(trace_path))
		return;
	memcpy(trace_path, requested_trace_path, trace_length + 1);
	char session_path[4096] = {0};
	if (session && strlen(session) < sizeof(session_path))
		memcpy(session_path, session, strlen(session) + 1);
	model_enabled = mode && strcmp(mode, "model") == 0;
	/* Allow cycle-only diagnostics to validate the emulator boundary without
	 * enabling model writes or IRQ injection. Packaged model mode still opts in
	 * through both marker files. */
	cycle_enabled = cycle && strcmp(cycle, "1") == 0;
	irq_enabled = model_enabled && irq && strcmp(irq, "1") == 0;
	/* The packaged library is currently diagnostic-only. Preserve the bounded
	 * MMIO trace so RX cursor consumption can be verified on the Brick without
	 * modifying its launcher between iterations. */
	verbose_trace = !verbose || strcmp(verbose, "0") != 0;
	/* DraStic invokes helper processes from its UI. They must not inherit a
	 * preload library that patches fixed addresses or reopens this trace. */
	unsetenv("LD_PRELOAD");
	unsetenv("DRASTIC_WIFI_TRACE");
	unsetenv("DRASTIC_WIFI_MODE");
	unsetenv("DRASTIC_WIFI_CYCLE");
	unsetenv("DRASTIC_WIFI_IRQ");
	unsetenv("DRASTIC_WIFI_SESSION");
	unsetenv("DRASTIC_WIFI_VERBOSE");
	if (model_enabled)
		dwifi_model_reset(&wifi_model);

	trace_fd = open(trace_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (trace_fd < 0)
		return;
	if (model_enabled)
		dwifi_model_set_receive_trace_callback(&wifi_model,
			hooked_trace_receive, &wifi_model);
	if (model_enabled && cycle_enabled && session_path[0] &&
	    dwifi_transport_start(&wifi_model, session_path))
		trace_text("drastic-wifi transport=netlink status=started\n");
	else if (session_path[0])
		trace_text("drastic-wifi transport=netlink status=disabled\n");

#if defined(__arm__)
	uintptr_t load_address = DWIFI_A30_LOAD16_ADDRESS;
	uintptr_t store_address = DWIFI_A30_STORE16_ADDRESS;
	trace_text(model_enabled ?
		"drastic-wifi trace_version=3 platform=a30 binary=2.5.0.4 mode=hybrid16\n" :
		"drastic-wifi trace_version=3 platform=a30 binary=2.5.0.4 mode=passthrough16\n");
#else
	uintptr_t image_base = find_main_image();
	uintptr_t load_address = image_base + DWIFI_BRICK_LOAD16_OFFSET;
	uintptr_t store_address = image_base + DWIFI_BRICK_STORE16_OFFSET;
	uintptr_t event_address = image_base + DWIFI_BRICK_EXECUTE_EVENTS_OFFSET;
	trace_text(model_enabled ?
		"drastic-wifi trace_version=3 platform=brick binary=2.5.2.0-59a7 mode=hybrid16\n" :
		"drastic-wifi trace_version=3 platform=brick binary=2.5.2.0-59a7 mode=passthrough16\n");
	trace_addresses("image", image_base, load_address);
	if (!image_base) {
		trace_text("drastic-wifi error=main-image-not-found\n");
		return;
	}
#endif

	if (!dwifi_words_match((const void *)load_address, load16_prologue,
		DWIFI_PATCH_WORDS) ||
	    !dwifi_words_match((const void *)store_address, store16_prologue,
		DWIFI_PATCH_WORDS)) {
		trace_text("drastic-wifi error=mmio-prologue-validation\n");
		return;
	}
#if defined(__aarch64__)
	if (cycle_enabled &&
	    !dwifi_words_match((const void *)event_address,
		execute_events_prologue, DWIFI_A64_PATCH_WORDS)) {
		trace_text("drastic-wifi error=event-cycle-prologue-validation\n");
		return;
	}
#endif
#if defined(__arm__)
	if (cycle_enabled &&
	    !dwifi_words_match((const void *)DWIFI_A30_EXECUTE_EVENTS_ADDRESS,
		execute_events_prologue, DWIFI_ARM_PATCH_WORDS)) {
		trace_text("drastic-wifi error=event-cycle-prologue-validation\n");
		return;
	}
#endif

#if defined(__arm__)
	if (install_hook(load_address, load16_prologue,
		(uintptr_t)hooked_load16, (void **)&original_load16) != 0) {
		trace_text("drastic-wifi error=load16-prologue-or-install\n");
		return;
	}
	if (install_hook(store_address, store16_prologue,
		(uintptr_t)hooked_store16, (void **)&original_store16) != 0) {
		trace_text("drastic-wifi error=store16-prologue-or-install\n");
		return;
	}
	if (cycle_enabled) {
		if (install_hook(DWIFI_A30_EXECUTE_EVENTS_ADDRESS,
			execute_events_prologue, (uintptr_t)hooked_execute_events,
			(void **)&original_execute_events) != 0) {
			trace_text("drastic-wifi error=event-cycle-prologue-or-install\n");
			return;
		}
		trace_text("drastic-wifi event-cycle-hook=ready hz=67027964\n");
	} else if (model_enabled) {
		trace_text("drastic-wifi event-cycle-hook=disabled\n");
	}
#else
	deferred_load_address = load_address;
	deferred_store_address = store_address;
	deferred_event_address = event_address;
	trace_text("drastic-wifi hooks=load16,store16 status=deferred\n");
	if (model_enabled && !cycle_enabled)
		trace_text("drastic-wifi event-cycle-hook=disabled\n");
#endif
	trace_text("drastic-wifi constructor=ready\n");
}

__attribute__((destructor))
static void dwifi_shutdown(void)
{
	char line[256];
	if (trace_fd < 0)
		return;
	dwifi_transport_stop();
	int length = snprintf(line, sizeof(line),
		"drastic-wifi summary loads=%llu stores=%llu dropped=%llu "
		"model_reads=%llu model_writes=%llu event_calls=%llu "
		"arm9_cycles=%llu wifi_us=%llu irq_injections=%llu "
		"packet_syncs=%llu tx_frames=%llu rx_frames=%llu errno=%d\n",
		(unsigned long long)load_count,
		(unsigned long long)store_count,
		(unsigned long long)dropped_count,
		(unsigned long long)wifi_model.read_count,
		(unsigned long long)wifi_model.write_count,
		(unsigned long long)event_call_count,
		(unsigned long long)emulated_cycle_count,
		(unsigned long long)wifi_model.us_timestamp,
		(unsigned long long)irq_injection_count,
		(unsigned long long)packet_sync_count,
		(unsigned long long)wifi_model.tx_frame_count,
		(unsigned long long)wifi_model.rx_frame_count, errno);
	if (length > 0)
		(void)write(trace_fd, line, (size_t)length);
	close(trace_fd);
	trace_fd = -1;
}

#endif
