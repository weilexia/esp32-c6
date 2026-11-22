#include "gfx.h"
#include "board_pins.h"
#include "esp_attr.h"
#include "font_cn16.h"
#include "font_hd.h"
#include "lcd_st7789.h"
#include <math.h>
#include <string.h>

// 240 * 20 pixels = 9600 bytes 4-byte aligned DMA-friendly scratch buffer (faster burst transfers)
#define GFX_TILE_LINES 20
static DMA_ATTR uint16_t s_tile_buf[BOARD_LCD_WIDTH * GFX_TILE_LINES];

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

void gfx_draw_round_rect(int x, int y, int w, int h, int r, uint16_t color)
{
    if (r <= 0) {
        gfx_draw_rect(x, y, w, h, color);
        return;
    }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    // Straight edges
    gfx_fill_rect(x + r, y, w - 2 * r, 1, color);
    gfx_fill_rect(x + r, y + h - 1, w - 2 * r, 1, color);
    gfx_fill_rect(x, y + r, 1, h - 2 * r, color);
    gfx_fill_rect(x + w - 1, y + r, 1, h - 2 * r, color);

    // 4 rounded corners
    for (int dy = 0; dy < r; dy++) {
        int cy = r - 1 - dy;
        int dx = 0;
        while ((dx + 1) * (dx + 1) + cy * cy <= r * r) {
            dx++;
        }
        int inset = r - dx;
        gfx_fill_rect(x + inset, y + dy, 1, 1, color);
        gfx_fill_rect(x + w - 1 - inset, y + dy, 1, 1, color);
        gfx_fill_rect(x + inset, y + h - 1 - dy, 1, 1, color);
        gfx_fill_rect(x + w - 1 - inset, y + h - 1 - dy, 1, 1, color);
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

// Blend two byte-swapped RGB565 colors using 4-bit alpha (0=bg, 15=fg)
static inline uint16_t blend_rgb565_4bit(uint16_t fg_swapped, uint16_t bg_swapped, uint8_t a)
{
    if (a == 0) return bg_swapped;
    if (a >= 15) return fg_swapped;

    uint16_t fg = (uint16_t)((fg_swapped >> 8) | (fg_swapped << 8));
    uint16_t bg = (uint16_t)((bg_swapped >> 8) | (bg_swapped << 8));

    uint32_t fr = (fg >> 11) & 0x1F, fg_g = (fg >> 5) & 0x3F, fb = fg & 0x1F;
    uint32_t br = (bg >> 11) & 0x1F, bg_g = (bg >> 5) & 0x3F, bb = bg & 0x1F;

    uint32_t ia = 15 - a;
    uint32_t r = (br * ia + fr * a + 7) / 15;
    uint32_t g = (bg_g * ia + fg_g * a + 7) / 15;
    uint32_t b = (bb * ia + fb * a + 7) / 15;

    uint16_t c = (uint16_t)((r << 11) | (g << 5) | b);
    return (uint16_t)((c >> 8) | (c << 8));
}

static void blit_4bit_glyph(int x, int y, int w, int h, int bpr,
                            const uint8_t *data, uint16_t fg, uint16_t bg)
{
    if (x < 0 || y < 0 || x + w > BOARD_LCD_WIDTH || y + h > BOARD_LCD_HEIGHT) {
        return;
    }
    if (w * h > BOARD_LCD_WIDTH * GFX_TILE_LINES) {
        return;
    }

    uint16_t lut[16];
    for (uint8_t a = 0; a < 16; a++) {
        lut[a] = blend_rgb565_4bit(fg, bg, a);
    }

    for (int row = 0; row < h; row++) {
        const uint8_t *src = &data[row * bpr];
        uint16_t *dst = &s_tile_buf[row * w];
        for (int col = 0; col < w; col++) {
            uint8_t byte = src[col >> 1];
            uint8_t alpha = (col & 1) ? (byte & 0x0F) : (byte >> 4);
            dst[col] = lut[alpha];
        }
    }
    lcd_st7789_draw_bitmap(x, y, x + w, y + h, s_tile_buf);
}

void gfx_draw_big_num(int x, int y, const char *str, uint16_t fg, uint16_t bg)
{
    if (!str) return;
    int cx = x;
    while (*str) {
        char c = *str++;
        int adv = (c == ':' || c == '.') ? 12 : (c == ' ' ? 10 : FONT_NUM36_W);
        if (cx + adv > BOARD_LCD_WIDTH) break;
        const uint8_t *bm = font_num36_get_bitmap(c);
        if (bm) {
            int ox = (c == ':' || c == '.') ? (cx - 5) : cx;
            if (ox < 0) ox = 0;
            blit_4bit_glyph(ox, y, FONT_NUM36_W, FONT_NUM36_H, FONT_NUM36_BPR, bm, fg, bg);
        } else {
            gfx_fill_rect(cx, y, adv, FONT_NUM36_H, bg);
        }
        cx += adv;
    }
}

void gfx_draw_big_num_centered(int cx, int y, const char *str, uint16_t fg, uint16_t bg)
{
    if (!str) return;
    int total_w = 0;
    for (const char *p = str; *p; p++) {
        total_w += (*p == ':' || *p == '.') ? 12 : (*p == ' ' ? 10 : FONT_NUM36_W);
    }
    int x = cx - total_w / 2;
    if (x < 0) x = 0;
    gfx_draw_big_num(x, y, str, fg, bg);
}

static void draw_cn16_glyph(int x, int y, uint16_t unicode, uint16_t fg, uint16_t bg)
{
    if (x < 0 || y < 0 || x + 16 > BOARD_LCD_WIDTH || y + 16 > BOARD_LCD_HEIGHT) {
        return;
    }
    const uint8_t *bm = font_cn16_get_bitmap(unicode);
    if (!bm) {
        for (int row = 0; row < 16; row++) {
            for (int col = 0; col < 16; col++) {
                bool edge = (row == 2 || row == 13 || col == 2 || col == 13) &&
                            (row >= 2 && row <= 13 && col >= 2 && col <= 13);
                s_tile_buf[row * 16 + col] = edge ? fg : bg;
            }
        }
        lcd_st7789_draw_bitmap(x, y, x + 16, y + 16, s_tile_buf);
    } else {
        blit_4bit_glyph(x, y, FONT_CN16_W, FONT_CN16_H, FONT_CN16_BPR, bm, fg, bg);
    }
}

static void draw_ascii16_glyph(int x, int y, char c, uint16_t fg, uint16_t bg, int adv)
{
    if (c < 0x20 || c > 0x7E) c = '?';
    int idx = c - 0x20;
    int draw_w = adv < FONT_ASCII16_W ? adv : FONT_ASCII16_W;
    if (x < 0 || y < 0 || x + draw_w > BOARD_LCD_WIDTH || y + FONT_ASCII16_H > BOARD_LCD_HEIGHT) {
        return;
    }

    uint16_t lut[16];
    for (uint8_t a = 0; a < 16; a++) {
        lut[a] = blend_rgb565_4bit(fg, bg, a);
    }
    const uint8_t *data = g_font_ascii16_bitmaps[idx];
    for (int row = 0; row < FONT_ASCII16_H; row++) {
        const uint8_t *src = &data[row * FONT_ASCII16_BPR];
        uint16_t *dst = &s_tile_buf[row * draw_w];
        for (int col = 0; col < draw_w; col++) {
            uint8_t byte = src[col >> 1];
            uint8_t alpha = (col & 1) ? (byte & 0x0F) : (byte >> 4);
            dst[col] = lut[alpha];
        }
    }
    lcd_st7789_draw_bitmap(x, y, x + draw_w, y + FONT_ASCII16_H, s_tile_buf);
}

static int decode_utf8_char(const char *s, uint32_t *out_cp)
{
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] < 0x80) {
        *out_cp = p[0];
        return 1;
    } else if ((p[0] & 0xE0) == 0xC0 && p[1]) {
        *out_cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    } else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
        *out_cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    } else if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        *out_cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                  ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *out_cp = '?';
    return 1;
}

int gfx_measure_utf8(const char *utf8)
{
    if (!utf8) return 0;
    int w = 0;
    const char *p = utf8;
    while (*p) {
        uint32_t cp = 0;
        int step = decode_utf8_char(p, &cp);
        if (cp >= 0x20 && cp <= 0x7E) {
            w += g_font_ascii16_adv[cp - 0x20];
        } else if (cp >= 0x80) {
            w += 16;
        }
        p += step;
    }
    return w;
}

void gfx_draw_utf8(int x, int y, const char *utf8, uint16_t fg, uint16_t bg)
{
    gfx_draw_utf8_wrap(x, y, BOARD_LCD_WIDTH - x, 1, 18, utf8, fg, bg);
}

void gfx_draw_utf8_centered(int cx, int y, const char *utf8, uint16_t fg, uint16_t bg)
{
    if (!utf8) return;
    int total_w = gfx_measure_utf8(utf8);
    int x = cx - total_w / 2;
    if (x < 0) x = 0;
    gfx_draw_utf8(x, y, utf8, fg, bg);
}

static bool is_invisible_cp(uint32_t cp)
{
    return (cp < 0x20) ||
           (cp >= 0x200B && cp <= 0x200F) || // ZWSP, ZWNJ, ZWJ, LRM, RLM (common in iOS ANCS!)
           (cp >= 0x202A && cp <= 0x202E) || // Bidi embedding/override
           (cp >= 0x2060 && cp <= 0x206F) || // Word joiner, bidi isolates
           (cp >= 0xFE00 && cp <= 0xFE0F) || // Variation selectors (emoji)
           (cp == 0xFEFF || cp == 0xFFFC);   // BOM, object replacement
}

static bool is_space_cp(uint32_t cp)
{
    return (cp == 0x00A0) || (cp >= 0x2000 && cp <= 0x200A) || (cp == 0x3000);
}

void gfx_draw_utf8_wrap(int x, int y, int max_w, int max_lines, int line_h,
                        const char *utf8, uint16_t fg, uint16_t bg)
{
    if (!utf8 || max_w <= 0 || max_lines <= 0) return;
    int cx = x;
    int cy = y;
    int line = 0;
    const char *p = utf8;

    while (*p && line < max_lines) {
        if (*p == '\n') {
            p++;
            cx = x;
            cy += line_h;
            line++;
            continue;
        }
        if (*p == '\r') {
            p++;
            continue;
        }

        uint32_t cp = 0;
        int step = decode_utf8_char(p, &cp);
        p += step;

        // Skip iOS invisible control/directional/variation-selector characters
        if (is_invisible_cp(cp)) {
            continue;
        }
        // Normalize non-ASCII spaces (NBSP, ideographic space) to normal ASCII space
        if (is_space_cp(cp)) {
            cp = ' ';
        }
        // Fallback full-width ASCII (e.g. ！，：（）) to half-width ASCII if not in cn16 table
        if (cp >= 0xFF01 && cp <= 0xFF5E && font_cn16_get_bitmap((uint16_t)cp) == NULL) {
            cp = cp - 0xFEE0;
        }
        // Replace Emoji (> 0xFFFF or dingbats) with a clean '*' instead of a garbled box
        if (cp > 0xFFFF || (cp >= 0x2600 && cp <= 0x27BF && font_cn16_get_bitmap((uint16_t)cp) == NULL)) {
            cp = '*';
        }

        int adv = (cp >= 0x20 && cp <= 0x7E) ? g_font_ascii16_adv[cp - 0x20] : 16;

        if (cx - x + adv > max_w) {
            cx = x;
            cy += line_h;
            line++;
            if (line >= max_lines) break;
        }

        if (cp >= 0x20 && cp <= 0x7E) {
            draw_ascii16_glyph(cx, cy, (char)cp, fg, bg, adv);
        } else {
            draw_cn16_glyph(cx, cy, (uint16_t)cp, fg, bg);
        }

        cx += adv;
    }
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

void gfx_draw_ring_arc(int cx, int cy, int r_outer, int r_inner, float progress,
                       uint16_t fg_col, uint16_t track_col, uint16_t bg_col)
{
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    int x0 = cx - r_outer - 1;
    int y0 = cy - r_outer - 1;
    int x1 = cx + r_outer + 2;
    int y1 = cy + r_outer + 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > BOARD_LCD_WIDTH) x1 = BOARD_LCD_WIDTH;
    if (y1 > BOARD_LCD_HEIGHT) y1 = BOARD_LCD_HEIGHT;

    int w = x1 - x0;
    int h = y1 - y0;
    if (w <= 0 || h <= 0) return;

    int max_lines = (BOARD_LCD_WIDTH * GFX_TILE_LINES) / w;
    if (max_lines < 1) max_lines = 1;

    uint16_t fg_lut[16], tr_lut[16];
    for (uint8_t a = 0; a < 16; a++) {
        fg_lut[a] = blend_rgb565_4bit(fg_col, bg_col, a);
        tr_lut[a] = blend_rgb565_4bit(track_col, bg_col, a);
    }

    const float two_pi = 6.2831853f;
    int cur_y = y0;
    while (cur_y < y1) {
        int lines = y1 - cur_y;
        if (lines > max_lines) lines = max_lines;

        for (int row = 0; row < lines; row++) {
            int py = cur_y + row;
            float dy = (float)(py - cy);
            uint16_t *dst = &s_tile_buf[row * w];
            for (int col = 0; col < w; col++) {
                int px = x0 + col;
                float dx = (float)(px - cx);
                float dist = sqrtf(dx * dx + dy * dy);

                if (dist <= (float)r_inner - 1.0f || dist >= (float)r_outer + 1.0f) {
                    dst[col] = bg_col;
                    continue;
                }

                float cov_out = (float)r_outer + 0.5f - dist;
                float cov_in  = dist - ((float)r_inner - 0.5f);
                if (cov_out < 0.0f) cov_out = 0.0f;
                if (cov_out > 1.0f) cov_out = 1.0f;
                if (cov_in < 0.0f)  cov_in = 0.0f;
                if (cov_in > 1.0f)  cov_in = 1.0f;
                float cov = cov_out < cov_in ? cov_out : cov_in;
                uint8_t alpha = (uint8_t)(cov * 15.0f + 0.5f);

                bool active = (progress >= 0.999f);
                if (!active && progress > 0.001f) {
                    float ang = atan2f(dx, -dy);
                    if (ang < 0.0f) ang += two_pi;
                    active = (ang / two_pi) <= progress;
                }
                dst[col] = active ? fg_lut[alpha] : tr_lut[alpha];
            }
        }
        lcd_st7789_draw_bitmap(x0, cur_y, x1, cur_y + lines, s_tile_buf);
        cur_y += lines;
    }
}

void gfx_draw_wifi_icon(int x, int y, uint16_t color, uint16_t bg)
{
    const int w = 15, h = 12;
    if (x < 0 || y < 0 || x + w > BOARD_LCD_WIDTH || y + h > BOARD_LCD_HEIGHT) return;

    const float cx = 7.0f;
    const float cy = 10.0f;
    for (int row = 0; row < h; row++) {
        uint16_t *dst = &s_tile_buf[row * w];
        for (int col = 0; col < w; col++) {
            float dx = (float)col - cx;
            float dy = (float)row - cy;
            float d = sqrtf(dx * dx + dy * dy);
            bool on = false;
            if (d <= 1.6f) {
                on = true;
            } else if (dy < -0.5f && fabsf(dx) <= (-dy) * 1.15f) {
                if ((d >= 3.6f && d <= 5.0f) || (d >= 6.8f && d <= 8.2f)) {
                    on = true;
                }
            }
            dst[col] = on ? color : bg;
        }
    }
    lcd_st7789_draw_bitmap(x, y, x + w, y + h, s_tile_buf);
}
