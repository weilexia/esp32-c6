#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_CN16_W      16
#define FONT_CN16_H      16
#define FONT_CN16_BPR    8
#define FONT_CN16_BYTES  128
#define FONT_CN16_COUNT  6793

extern const uint16_t g_font_cn16_codes[FONT_CN16_COUNT];
extern const uint8_t g_font_cn16_bitmaps[FONT_CN16_COUNT][FONT_CN16_BYTES];

// Returns pointer to 128-byte (16x16 4-bit anti-aliased, 2px/byte) glyph in Flash, or NULL.
const uint8_t *font_cn16_get_bitmap(uint16_t unicode);

#ifdef __cplusplus
}
#endif