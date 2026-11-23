#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Custom 16px anti-aliased Chinese font (6,793 glyphs from font_cn16.c)
// with automatic fallback to lv_font_montserrat_14 for crisp ASCII & LVGL icons
extern const lv_font_t g_lv_font_cn16;

void lv_port_init(void);
void lv_port_set_wake_cb(bool (*cb)(void));

#ifdef __cplusplus
}
#endif
