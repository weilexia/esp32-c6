#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lcd_st7789_init(void);
esp_lcd_panel_handle_t lcd_st7789_get_panel(void);
esp_err_t lcd_st7789_set_backlight(uint8_t percent);
uint8_t lcd_st7789_get_backlight(void);
esp_err_t lcd_st7789_draw_bitmap(int x_start, int y_start, int x_end, int y_end, const void *color_data);

#ifdef __cplusplus
}
#endif
