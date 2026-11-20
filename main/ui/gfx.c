#include "gfx.h"
#include "board_pins.h"
#include "lcd_st7789.h"
#include <string.h>

// 240 * 8 pixels = 3840 bytes static DMA-friendly scratch buffer
#define GFX_TILE_LINES 8
static uint16_t s_tile_buf[BOARD_LCD_WIDTH * GFX_TILE_LINES];

// Compact 5x7 ASCII font (0x20..0x7E)
static const uint8_t s_font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00}, // '!'
    {0x00,0x07,0x00,0x07,0x00}, // '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // '$'
    {0x23,0x13,0x08,0x64,0x62}, // '%'
    {0x36,0x49,0x55,0x22,0x50}, // '&'
    {0x00,0x05,0x03,0x00,0x00}, // '''
    {0x00,0x1C,0x22,0x41,0x00}, // '('
    {0x00,0x41,0x22,0x1C,0x00}, // ')'
    {0x14,0x08,0x3E,0x08,0x14}, // '*'
    {0x08,0x08,0x3E,0x08,0x08}, // '+'
    {0x00,0x50,0x30,0x00,0x00}, // ','
    {0x08,0x08,0x08,0x08,0x08}, // '-'
    {0x00,0x60,0x60,0x00,0x00}, // '.'
    {0x20,0x10,0x08,0x04,0x02}, // '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // '0'
    {0x00,0x42,0x7F,0x40,0x00}, // '1'
    {0x42,0x61,0x51,0x49,0x46}, // '2'
    {0x21,0x41,0x45,0x4B,0x31}, // '3'
    {0x18,0x14,0x12,0x7F,0x10}, // '4'
    {0x27,0x45,0x45,0x45,0x39}, // '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // '6'
    {0x01,0x71,0x09,0x05,0x03}, // '7'
    {0x36,0x49,0x49,0x49,0x36}, // '8'
    {0x06,0x49,0x49,0x29,0x1E}, // '9'
    {0x00,0x36,0x36,0x00,0x00}, // ':'
    {0x00,0x56,0x36,0x00,0x00}, // ';'
    {0x08,0x14,0x22,0x41,0x00}, // '<'
    {0x14,0x14,0x14,0x14,0x14}, // '='
    {0x00,0x41,0x22,0x14,0x08}, // '>'
    {0x02,0x01,0x51,0x09,0x06}, // '?'
    {0x32,0x49,0x79,0x41,0x3E}, // '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 'L'
    {0x7F,0x02,0x0C,0x02,0x7F}, // 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 'Z'
    {0x00,0x7F,0x41,0x41,0x00}, // '['
    {0x02,0x04,0x08,0x10,0x20}, // '\'
    {0x00,0x41,0x41,0x7F,0x00}, // ']'
    {0x04,0x02,0x01,0x02,0x04}, // '^'
    {0x40,0x40,0x40,0x40,0x40}, // '_'
    {0x00,0x01,0x02,0x04,0x00}, // '`'
    {0x20,0x54,0x54,0x54,0x78}, // 'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 'b'
    {0x38,0x44,0x44,0x44,0x20}, // 'c'
    {0x38,0x44,0x44,0x48,0x7F}, // 'd'
    {0x38,0x54,0x54,0x54,0x18}, // 'e'
    {0x08,0x7E,0x09,0x01,0x02}, // 'f'
    {0x0C,0x52,0x52,0x52,0x3E}, // 'g'
    {0x7F,0x08,0x04,0x04,0x78}, // 'h'
    {0x00,0x44,0x7D,0x40,0x00}, // 'i'
    {0x20,0x40,0x44,0x3D,0x00}, // 'j'
    {0x7F,0x10,0x28,0x44,0x00}, // 'k'
    {0x00,0x41,0x7F,0x40,0x00}, // 'l'
    {0x7C,0x04,0x18,0x04,0x78}, // 'm'
    {0x7C,0x08,0x04,0x04,0x78}, // 'n'
    {0x38,0x44,0x44,0x44,0x38}, // 'o'
    {0x7C,0x14,0x14,0x14,0x08}, // 'p'
    {0x08,0x14,0x14,0x18,0x7C}, // 'q'
    {0x7C,0x08,0x04,0x04,0x08}, // 'r'
    {0x48,0x54,0x54,0x54,0x20}, // 's'
    {0x04,0x3F,0x44,0x40,0x20}, // 't'
    {0x3C,0x40,0x40,0x20,0x7C}, // 'u'
    {0x1C,0x20,0x40,0x20,0x1C}, // 'v'
    {0x3C,0x40,0x30,0x40,0x3C}, // 'w'
    {0x44,0x28,0x10,0x28,0x44}, // 'x'
    {0x0C,0x50,0x50,0x50,0x3C}, // 'y'
    {0x44,0x64,0x54,0x4C,0x44}, // 'z'
    {0x00,0x08,0x36,0x41,0x00}, // '{'
    {0x00,0x00,0x7F,0x00,0x00}, // '|'
    {0x00,0x41,0x36,0x08,0x00}, // '}'
    {0x10,0x08,0x08,0x10,0x08}, // '~'
};

void gfx_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > BOARD_LCD_WIDTH) w = BOARD_LCD_WIDTH - x;
    if (y + h > BOARD_LCD_HEIGHT) h = BOARD_LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    int max_lines = (BOARD_LCD_WIDTH * GFX_TILE_LINES) / w;
    if (max_lines < 1) max_lines = 1;

    int fill_px = w * (h < max_lines ? h : max_lines);
    for (int i = 0; i < fill_px; i++) {
        s_tile_buf[i] = color;
    }

    int cy = y;
    int rem = h;
    while (rem > 0) {
        int lines = rem < max_lines ? rem : max_lines;
        lcd_st7789_draw_bitmap(x, cy, x + w, cy + lines, s_tile_buf);
        cy += lines;
        rem -= lines;
    }
}

void gfx_fill_screen(uint16_t color)
{
    gfx_fill_rect(0, 0, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, color);
}

void gfx_draw_hline(int x, int y, int w, uint16_t color)
{
    gfx_fill_rect(x, y, w, 1, color);
}

void gfx_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    gfx_fill_rect(x, y, w, 1, color);
    gfx_fill_rect(x, y + h - 1, w, 1, color);
    gfx_fill_rect(x, y, 1, h, color);
    gfx_fill_rect(x + w - 1, y, 1, h, color);
}

void gfx_fill_circle(int cx, int cy, int r, uint16_t color)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r) {
            dx++;
        }
        gfx_fill_rect(cx - dx, cy + dy, dx * 2 + 1, 1, color);
    }
}

void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint16_t color)
{
    if (r <= 0) {
        gfx_fill_rect(x, y, w, h, color);
        return;
    }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    // Middle body
    gfx_fill_rect(x, y + r, w, h - 2 * r, color);
    // Top & bottom rounded strips
    for (int dy = 0; dy < r; dy++) {
        int cy = r - 1 - dy;
        int dx = 0;
        while ((dx + 1) * (dx + 1) + cy * cy <= r * r) {
            dx++;
        }
        int inset = r - dx;
        gfx_fill_rect(x + inset, y + dy, w - 2 * inset, 1, color);
        gfx_fill_rect(x + inset, y + h - 1 - dy, w - 2 * inset, 1, color);
    }
}

static void draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    if (c < 0x20 || c > 0x7E) c = '?';

    int gw = 6 * scale;
    int gh = 8 * scale;
    if (x < 0 || y < 0 || x + gw > BOARD_LCD_WIDTH || y + gh > BOARD_LCD_HEIGHT) {
        return;
    }

    const uint8_t *glyph = s_font5x7[c - 0x20];
    for (int row = 0; row < 8; row++) {
        for (int sy = 0; sy < scale; sy++) {
            int out_y = row * scale + sy;
            uint16_t *line = &s_tile_buf[out_y * gw];
            for (int col = 0; col < 6; col++) {
                bool on = (col < 5 && row < 7) ? ((glyph[col] & (1 << row)) != 0) : false;
                uint16_t px = on ? fg : bg;
                for (int sx = 0; sx < scale; sx++) {
                    line[col * scale + sx] = px;
                }
            }
        }
    }
    lcd_st7789_draw_bitmap(x, y, x + gw, y + gh, s_tile_buf);
}

void gfx_draw_text(int x, int y, const char *str, uint16_t fg, uint16_t bg, int scale)
{
    if (!str) return;
    int cx = x;
    int adv = 6 * scale;
    while (*str) {
        if (cx + adv > BOARD_LCD_WIDTH) break;
        draw_char(cx, y, *str++, fg, bg, scale);
        cx += adv;
    }
}

void gfx_draw_text_centered(int cx, int y, const char *str, uint16_t fg, uint16_t bg, int scale)
{
    if (!str) return;
    int len = (int)strlen(str);
    int total_w = len * 6 * scale;
    int x = cx - total_w / 2;
    if (x < 0) x = 0;
    gfx_draw_text(x, y, str, fg, bg, scale);
}

void gfx_draw_weather_icon(int cx, int cy, int wmo_code, uint16_t bg)
{
    gfx_fill_rect(cx - 24, cy - 24, 48, 48, bg);

    if (wmo_code == 0) {
        // Sunny
        gfx_fill_circle(cx, cy, 12, COL_AMBER);
        gfx_fill_rect(cx - 1, cy - 20, 3, 5, COL_AMBER);
        gfx_fill_rect(cx - 1, cy + 15, 3, 5, COL_AMBER);
        gfx_fill_rect(cx - 20, cy - 1, 5, 3, COL_AMBER);
        gfx_fill_rect(cx + 15, cy - 1, 5, 3, COL_AMBER);
    } else if (wmo_code <= 2) {
        // Partly Cloudy (Sun + Cloud)
        gfx_fill_circle(cx - 8, cy - 8, 9, COL_AMBER);
        gfx_fill_circle(cx - 4, cy + 5, 10, COL_WHITE);
        gfx_fill_circle(cx + 8, cy + 7, 8, COL_WHITE);
        gfx_fill_rect(cx - 12, cy + 7, 24, 8, COL_WHITE);
    } else if (wmo_code == 3 || wmo_code == 45 || wmo_code == 48) {
        // Overcast / Fog
        gfx_fill_circle(cx - 7, cy + 1, 10, COL_MUTED);
        gfx_fill_circle(cx + 6, cy + 3, 9, COL_MUTED);
        gfx_fill_rect(cx - 14, cy + 3, 28, 9, COL_MUTED);
    } else if (wmo_code >= 71 && wmo_code <= 77) {
        // Snow
        gfx_fill_circle(cx - 6, cy - 4, 9, COL_WHITE);
        gfx_fill_circle(cx + 6, cy - 2, 8, COL_WHITE);
        gfx_fill_rect(cx - 12, cy - 2, 24, 8, COL_WHITE);
        gfx_fill_circle(cx - 8, cy + 13, 2, COL_CYAN);
        gfx_fill_circle(cx, cy + 16, 2, COL_CYAN);
        gfx_fill_circle(cx + 8, cy + 13, 2, COL_CYAN);
    } else if (wmo_code >= 95) {
        // Thunderstorm
        gfx_fill_circle(cx - 6, cy - 5, 9, COL_MUTED);
        gfx_fill_circle(cx + 6, cy - 3, 8, COL_MUTED);
        gfx_fill_rect(cx - 12, cy - 3, 24, 8, COL_MUTED);
        gfx_fill_rect(cx - 1, cy + 6, 4, 7, COL_AMBER);
        gfx_fill_rect(cx - 4, cy + 11, 5, 6, COL_AMBER);
    } else {
        // Rain / Drizzle / Showers
        gfx_fill_circle(cx - 6, cy - 5, 9, COL_MUTED);
        gfx_fill_circle(cx + 6, cy - 3, 8, COL_MUTED);
        gfx_fill_rect(cx - 12, cy - 3, 24, 8, COL_MUTED);
        gfx_fill_rect(cx - 8, cy + 9, 2, 8, COL_CYAN);
        gfx_fill_rect(cx, cy + 11, 2, 8, COL_CYAN);
        gfx_fill_rect(cx + 8, cy + 9, 2, 8, COL_CYAN);
    }
}
