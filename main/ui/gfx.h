#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Converts 8-bit RGB to byte-swapped RGB565 for ST7789 SPI big-endian transfer
static inline uint16_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));
}

// Theme Palette (Modern Dark Slate)
#define COL_BG          gfx_rgb(13, 18, 30)
#define COL_CARD        gfx_rgb(24, 33, 52)
#define COL_CARD_ALT    gfx_rgb(33, 45, 70)
#define COL_BORDER      gfx_rgb(52, 68, 98)
#define COL_WHITE       gfx_rgb(245, 248, 255)
#define COL_MUTED       gfx_rgb(148, 163, 192)
#define COL_CYAN        gfx_rgb(56, 189, 248)
#define COL_BLUE        gfx_rgb(59, 130, 246)
#define COL_GREEN       gfx_rgb(34, 197, 94)
#define COL_AMBER       gfx_rgb(251, 191, 36)
#define COL_RED         gfx_rgb(239, 68, 68)
#define COL_PURPLE      gfx_rgb(168, 85, 247)

void gfx_fill_screen(uint16_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint16_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint16_t color);
void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint16_t color);
void gfx_fill_circle(int cx, int cy, int r, uint16_t color);
void gfx_draw_hline(int x, int y, int w, uint16_t color);

// Built-in 5x7 ASCII font rendered with integer scale (1..4)
void gfx_draw_text(int x, int y, const char *str, uint16_t fg, uint16_t bg, int scale);
void gfx_draw_text_centered(int cx, int y, const char *str, uint16_t fg, uint16_t bg, int scale);

// Weather icon renderer (48x48 area centered at cx, cy)
void gfx_draw_weather_icon(int cx, int cy, int wmo_code, uint16_t bg);

#ifdef __cplusplus
}
#endif
