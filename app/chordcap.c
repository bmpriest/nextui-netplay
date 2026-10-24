/*
 * Raw button capture for the quick menu's button combination. The quick menu
 * process reads evdev, not SDL, so the combination is stored as the evdev key
 * codes the buttons send; the app learns them here while the user holds the
 * buttons. Kept out of main.c because <linux/input.h> defines BTN_A and the
 * rest as macros, which collide with NextUI's own button names.
 */

#include "chordcap.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_DEVICES 12

static int fds[MAX_DEVICES];
static int fd_count;
static unsigned char held[KEY_CNT];
/* Per device: the trigger axes' midpoint, above which a trigger counts as
 * pressed, and their current values. */
static int trig_mid[MAX_DEVICES][2], trig_val[MAX_DEVICES][2];
static int hat[MAX_DEVICES][2];

#define BIT(arr, n) ((arr)[(n) / (8 * sizeof(long))] & (1UL << ((n) % (8 * sizeof(long)))))

void CHORD_open(void) {
	CHORD_close();
	for (int i = 0; i < 32 && fd_count < MAX_DEVICES; i++) {
		char path[32];
		unsigned long evbits = 0;
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) continue;
		if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0 || !(evbits & (1UL << EV_KEY))) {
			close(fd);
			continue;
		}
		int d = fd_count++;
		fds[d] = fd;
		static const int TRIG[2] = { ABS_Z, ABS_RZ };
		for (int t = 0; t < 2; t++) {
			struct input_absinfo a;
			trig_mid[d][t] = 1 << 30;   /* no such axis: never pressed */
			trig_val[d][t] = 0;
			if (ioctl(fd, EVIOCGABS(TRIG[t]), &a) == 0 && a.maximum > a.minimum) {
				trig_mid[d][t] = a.minimum + (a.maximum - a.minimum) / 2;
				trig_val[d][t] = a.value;
			}
		}
		hat[d][0] = hat[d][1] = 0;
	}
	memset(held, 0, sizeof(held));
}

void CHORD_close(void) {
	for (int i = 0; i < fd_count; i++) close(fds[i]);
	fd_count = 0;
}

void CHORD_poll(void) {
	for (int d = 0; d < fd_count; d++) {
		struct input_event ev;
		while (read(fds[d], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
			if (ev.type == EV_KEY && ev.code < KEY_CNT) {
				if (ev.value != 2) held[ev.code] = ev.value != 0;
			} else if (ev.type == EV_ABS) {
				if (ev.code == ABS_HAT0X) hat[d][0] = ev.value;
				else if (ev.code == ABS_HAT0Y) hat[d][1] = ev.value;
				else if (ev.code == ABS_Z) trig_val[d][0] = ev.value;
				else if (ev.code == ABS_RZ) trig_val[d][1] = ev.value;
			}
		}
	}
}

int CHORD_held(int* codes, int max) {
	int n = 0;
	for (int k = 0; k < KEY_CNT && n < max; k++)
		if (held[k]) codes[n++] = k;
	return n;
}

bool CHORD_axisHeld(void) {
	for (int d = 0; d < fd_count; d++) {
		if (hat[d][0] || hat[d][1]) return true;
		for (int t = 0; t < 2; t++)
			if (trig_val[d][t] > trig_mid[d][t]) return true;
	}
	return false;
}

bool CHORD_isSystemKey(int code) {
	return code == KEY_POWER || code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN ||
	       code == KEY_MENU || code == BTN_MODE;
}
