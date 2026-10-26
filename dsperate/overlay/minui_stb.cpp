// SPDX-License-Identifier: GPL-3.0-or-later
// Vendored by minui-nintendo-dsperate-pak. The one translation unit holding
// the stb implementations minui_gfx.cpp is built on.
//
// stb_image.h and stb_truetype.h are nothings/stb at
// 6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760, unmodified, public domain or MIT
// at the user's choice (see the end of each header), either of which GPLv3
// can absorb. This file is compiled with -w: the headers are upstream's, not
// ours to fix, and DSperate's warning set is far stricter than stb's.
//
// Only PNG and BMP are decoded. The thumbnail the launcher reads is PNG
// bytes under a .bmp name when NextUI or this pak wrote it and a real BMP
// when MinUI did; nothing else ever lands there. stb_image carries its own
// inflate, and stb_truetype needs only libm, so neither adds a library the
// ABI check in scripts/check-abi.sh would see.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
