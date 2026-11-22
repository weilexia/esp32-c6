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

// Theme Palette (OLED watchOS / Usage Deck Monitoring Dashboard)
#define COL_BG          gfx_rgb(0, 0, 0)        // Pure OLED black
#define COL_CARD        gfx_rgb(13, 18, 26)     // Elevated dark deck card
#define COL_CARD_ALT    gfx_rgb(24, 32, 46)     // Interactive pill / secondary surface
#define COL_BORDER      gfx_rgb(28, 39, 54)     // Crisp 1px card outline
#define COL_RING_TRACK  gfx_rgb(24, 34, 44)     // Inactive circular gauge track
#define COL_WHITE       gfx_rgb(248, 250, 252)
#define COL_MUTED       gfx_rgb(115, 145, 178)  // Cool steel-blue label
#define COL_CYAN        gfx_rgb(56, 189, 248)
#define COL_BLUE        gfx_rgb(59, 130, 246)
#define COL_NEON_GREEN  gfx_rgb(74, 222, 128)   // Usage Deck neon green
#define COL_GREEN       gfx_rgb(74, 222, 128)   // Alias for consistent neon green
#define COL_AMBER       gfx_rgb(245, 158, 11)   // Warning / error amber-orange
#define COL_ORANGE      gfx_rgb(251, 146, 60)   // Bright error orange
#define COL_RED         gfx_rgb(244, 63, 94)    // Rose alert red
#define COL_RED_DARK    gfx_rgb(64, 16, 28)     // Subtle dark red tint
#define COL_PURPLE      gfx_rgb(168, 85, 247)

void gfx_fill_screen(uint16_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint16_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint16_t color);
void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint16_t color);
void gfx_draw_round_rect(int x, int y, int w, int h, int r, uint16_t color);
void gfx_fill_circle(int cx, int cy, int r, uint16_t color);
void gfx_draw_hline(int x, int y, int w, uint16_t color);

// Built-in 5x7 ASCII font rendered with integer scale (1..4)
void gfx_draw_text(int x, int y, const char *str, uint16_t fg, uint16_t bg, int scale);
void gfx_draw_text_centered(int cx, int y, const char *str, uint16_t fg, uint16_t bg, int scale);

// HD 4-bit anti-aliased 36px big digits (0-9, ':', '-', '+', '%', '.', 'C')
void gfx_draw_big_num(int x, int y, const char *str, uint16_t fg, uint16_t bg);
void gfx_draw_big_num_centered(int cx, int y, const char *str, uint16_t fg, uint16_t bg);

// HD 4-bit anti-aliased 16px UTF-8 Chinese + ASCII mixed renderer (Flash-resident Microsoft YaHei UI)
int gfx_measure_utf8(const char *utf8);
void gfx_draw_utf8(int x, int y, const char *utf8, uint16_t fg, uint16_t bg);
void gfx_draw_utf8_centered(int cx, int y, const char *utf8, uint16_t fg, uint16_t bg);
void gfx_draw_utf8_wrap(int x, int y, int max_w, int max_lines, int line_h,
                        const char *utf8, uint16_t fg, uint16_t bg);

// Weather icon renderer (48x48 area centered at cx, cy)
void gfx_draw_weather_icon(int cx, int cy, int wmo_code, uint16_t bg);

// Anti-aliased circular progress ring (progress: 0.0f .. 1.0f, clockwise from 12 o'clock)
void gfx_draw_ring_arc(int cx, int cy, int r_outer, int r_inner, float progress,
                       uint16_t fg_col, uint16_t track_col, uint16_t bg_col);

// Compact 14x11 Wi-Fi signal icon
void gfx_draw_wifi_icon(int x, int y, uint16_t color, uint16_t bg);

#ifdef __cplusplus
}
#endif
