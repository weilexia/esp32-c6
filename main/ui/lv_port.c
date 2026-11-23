#include "lv_port.h"
#include "board_pins.h"
#include "font_cn16.h"
#include "lcd_st7789.h"
#include "touch_cst816d.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "lv_port";

// Double DMA buffers (2 x 240 x 14 x 2 = 13,440 B): CPU renders next strip while SPI DMA flushes previous strip!
#define LV_BUF_LINES 14
static DMA_ATTR uint8_t s_draw_buf1[BOARD_LCD_WIDTH * LV_BUF_LINES * 2];
static DMA_ATTR uint8_t s_draw_buf2[BOARD_LCD_WIDTH * LV_BUF_LINES * 2];

static bool (*s_wake_cb)(void) = NULL;
static bool s_ignore_until_release = false;
static int16_t s_last_x = 0;
static int16_t s_last_y = 0;

static uint32_t lv_tick_get_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void IRAM_ATTR lcd_flush_done_isr(void *ctx)
{
    lv_display_t *disp = (lv_display_t *)ctx;
    if (disp) {
        lv_display_flush_ready(disp);
    }
}

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    // ST7789 SPI expects big-endian RGB565
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)(w * h));
    if (lcd_st7789_draw_bitmap_async(area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map) != ESP_OK) {
        lv_display_flush_ready(disp);
    }
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    touch_point_t pt = {0};
    touch_cst816d_read(&pt);

    if (pt.pressed) {
        s_last_x = (int16_t)pt.x;
        s_last_y = (int16_t)pt.y;
        if (s_wake_cb && s_wake_cb()) {
            // Consumed as wake-from-dim tap so it never triggers an underlying button
            s_ignore_until_release = true;
            data->point.x = s_last_x;
            data->point.y = s_last_y;
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        if (s_ignore_until_release) {
            data->point.x = s_last_x;
            data->point.y = s_last_y;
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        data->point.x = s_last_x;
        data->point.y = s_last_y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        s_ignore_until_release = false;
        // Always report last valid (x, y) on release so LVGL registers clicks accurately!
        data->point.x = s_last_x;
        data->point.y = s_last_y;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// === Custom LVGL 9.x Font Adapter for Flash-resident 6,793-glyph Microsoft YaHei UI (font_cn16.c) ===
static bool cn16_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc_out,
                               uint32_t letter, uint32_t letter_next)
{
    (void)font;
    (void)letter_next;

    // Let Montserrat 14 handle ASCII and LVGL built-in symbols (0xE000..0xF8FF) via fallback
    if (letter < 0x80 || (letter >= 0xE000 && letter <= 0xF8FF)) {
        return false;
    }
    // Skip invisible iOS ANCS control/bidi/variation characters cleanly (0-width)
    if ((letter >= 0x200B && letter <= 0x200F) ||
        (letter >= 0x202A && letter <= 0x202E) ||
        (letter >= 0x2060 && letter <= 0x206F) ||
        (letter >= 0xFE00 && letter <= 0xFE0F) ||
        letter == 0xFEFF || letter == 0xFFFC) {
        dsc_out->adv_w = 0;
        dsc_out->box_w = 0;
        dsc_out->box_h = 0;
        dsc_out->ofs_x = 0;
        dsc_out->ofs_y = 0;
        dsc_out->format = LV_FONT_GLYPH_FORMAT_A8;
        dsc_out->gid.src = NULL;
        dsc_out->is_placeholder = 0;
        return true;
    }

    if (letter > 0xFFFF) {
        return false;
    }

    const uint8_t *bm = font_cn16_get_bitmap((uint16_t)letter);
    if (!bm) {
        return false;
    }

    dsc_out->adv_w = FONT_CN16_W;
    dsc_out->box_w = FONT_CN16_W;
    dsc_out->box_h = FONT_CN16_H;
    dsc_out->ofs_x = 0;
    dsc_out->ofs_y = -2;
    dsc_out->stride = 0;
    dsc_out->format = LV_FONT_GLYPH_FORMAT_A8;
    dsc_out->gid.src = bm;
    dsc_out->is_placeholder = 0;
    return true;
}

static const void *cn16_get_glyph_bitmap(lv_font_glyph_dsc_t *g_dsc, lv_draw_buf_t *draw_buf)
{
    const uint8_t *src = (const uint8_t *)g_dsc->gid.src;
    if (!src || !draw_buf) {
        return NULL;
    }

    uint8_t *dst = draw_buf->data;
    uint32_t stride_out = lv_draw_buf_width_to_stride(FONT_CN16_W, LV_COLOR_FORMAT_A8);
    for (int row = 0; row < FONT_CN16_H; row++) {
        const uint8_t *srow = &src[row * FONT_CN16_BPR];
        uint8_t *drow = &dst[row * stride_out];
        for (int col = 0; col < FONT_CN16_W; col++) {
            uint8_t byte = srow[col >> 1];
            uint8_t a4 = (col & 1) ? (byte & 0x0F) : (byte >> 4);
            drow[col] = (uint8_t)(a4 * 17); // Expand 4-bit (0..15) to 8-bit (0..255)
        }
    }
    lv_draw_buf_flush_cache(draw_buf, NULL);
    return draw_buf;
}

const lv_font_t g_lv_font_cn16 = {
    .get_glyph_dsc = cn16_get_glyph_dsc,
    .get_glyph_bitmap = cn16_get_glyph_bitmap,
    .release_glyph = NULL,
    .line_height = 17,
    .base_line = 3,
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -2,
    .underline_thickness = 1,
    .dsc = NULL,
    .fallback = &lv_font_montserrat_14,
    .user_data = NULL,
};

void lv_port_set_wake_cb(bool (*cb)(void))
{
    s_wake_cb = cb;
}

void lv_port_init(void)
{
    lv_init();
    lv_tick_set_cb(lv_tick_get_ms);

    lv_display_t *disp = lv_display_create(BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT);
    lcd_st7789_set_flush_done_cb(lcd_flush_done_isr, disp);
    lv_display_set_flush_cb(disp, disp_flush_cb);
    lv_display_set_buffers(disp, s_draw_buf1, s_draw_buf2, sizeof(s_draw_buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    // 10ms fast touch polling so quick taps & swipes feel instant
    lv_timer_t *indev_tmr = lv_indev_get_read_timer(indev);
    if (indev_tmr) {
        lv_timer_set_period(indev_tmr, 10);
    }

    ESP_LOGI(TAG, "LVGL v%d.%d.%d port initialized (%dx%d, 2x%uB async DMA @ 60MHz)",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
             BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, (unsigned)sizeof(s_draw_buf1));
}
