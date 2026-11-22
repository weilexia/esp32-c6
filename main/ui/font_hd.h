#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_NUM36_W      22
#define FONT_NUM36_H      36
#define FONT_NUM36_BPR    11
#define FONT_NUM36_BYTES  (11 * 36)
#define FONT_NUM36_COUNT  16

#define FONT_ASCII16_W     10
#define FONT_ASCII16_H     16
#define FONT_ASCII16_BPR   5
#define FONT_ASCII16_BYTES (5 * 16)

extern const uint8_t g_font_ascii16_adv[95];
extern const uint8_t g_font_ascii16_bitmaps[95][FONT_ASCII16_BYTES];

const uint8_t *font_num36_get_bitmap(char c);

#ifdef __cplusplus
}
#endif