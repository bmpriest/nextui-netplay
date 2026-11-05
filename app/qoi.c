#include "qoi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QOI_OP_INDEX 0x00 /* 00xxxxxx */
#define QOI_OP_DIFF  0x40 /* 01xxxxxx */
#define QOI_OP_LUMA  0x80 /* 10xxxxxx */
#define QOI_OP_RUN   0xc0 /* 11xxxxxx */
#define QOI_OP_RGB   0xfe
#define QOI_OP_RGBA  0xff
#define QOI_MASK_2   0xc0

#define QOI_HEADER  14
#define QOI_PADDING 8   /* seven 0x00 bytes, then 0x01 */
/* Box art is a few hundred pixels a side; anything near this is not art. */
#define QOI_MAX_PIXELS (4096u * 4096u)

uint8_t* QOI_decode(const uint8_t* data, size_t len, int* w, int* h) {
	if (!data || len < QOI_HEADER + QOI_PADDING || memcmp(data, "qoif", 4)) return NULL;
	uint32_t width = (uint32_t)data[4] << 24 | data[5] << 16 | data[6] << 8 | data[7];
	uint32_t height = (uint32_t)data[8] << 24 | data[9] << 16 | data[10] << 8 | data[11];
	uint8_t channels = data[12];
	if (!width || !height || (channels != 3 && channels != 4) ||
	    height > QOI_MAX_PIXELS / width) return NULL;

	size_t pixels = (size_t)width * height;
	uint8_t* out = malloc(pixels * 4);
	if (!out) return NULL;

	uint8_t index[64][4];
	memset(index, 0, sizeof(index));
	uint8_t px[4] = {0, 0, 0, 255};
	size_t p = QOI_HEADER, end = len - QOI_PADDING;
	int run = 0;

	for (size_t i = 0; i < pixels; i++) {
		if (run > 0) {
			run--;
		} else if (p < end) {
			uint8_t b1 = data[p++];
			if (b1 == QOI_OP_RGB) {
				if (p + 3 > end) break;
				px[0] = data[p++]; px[1] = data[p++]; px[2] = data[p++];
			} else if (b1 == QOI_OP_RGBA) {
				if (p + 4 > end) break;
				px[0] = data[p++]; px[1] = data[p++]; px[2] = data[p++]; px[3] = data[p++];
			} else if ((b1 & QOI_MASK_2) == QOI_OP_INDEX) {
				memcpy(px, index[b1], 4);
			} else if ((b1 & QOI_MASK_2) == QOI_OP_DIFF) {
				px[0] += ((b1 >> 4) & 0x03) - 2;
				px[1] += ((b1 >> 2) & 0x03) - 2;
				px[2] += (b1 & 0x03) - 2;
			} else if ((b1 & QOI_MASK_2) == QOI_OP_LUMA) {
				if (p + 1 > end) break;
				uint8_t b2 = data[p++];
				int vg = (b1 & 0x3f) - 32;
				px[0] += vg - 8 + ((b2 >> 4) & 0x0f);
				px[1] += vg;
				px[2] += vg - 8 + (b2 & 0x0f);
			} else { /* QOI_OP_RUN */
				run = b1 & 0x3f;
			}
			memcpy(index[(px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64], px, 4);
		} else {
			break;   /* ran out of data before every pixel was produced */
		}
		memcpy(out + i * 4, px, 4);
		if (i + 1 == pixels) {
			*w = (int)width;
			*h = (int)height;
			return out;
		}
	}
	free(out);
	return NULL;
}

uint8_t* QOI_read(const char* path, int* w, int* h) {
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	uint8_t* data = NULL;
	long len = -1;
	if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
	if (len > 0 && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)len)) &&
	    fread(data, 1, (size_t)len, f) != (size_t)len) {
		free(data);
		data = NULL;
	}
	fclose(f);
	uint8_t* out = data ? QOI_decode(data, (size_t)len, w, h) : NULL;
	free(data);
	return out;
}
