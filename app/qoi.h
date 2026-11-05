#ifndef NETPLAY_QOI_H
#define NETPLAY_QOI_H

#include <stddef.h>
#include <stdint.h>

/* Decode a QOI image (https://qoiformat.org) to 8-bit RGBA, row-major.
 * Returns a malloc'd buffer of w*h*4 bytes, or NULL if the data is not a
 * well-formed QOI file. spruce stores nearly all of its box art as QOI. */
uint8_t* QOI_decode(const uint8_t* data, size_t len, int* w, int* h);

/* Read a whole file and decode it. */
uint8_t* QOI_read(const char* path, int* w, int* h);

#endif
