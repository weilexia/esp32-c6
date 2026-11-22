#include "app_ui.h"
#include "alert_server.h"
#include "app_config.h"
#include "axp2101.h"
#include "ble_ancs.h"
#include "board_pins.h"
#include "deepseek_service.h"
#include "gfx.h"
#include "lcd_st7789.h"
#include "touch_cst816d.h"
#include "weather_service.h"
#include "web_portal.h"
#include "wifi_time.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_ui";

typedef enum {
    PAGE_OVERVIEW = 0,  // DeepSeek Usage Deck (CC Switch style: ring + 3 stacked cards)
    PAGE_TOKENS,        // DeepSeek detailed token & balance breakdown
    PAGE_ALERTS,        // Clock, weather & system alerts
    PAGE_SYSTEM,        // Network, brightness power-save & hotspot trigger
    PAGE_DETAIL,        // Single alert detail view
} ui_page_t;

#define MAIN_PAGE_COUNT 4
#define DIM_BACKLIGHT_PCT 5

static ui_page_t s_page = PAGE_OVERVIEW;
static bool s_full_redraw = true;
static alert_record_t s_detail_rec = {0};

// Rendered state cache for zero-flicker differential partial refresh
static int s_rendered_min = -1;
static int s_rendered_bat = -999;
static bool s_rendered_wifi = false;
static bool s_rendered_ap = false;
static deepseek_usage_t s_rendered_ds = {0};
static bool s_rendered_ds_valid = false;
static uint32_t s_rendered_alert_total = 0xFFFFFFFF;
static bool s_rendered_weather_valid = false;
static int s_rendered_weather_temp = -999;
static uint8_t s_rendered_bl = 0;
static uint16_t s_rendered_dim = 0xFFFF;
static int s_poll_div = 0;

// Brightness & auto-dim power saving
static uint8_t s_brightness = 80;
static uint16_t s_auto_dim_sec = 60;
static int64_t s_last_activity_ms = 0;
static bool s_is_dimmed = false;

static bool s_prev_pressed = false;
static uint16_t s_touch_start_x = 0;
static uint16_t s_touch_start_y = 0;

static bool s_prev_boot_btn = false;
static int s_boot_press_ticks = 0;
static bool s_boot_long_fired = false;

static bool s_was_portal = false;
static bool s_portal_dismissed = false;

static bool s_was_alert = false;
static uint32_t s_last_alert_seq = 0;

static uint32_t s_last_wx_seq = 0;
static bool s_wx_showing = false;
static int s_wx_ticks_left = 0;
static ble_ancs_msg_t s_wx_msg = {0};

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void wake_backlight(void)
{
    s_last_activity_ms = now_ms();
    if (s_is_dimmed) {
        s_is_dimmed = false;
        lcd_st7789_set_backlight(s_brightness);
    }
}

static void set_brightness_and_save(uint8_t pct)
{
    if (pct < 10) pct = 10;
    if (pct > 100) pct = 100;
    s_brightness = pct;
    s_is_dimmed = false;
    s_last_activity_ms = now_ms();
    lcd_st7789_set_backlight(s_brightness);
    app_config_set_display(s_brightness, s_auto_dim_sec);
}

static void cycle_auto_dim_and_save(void)
{
    if (s_auto_dim_sec == 30) s_auto_dim_sec = 60;
    else if (s_auto_dim_sec == 60) s_auto_dim_sec = 120;
    else if (s_auto_dim_sec == 120) s_auto_dim_sec = 0;
    else s_auto_dim_sec = 30;
    s_last_activity_ms = now_ms();
    app_config_set_display(s_brightness, s_auto_dim_sec);
}

static uint16_t level_color(alert_level_t lvl)
{
    if (lvl == ALERT_LEVEL_ERROR) return COL_RED;
    if (lvl == ALERT_LEVEL_WARN)  return COL_AMBER;
    return COL_CYAN;
}

static const char *level_name(alert_level_t lvl)
{
    if (lvl == ALERT_LEVEL_ERROR) return "严重告警";
    if (lvl == ALERT_LEVEL_WARN)  return "系统预警";
    return "系统通知";
}

// === Partial-friendly Top-Right Wi-Fi Status ===
static void draw_header_wifi_status(void)
{
    bool online = wifi_time_is_connected();
    bool ap_on = web_portal_is_active();
    uint16_t st_col = ap_on ? COL_AMBER : (online ? COL_NEON_GREEN : COL_RED);
    const char *st_txt = ap_on ? "AP MODE" : (online ? "ONLINE" : "OFFLINE");

    gfx_fill_rect(160, 10, 74, 18, COL_BG);
    gfx_draw_wifi_icon(162, 12, st_col, COL_BG);
    gfx_draw_text(182, 15, st_txt, st_col, COL_BG, 1);

    s_rendered_wifi = online;
    s_rendered_ap = ap_on;
}

// === Usage Deck Header (CC Switch Style) ===
static void draw_deck_header(const char *section_title, const char *right_tag)
{
    // Left neon-green badge "DS"
    gfx_fill_round_rect(10, 8, 24, 24, 6, COL_NEON_GREEN);
    gfx_draw_text_centered(22, 16, "DS", COL_BG, COL_NEON_GREEN, 1);

    // Brand Title + Subtitle
    gfx_draw_text(40, 10, "DEEPSEEK", COL_NEON_GREEN, COL_BG, 1);
    gfx_draw_text(40, 22, "USAGE DECK", COL_MUTED, COL_BG, 1);

    draw_header_wifi_status();

    // Sub-header row
    gfx_draw_text(10, 42, section_title, COL_NEON_GREEN, COL_BG, 1);
    int rlen = (int)strlen(right_tag) * 6;
    gfx_draw_text(230 - rlen, 42, right_tag, COL_MUTED, COL_BG, 1);
}

// === Partial-friendly Bottom Time Update ===
static void draw_bottom_time_partial(const struct tm *now)
{
    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", now->tm_hour & 0xFF, now->tm_min & 0xFF);
    gfx_draw_text(58, 260, tbuf, COL_MUTED, COL_BG, 1);
    s_rendered_min = now->tm_min;
}

// === Usage Deck Bottom Bar: HOME pill + Time + 4 Dots + SET Button ===
static void draw_deck_bottom_bar(const struct tm *now, int active_dot)
{
    const int bar_y = 252;
    gfx_fill_rect(0, bar_y - 2, 240, 34, COL_BG);

    // Left: Neon-green HOME pill
    gfx_fill_round_rect(8, bar_y, 44, 24, 8, COL_NEON_GREEN);
    gfx_draw_text_centered(30, bar_y + 8, "HOME", COL_BG, COL_NEON_GREEN, 1);

    draw_bottom_time_partial(now);

    // Center: 4 pagination dots
    const int dot_x0 = 102;
    for (int i = 0; i < MAIN_PAGE_COUNT; i++) {
        uint16_t col = (i == active_dot) ? COL_NEON_GREEN : COL_BORDER;
        gfx_fill_circle(dot_x0 + i * 12, bar_y + 12, (i == active_dot) ? 3 : 2, col);
    }

    // Right: Crisp Settings / Hotspot Button ("设置", generous padding inside 62x24 pill)
    gfx_fill_round_rect(168, bar_y, 62, 24, 8, COL_CARD);
    gfx_draw_round_rect(168, bar_y, 62, 24, 8, COL_NEON_GREEN);
    gfx_draw_utf8_centered(199, bar_y + 4, "设置", COL_NEON_GREEN, COL_CARD);
}

// Draw token count cleanly: anti-aliased digits + crisp 5x7 unit suffix (avoids 10px 'M' crop)
static void draw_token_value(int x, int y, uint64_t tokens, uint16_t fg, uint16_t bg)
{
    char num[16];
    const char *unit = "";
    if (tokens >= 1000000ULL) {
        snprintf(num, sizeof(num), "%.2f", (double)tokens / 1000000.0);
        unit = "M";
    } else if (tokens >= 10000ULL) {
        snprintf(num, sizeof(num), "%.1f", (double)tokens / 1000.0);
        unit = "K";
    } else if (tokens >= 1000ULL) {
        snprintf(num, sizeof(num), "%.2f", (double)tokens / 1000.0);
        unit = "K";
    } else {
        snprintf(num, sizeof(num), "%llu", (unsigned long long)tokens);
    }
    gfx_draw_utf8(x, y, num, fg, bg);
    if (unit[0]) {
        int nw = gfx_measure_utf8(num);
        gfx_draw_text(x + nw + 2, y + 6, unit, COL_NEON_GREEN, bg, 1);
    }
}

// === Page 0: OVERVIEW — Partial Data Update (Zero Flicker!) ===
static void draw_overview_data_partial(const deepseek_usage_t *ds, int bat, bool force_ring)
{
    // 1. Circular Progress Ring + Center Text
    float ring_pct = 0.0f;
    const char *ring_label = "WAIT";
    char ring_num[12] = "--";

    if (ds->calls > 0) {
        ring_pct = 100.0f * (float)(ds->calls - ds->errors) / (float)ds->calls;
        ring_label = "SUCCESS";
        if (ring_pct >= 99.95f) snprintf(ring_num, sizeof(ring_num), "100");
        else snprintf(ring_num, sizeof(ring_num), "%.1f", ring_pct);
    } else if (ds->balance_valid) {
        float budget = ds->daily_budget > 0.01f ? ds->daily_budget : 20.0f;
        float used_ratio = ds->today_cost / budget;
        if (used_ratio < 0.0f) used_ratio = 0.0f;
        if (used_ratio > 1.0f) used_ratio = 1.0f;
        ring_pct = used_ratio * 100.0f;
        ring_label = ds->is_available ? "USED" : "EMPTY";
        if (ring_pct >= 99.95f) snprintf(ring_num, sizeof(ring_num), "100");
        else snprintf(ring_num, sizeof(ring_num), "%.1f", ring_pct);
    } else {
        ring_label = ds->status_text[0] ? ds->status_text : "WAIT";
    }

    uint16_t ring_col = COL_NEON_GREEN;
    if (ds->calls > 0 && ring_pct < 90.0f) ring_col = COL_ORANGE;
    else if (ds->balance_valid && !ds->is_available) ring_col = COL_RED;
    else if (ds->calls == 0 && ds->balance_valid && ring_pct >= 80.0f) ring_col = COL_ORANGE;

    if (force_ring) {
        const int rcx = 64, rcy = 144;
        // Clear strictly inside inner radius (r=46 < r_inner=49) so we NEVER bite into the ring!
        gfx_fill_circle(rcx, rcy, 46, COL_BG);
        gfx_draw_ring_arc(rcx, rcy, 55, 49, ring_pct / 100.0f, ring_col, COL_RING_TRACK, COL_BG);

        if (strcmp(ring_num, "--") == 0) {
            gfx_draw_big_num_centered(rcx, rcy - 26, "--", COL_MUTED, COL_BG);
        } else {
            gfx_draw_big_num_centered(rcx, rcy - 26, ring_num, COL_WHITE, COL_BG);
        }
        gfx_draw_text_centered(rcx, rcy + 13, "%", ring_col, COL_BG, 1);
        gfx_draw_text_centered(rcx, rcy + 26, ring_label, COL_MUTED, COL_BG, 1);
    }

    // 2. Right 3 Cards — Update ONLY the inner value boxes!
    const int cx = 130;

    // Card 1 Value: TOKENS
    gfx_fill_rect(cx + 6, 66, 88, 38, COL_CARD);
    gfx_draw_text(cx + 8, 68, ds->tokens_estimated ? "TOKENS (EST)" : "TOKENS", COL_MUTED, COL_CARD, 1);
    if (ds->total_tokens > 0 || ds->calls == 0) {
        draw_token_value(cx + 8, 84, ds->total_tokens, COL_WHITE, COL_CARD);
    } else {
        char call_buf[20];
        snprintf(call_buf, sizeof(call_buf), "%lu", (unsigned long)ds->calls);
        gfx_draw_utf8(cx + 8, 84, call_buf, COL_WHITE, COL_CARD);
    }

    // Card 2 Value: BALANCE (currency in label, pure ASCII number — no missing '¥' glyph!)
    gfx_fill_rect(cx + 6, 124, 88, 38, COL_CARD);
    if (ds->balance_valid) {
        char blbl[20];
        snprintf(blbl, sizeof(blbl), "BAL (%s)", ds->currency[0] ? ds->currency : "CNY");
        gfx_draw_text(cx + 8, 126, blbl, COL_MUTED, COL_CARD, 1);
        char bal_buf[20];
        snprintf(bal_buf, sizeof(bal_buf), "%.2f", ds->total_balance);
        uint16_t bcol = (ds->total_balance < 5.0f) ? COL_ORANGE : COL_CYAN;
        gfx_draw_utf8(cx + 8, 142, bal_buf, bcol, COL_CARD);
    } else {
        gfx_draw_text(cx + 8, 126, "ERRORS", COL_MUTED, COL_CARD, 1);
        char err_buf[16];
        snprintf(err_buf, sizeof(err_buf), "%lu", (unsigned long)ds->errors);
        gfx_draw_utf8(cx + 8, 142, err_buf, ds->errors > 0 ? COL_ORANGE : COL_NEON_GREEN, COL_CARD);
    }

    // Card 3 Value: TODAY COST (RMB)
    gfx_fill_rect(cx + 6, 182, 88, 38, COL_CARD);
    gfx_draw_text(cx + 8, 184, "TODAY (RMB)", COL_MUTED, COL_CARD, 1);
    char cost_buf[20];
    snprintf(cost_buf, sizeof(cost_buf), "%.2f", ds->today_cost);
    gfx_draw_utf8(cx + 8, 200, cost_buf, COL_WHITE, COL_CARD);

    // 3. Compact Meta Strip
    gfx_fill_rect(8, 234, 224, 12, COL_BG);
    char meta[48];
    if (bat >= 0) {
        snprintf(meta, sizeof(meta), "CALL:%lu  ERR:%lu  BAT %d%%",
                 (unsigned long)ds->calls, (unsigned long)ds->errors, bat);
    } else {
        snprintf(meta, sizeof(meta), "CALL:%lu  ERR:%lu  USB",
                 (unsigned long)ds->calls, (unsigned long)ds->errors);
    }
    gfx_draw_text_centered(120, 236, meta, COL_MUTED, COL_BG, 1);

    s_rendered_ds = *ds;
    s_rendered_ds_valid = true;
    s_rendered_bat = bat;
}

// === Page 0: OVERVIEW (Full Frame) ===
static void draw_page_overview_full(const struct tm *now)
{
    deepseek_usage_t ds = {0};
    deepseek_service_get_usage(&ds);

    gfx_fill_screen(COL_BG);
    draw_deck_header("OVERVIEW", "TODAY");

    // Static card shells drawn once
    const int cx = 130, cw = 100, ch = 52;
    const int cys[3] = {60, 118, 176};
    for (int i = 0; i < 3; i++) {
        gfx_fill_round_rect(cx, cys[i], cw, ch, 10, COL_CARD);
        gfx_draw_round_rect(cx, cys[i], cw, ch, 10, COL_BORDER);
    }

    draw_overview_data_partial(&ds, axp2101_get_battery_percent(), true);
    draw_deck_bottom_bar(now, 0);
}

// === Page 1: TOKENS — Partial Data Update ===
static void draw_tokens_data_partial(const deepseek_usage_t *ds)
{
    // Top Balance Value Area
    gfx_fill_rect(16, 80, 208, 24, COL_CARD);
    char bal_s[32];
    if (ds->balance_valid) {
        snprintf(bal_s, sizeof(bal_s), "%s %.2f", ds->currency, ds->total_balance);
        gfx_draw_utf8(20, 84, bal_s, COL_NEON_GREEN, COL_CARD);

        char sub_s[32];
        snprintf(sub_s, sizeof(sub_s), "赠:%.1f 充:%.1f", ds->granted_balance, ds->topped_up_balance);
        int sw = gfx_measure_utf8(sub_s);
        gfx_draw_utf8(222 - sw, 84, sub_s, COL_MUTED, COL_CARD);
    } else {
        gfx_draw_utf8(20, 84, "未配置 API Key", COL_AMBER, COL_CARD);
        gfx_draw_text(162, 88, "TAP SET", COL_NEON_GREEN, COL_CARD, 1);
    }

    // 2x2 Metric Value Areas
    char call_str[16], err_str[16];
    snprintf(call_str, sizeof(call_str), "%lu", (unsigned long)ds->calls);
    snprintf(err_str, sizeof(err_str), "%lu", (unsigned long)ds->errors);

    const int gx[2] = {10, 124};
    const int gy[2] = {118, 174};

    // Prompt & Completion tokens with crisp unit suffix
    gfx_fill_rect(gx[0] + 6, gy[0] + 22, 94, 22, COL_CARD);
    draw_token_value(gx[0] + 8, gy[0] + 24, ds->prompt_tokens, COL_CYAN, COL_CARD);

    gfx_fill_rect(gx[1] + 6, gy[0] + 22, 94, 22, COL_CARD);
    draw_token_value(gx[1] + 8, gy[0] + 24, ds->completion_tokens, COL_PURPLE, COL_CARD);

    // Calls & Errors
    gfx_fill_rect(gx[0] + 6, gy[1] + 22, 94, 22, COL_CARD);
    gfx_draw_utf8(gx[0] + 8, gy[1] + 24, call_str, COL_WHITE, COL_CARD);

    gfx_fill_rect(gx[1] + 6, gy[1] + 22, 94, 22, COL_CARD);
    gfx_draw_utf8(gx[1] + 8, gy[1] + 24, err_str, ds->errors > 0 ? COL_ORANGE : COL_NEON_GREEN, COL_CARD);

    s_rendered_ds = *ds;
    s_rendered_ds_valid = true;
}

static void draw_page_tokens_full(const struct tm *now)
{
    deepseek_usage_t ds = {0};
    deepseek_service_get_usage(&ds);

    gfx_fill_screen(COL_BG);
    draw_deck_header("TOKENS", ds.last_sync[0] ? ds.last_sync : "SYNC");

    gfx_fill_round_rect(10, 58, 220, 54, 10, COL_CARD);
    gfx_draw_round_rect(10, 58, 220, 54, 10, COL_BORDER);
    gfx_draw_text(20, 66, "DEEPSEEK BALANCE", COL_MUTED, COL_CARD, 1);

    const int gx[2] = {10, 124};
    const int gy[2] = {118, 174};
    const char *labels[4] = {"PROMPT TOKENS", "COMPLETION", "API CALLS", "ERRORS"};
    for (int i = 0; i < 4; i++) {
        int x = gx[i % 2];
        int y = gy[i / 2];
        gfx_fill_round_rect(x, y, 106, 50, 10, COL_CARD);
        gfx_draw_round_rect(x, y, 106, 50, 10, COL_BORDER);
        gfx_draw_text(x + 8, y + 8, labels[i], COL_MUTED, COL_CARD, 1);
    }

    draw_tokens_data_partial(&ds);
    gfx_draw_text_centered(120, 234, "TAP SCREEN TO REFRESH", COL_MUTED, COL_BG, 1);
    draw_deck_bottom_bar(now, 1);
}

// === Page 2: ALERTS (Clock, Weather & System Alerts) ===
static void draw_page_alerts_full(const struct tm *now)
{
    gfx_fill_screen(COL_BG);
    draw_deck_header("ALERTS", "MONITOR");

    gfx_fill_round_rect(10, 58, 220, 52, 10, COL_CARD);
    gfx_draw_round_rect(10, 58, 220, 52, 10, COL_BORDER);

    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", now->tm_hour & 0xFF, now->tm_min & 0xFF);
    gfx_draw_big_num(18, 66, tbuf, COL_WHITE, COL_CARD);

    weather_data_t w = {0};
    weather_service_get_data(&w);
    s_rendered_weather_valid = w.valid;
    s_rendered_weather_temp = (int)w.temp_c;
    if (w.valid) {
        char ws[24];
        snprintf(ws, sizeof(ws), "%.0f° %s", w.temp_c, w.desc);
        gfx_draw_utf8(142, 66, ws, COL_AMBER, COL_CARD);
        gfx_draw_utf8(142, 86, w.city, COL_MUTED, COL_CARD);
    } else {
        gfx_draw_utf8(142, 76, "天气离线", COL_MUTED, COL_CARD);
    }

    alert_stats_t st = {0};
    alert_server_get_stats(&st);
    s_rendered_alert_total = st.total;

    alert_record_t list[3];
    int total = alert_server_get_history(list, 3);
    if (total == 0) {
        gfx_fill_round_rect(10, 118, 220, 118, 10, COL_CARD);
        gfx_draw_round_rect(10, 118, 220, 118, 10, COL_BORDER);
        gfx_draw_utf8_centered(120, 156, "系统运行正常", COL_NEON_GREEN, COL_CARD);
        gfx_draw_utf8_centered(120, 180, "暂无告警记录", COL_MUTED, COL_CARD);
    } else {
        for (int i = 0; i < total; i++) {
            int y = 118 + i * 40;
            gfx_fill_round_rect(10, y, 220, 36, 8, COL_CARD);
            gfx_draw_round_rect(10, y, 220, 36, 8, COL_BORDER);
            gfx_fill_round_rect(15, y + 6, 4, 24, 2, level_color(list[i].level));
            gfx_draw_text(24, y + 5, list[i].time_str, COL_MUTED, COL_CARD, 1);
            gfx_draw_utf8(172, y + 2, list[i].acked ? "已确认" : "未处理",
                          list[i].acked ? COL_NEON_GREEN : COL_RED, COL_CARD);
            gfx_draw_utf8_wrap(24, y + 17, 196, 1, 16, list[i].title, COL_WHITE, COL_CARD);
        }
    }

    draw_deck_bottom_bar(now, 2);
}

// === Page 3: SYSTEM — Partial Brightness & Power-Save Update (Zero Flicker!) ===
static void draw_system_brightness_partial(void)
{
    // Brightness header row (inside brightness card y=126..196)
    gfx_fill_rect(18, 132, 204, 20, COL_CARD);

    char btxt[24];
    snprintf(btxt, sizeof(btxt), "亮度 %u%%", (unsigned)s_brightness);
    gfx_draw_utf8(20, 133, btxt, COL_WHITE, COL_CARD);

    // Auto-dim power-save pill on right
    char dtxt[20];
    if (s_auto_dim_sec == 0) snprintf(dtxt, sizeof(dtxt), "常亮模式");
    else if (s_auto_dim_sec >= 120) snprintf(dtxt, sizeof(dtxt), "省电:%um", (unsigned)(s_auto_dim_sec / 60));
    else snprintf(dtxt, sizeof(dtxt), "省电:%us", (unsigned)s_auto_dim_sec);

    uint16_t dcol = (s_auto_dim_sec > 0) ? COL_NEON_GREEN : COL_MUTED;
    gfx_fill_round_rect(136, 130, 86, 22, 6, COL_CARD_ALT);
    gfx_draw_utf8_centered(179, 133, dtxt, dcol, COL_CARD_ALT);

    // Slider bar track (x=54..186, y=166, w=132, h=14)
    const int bx = 54, by = 166, bw = 132, bh = 14;
    gfx_fill_round_rect(bx, by, bw, bh, 6, COL_RING_TRACK);
    int fill_w = (bw * (int)s_brightness) / 100;
    if (fill_w < 12) fill_w = 12;
    gfx_fill_round_rect(bx, by, fill_w, bh, 6, COL_NEON_GREEN);

    s_rendered_bl = s_brightness;
    s_rendered_dim = s_auto_dim_sec;
}

static void draw_page_system_full(const struct tm *now)
{
    app_config_t cfg;
    app_config_get(&cfg);

    char ip_s[20];
    wifi_time_get_ip(ip_s, sizeof(ip_s));

    gfx_fill_screen(COL_BG);
    draw_deck_header("SYSTEM", "POWER");

    // Compact Network & Device Info Card (y=56..118)
    gfx_fill_round_rect(10, 56, 220, 62, 10, COL_CARD);
    gfx_draw_round_rect(10, 56, 220, 62, 10, COL_BORDER);

    char line[48];
    snprintf(line, sizeof(line), "Wi-Fi: %s", cfg.wifi_ssid[0] ? cfg.wifi_ssid : "未配置");
    gfx_draw_utf8_wrap(18, 62, 204, 1, 18, line, COL_WHITE, COL_CARD);

    snprintf(line, sizeof(line), "IP: %s", wifi_time_is_connected() ? ip_s : "未连接");
    gfx_draw_utf8(18, 82, line, wifi_time_is_connected() ? COL_NEON_GREEN : COL_RED, COL_CARD);

    int bat = axp2101_get_battery_percent();
    bool chg = axp2101_is_charging();
    if (bat >= 0) snprintf(line, sizeof(line), "电量:%d%%%s", bat, chg ? "+" : "");
    else snprintf(line, sizeof(line), "USB供电");
    int bw = gfx_measure_utf8(line);
    gfx_draw_utf8(222 - bw, 82, line, COL_MUTED, COL_CARD);

    snprintf(line, sizeof(line), "Key:%s",
             (cfg.deepseek_api_key[0] || cfg.ai_api_key[0]) ? "已配" : "未配");
    gfx_draw_utf8(18, 100, line,
                  (cfg.deepseek_api_key[0] || cfg.ai_api_key[0]) ? COL_CYAN : COL_AMBER, COL_CARD);

    const char *ble_st = ble_ancs_is_ready() ? "BLE:已连" :
                         (ble_ancs_is_connected() ? "BLE:配对中" : "BLE:待连");
    uint16_t ble_col = ble_ancs_is_ready() ? COL_NEON_GREEN :
                       (ble_ancs_is_connected() ? COL_CYAN : COL_MUTED);
    int blw = gfx_measure_utf8(ble_st);
    gfx_draw_utf8(222 - blw, 100, ble_st, ble_col, COL_CARD);

    // Brightness & Power-Saving Control Card (y=124..198)
    gfx_fill_round_rect(10, 124, 220, 74, 10, COL_CARD);
    gfx_draw_round_rect(10, 124, 220, 74, 10, COL_BORDER);

    // [-] and [+] buttons (static shells)
    gfx_fill_round_rect(18, 159, 30, 28, 8, COL_CARD_ALT);
    gfx_draw_text_centered(33, 169, "-", COL_WHITE, COL_CARD_ALT, 1);

    gfx_fill_round_rect(192, 159, 30, 28, 8, COL_CARD_ALT);
    gfx_draw_text_centered(207, 169, "+", COL_WHITE, COL_CARD_ALT, 1);

    draw_system_brightness_partial();

    // Prominent Hotspot Launch Button (y=204..242)
    gfx_fill_round_rect(10, 204, 220, 38, 10, COL_NEON_GREEN);
    gfx_draw_utf8_centered(120, 215, "开启配网热点 (192.168.1.4)", COL_BG, COL_NEON_GREEN);

    draw_deck_bottom_bar(now, 3);
}

// === Single Alert Detail ===
static void draw_page_detail(void)
{
    gfx_fill_screen(COL_BG);
    uint16_t col = level_color(s_detail_rec.level);
    gfx_fill_round_rect(12, 14, 96, 26, 8, col);
    gfx_draw_utf8_centered(60, 19, level_name(s_detail_rec.level),
                           s_detail_rec.level == ALERT_LEVEL_WARN ? COL_BG : COL_WHITE, col);
    gfx_draw_utf8(120, 19, s_detail_rec.time_str, COL_MUTED, COL_BG);

    gfx_fill_round_rect(12, 48, 216, 178, 12, COL_CARD);
    gfx_draw_round_rect(12, 48, 216, 178, 12, COL_BORDER);
    gfx_draw_utf8_wrap(22, 58, 196, 1, 18, s_detail_rec.title, COL_CYAN, COL_CARD);
    gfx_draw_hline(22, 80, 196, COL_BORDER);
    gfx_draw_utf8_wrap(22, 90, 196, 5, 22, s_detail_rec.message, COL_WHITE, COL_CARD);

    gfx_fill_round_rect(12, 236, 216, 36, 10, COL_CARD_ALT);
    gfx_draw_round_rect(12, 236, 216, 36, 10, COL_BORDER);
    gfx_draw_utf8_centered(120, 246, "返回告警列表", COL_WHITE, COL_CARD_ALT);
}

// === Active Voice Alert Overlay ===
static void draw_alert_overlay(const alert_state_t *st)
{
    uint16_t hdr_col = level_color(st->current.level);
    uint16_t hdr_fg  = (st->current.level == ALERT_LEVEL_WARN) ? COL_BG : COL_WHITE;

    gfx_fill_screen(COL_BG);
    gfx_fill_round_rect(12, 12, 216, 40, 12, hdr_col);
    gfx_draw_utf8_centered(120, 24,
                           st->current.has_voice ? "告警 · 语音循环播报中" : level_name(st->current.level),
                           hdr_fg, hdr_col);

    gfx_fill_round_rect(12, 60, 216, 158, 14, COL_CARD);
    gfx_draw_round_rect(12, 60, 216, 158, 14, hdr_col);
    gfx_draw_utf8(22, 68, st->current.time_str, COL_MUTED, COL_CARD);
    gfx_draw_utf8_wrap(22, 88, 196, 1, 18, st->current.title, COL_CYAN, COL_CARD);
    gfx_draw_hline(22, 110, 196, COL_BORDER);
    gfx_draw_utf8_wrap(22, 118, 196, 4, 22, st->current.message, COL_WHITE, COL_CARD);

    gfx_fill_round_rect(12, 226, 216, 44, 12, COL_NEON_GREEN);
    gfx_draw_utf8_centered(120, 240, "取消语音提醒 (点此 / BOOT)", COL_BG, COL_NEON_GREEN);
}

static void draw_wechat_overlay(const ble_ancs_msg_t *msg)
{
    gfx_fill_screen(COL_BG);
    gfx_fill_round_rect(12, 14, 216, 36, 10, COL_NEON_GREEN);
    gfx_draw_utf8_centered(120, 24, "微信消息", COL_BG, COL_NEON_GREEN);

    gfx_fill_round_rect(12, 58, 216, 166, 14, COL_CARD);
    gfx_draw_round_rect(12, 58, 216, 166, 14, COL_BORDER);
    gfx_draw_utf8_wrap(22, 68, 196, 1, 18, msg->title[0] ? msg->title : "微信", COL_CYAN, COL_CARD);
    gfx_draw_hline(22, 90, 196, COL_BORDER);
    gfx_draw_utf8_wrap(22, 100, 196, 5, 22, msg->message, COL_WHITE, COL_CARD);

    gfx_fill_round_rect(20, 234, 200, 36, 10, COL_CARD_ALT);
    gfx_draw_utf8_centered(120, 244, "点击关闭", COL_WHITE, COL_CARD_ALT);
}

// === SoftAP Wi-Fi Setup Hotspot Overlay (192.168.1.4) ===
static void draw_portal_overlay(void)
{
    gfx_fill_screen(COL_BG);

    gfx_fill_round_rect(12, 14, 216, 36, 10, COL_NEON_GREEN);
    gfx_draw_utf8_centered(120, 24, "Wi-Fi 热点配网已开启", COL_BG, COL_NEON_GREEN);

    gfx_fill_round_rect(12, 58, 216, 154, 14, COL_CARD);
    gfx_draw_round_rect(12, 58, 216, 154, 14, COL_NEON_GREEN);

    gfx_draw_utf8_centered(120, 70, "1. 手机连接无密码热点:", COL_MUTED, COL_CARD);
    gfx_draw_utf8_centered(120, 92, web_portal_get_ap_ssid(), COL_CYAN, COL_CARD);

    gfx_draw_utf8_centered(120, 122, "2. 手机浏览器访问:", COL_MUTED, COL_CARD);
    char url[32];
    snprintf(url, sizeof(url), "http://%s", web_portal_get_ap_ip());
    gfx_draw_utf8_centered(120, 144, url, COL_AMBER, COL_CARD);

    gfx_draw_utf8_centered(120, 174, "配置 Wi-Fi 账号密码及 Key", COL_NEON_GREEN, COL_CARD);
    gfx_draw_utf8_centered(120, 192, "保存后设备将自动重启", COL_MUTED, COL_CARD);

    gfx_fill_round_rect(16, 224, 208, 40, 10, COL_CARD_ALT);
    gfx_draw_round_rect(16, 224, 208, 40, 10, COL_BORDER);
    gfx_draw_utf8_centered(120, 236, "关闭热点并返回", COL_WHITE, COL_CARD_ALT);
}

static void open_hotspot_portal(void)
{
    s_portal_dismissed = false;
    web_portal_start();
    s_full_redraw = true;
}

static void handle_tap(uint16_t x, uint16_t y)
{
    if (alert_server_get_state(NULL)) {
        if (y >= 216) {
            alert_server_stop();
            s_full_redraw = true;
        }
        return;
    }

    if (s_page == PAGE_DETAIL) {
        if (y >= 225) {
            s_page = PAGE_ALERTS;
            s_full_redraw = true;
        }
        return;
    }

    // Bottom Bar Actions (y >= 246)
    if (y >= 246) {
        if (x <= 72) {
            if (s_page != PAGE_OVERVIEW) {
                s_page = PAGE_OVERVIEW;
                s_full_redraw = true;
            }
        } else if (x >= 152) {
            open_hotspot_portal();
        } else {
            s_page = (ui_page_t)((s_page + 1) % MAIN_PAGE_COUNT);
            s_full_redraw = true;
        }
        return;
    }

    // Page-specific taps
    if (s_page == PAGE_OVERVIEW || s_page == PAGE_TOKENS) {
        // Trigger async background API refresh — UI will update seamlessly with zero flicker when data arrives!
        deepseek_service_request_refresh();
    } else if (s_page == PAGE_ALERTS) {
        if (y >= 58 && y <= 112) {
            weather_service_request_refresh();
        } else if (y >= 118 && y < 240) {
            int row = (y - 118) / 40;
            alert_record_t list[3];
            int total = alert_server_get_history(list, 3);
            if (row >= 0 && row < total) {
                s_detail_rec = list[row];
                s_page = PAGE_DETAIL;
                s_full_redraw = true;
            }
        }
    } else if (s_page == PAGE_SYSTEM) {
        if (y >= 124 && y <= 156 && x >= 130) {
            // Toggle auto-dim power saving timeout (partial update only!)
            cycle_auto_dim_and_save();
            draw_system_brightness_partial();
        } else if (y >= 155 && y <= 198) {
            // Brightness [-] / slider / [+] (partial update only — zero flicker!)
            if (x < 52) {
                int nb = (int)s_brightness - 10;
                set_brightness_and_save(nb < 10 ? 10 : (uint8_t)nb);
            } else if (x > 188) {
                int nb = (int)s_brightness + 10;
                set_brightness_and_save(nb > 100 ? 100 : (uint8_t)nb);
            } else {
                // Direct tap on slider bar (x=54..186 -> 10%..100%)
                int pct = 10 + ((int)(x - 54) * 90) / 132;
                pct = ((pct + 5) / 10) * 10; // Snap to nearest 10%
                if (pct < 10) pct = 10;
                if (pct > 100) pct = 100;
                set_brightness_and_save((uint8_t)pct);
            }
            draw_system_brightness_partial();
        } else if (y >= 202 && y <= 244) {
            open_hotspot_portal();
        }
    }
}

void app_ui_init(void)
{
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOARD_BTN_BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    app_config_t cfg;
    app_config_get(&cfg);
    s_brightness = (cfg.brightness >= 10 && cfg.brightness <= 100) ? cfg.brightness : 80;
    s_auto_dim_sec = cfg.auto_dim_sec;
    lcd_st7789_set_backlight(s_brightness);
    s_last_activity_ms = now_ms();
    s_is_dimmed = false;

    s_full_redraw = true;
    ESP_LOGI(TAG, "Usage Deck UI ready (zero-flicker partial refresh, brightness=%u%%, auto_dim=%us)",
             (unsigned)s_brightness, (unsigned)s_auto_dim_sec);
}

void app_ui_tick(void)
{
    // 1. Hardware BOOT button
    bool boot_pressed = (gpio_get_level(BOARD_BTN_BOOT_GPIO) == 0);
    if (boot_pressed) {
        if (s_is_dimmed) {
            wake_backlight();
            s_prev_boot_btn = true;
            s_boot_long_fired = true; // Consume wake press
            return;
        }
        s_last_activity_ms = now_ms();
        s_boot_press_ticks++;
        if (s_boot_press_ticks >= 85 && !s_boot_long_fired) {
            s_boot_long_fired = true;
            open_hotspot_portal();
        }
    } else if (s_prev_boot_btn) {
        if (!s_boot_long_fired) {
            if (alert_server_get_state(NULL)) {
                alert_server_stop();
            } else if (s_wx_showing) {
                s_wx_showing = false;
                s_wx_ticks_left = 0;
            } else if (web_portal_is_active() && !s_portal_dismissed) {
                web_portal_stop();
                s_portal_dismissed = true;
            } else {
                s_page = (ui_page_t)((s_page + 1) % MAIN_PAGE_COUNT);
            }
            s_full_redraw = true;
        }
        s_boot_press_ticks = 0;
        s_boot_long_fired = false;
    }
    s_prev_boot_btn = boot_pressed;

    // 2. Touch & horizontal swipe handling
    touch_point_t pt = {0};
    touch_cst816d_read(&pt);
    if (pt.pressed && !s_prev_pressed) {
        if (s_is_dimmed) {
            // First touch wakes the screen cleanly without triggering underlying buttons
            wake_backlight();
            s_prev_pressed = false;
            return;
        }
        s_last_activity_ms = now_ms();
        s_touch_start_x = pt.x;
        s_touch_start_y = pt.y;
    } else if (!pt.pressed && s_prev_pressed) {
        s_last_activity_ms = now_ms();
        if (alert_server_get_state(NULL)) {
            handle_tap(s_touch_start_x, s_touch_start_y);
        } else if (s_wx_showing) {
            s_wx_showing = false;
            s_wx_ticks_left = 0;
            s_full_redraw = true;
        } else if (web_portal_is_active() && !s_portal_dismissed) {
            if (s_touch_start_y >= 216) {
                web_portal_stop();
                s_portal_dismissed = true;
                s_full_redraw = true;
            }
        } else {
            int dx = (int)pt.x - (int)s_touch_start_x;
            if (dx < -45) {
                if (s_page < MAIN_PAGE_COUNT - 1) {
                    s_page = (ui_page_t)(s_page + 1);
                    s_full_redraw = true;
                }
            } else if (dx > 45) {
                if (s_page == PAGE_DETAIL) {
                    s_page = PAGE_ALERTS;
                    s_full_redraw = true;
                } else if (s_page > 0) {
                    s_page = (ui_page_t)(s_page - 1);
                    s_full_redraw = true;
                }
            } else {
                handle_tap(s_touch_start_x, s_touch_start_y);
            }
        }
    }
    s_prev_pressed = pt.pressed;

    // 3. Active Voice Alert Overlay (wakes screen automatically!)
    alert_state_t alert_st = {0};
    bool alert_on = alert_server_get_state(&alert_st);
    if (alert_on != s_was_alert || (alert_on && alert_st.seq != s_last_alert_seq)) {
        s_was_alert = alert_on;
        s_last_alert_seq = alert_st.seq;
        if (alert_on) wake_backlight();
        s_full_redraw = true;
    }
    if (alert_on) {
        if (s_full_redraw) {
            draw_alert_overlay(&alert_st);
            s_full_redraw = false;
        }
        return;
    }

    // 4. iPhone WeChat notification popup
    ble_ancs_msg_t latest_wx = {0};
    if (ble_ancs_get_latest_msg(&latest_wx) && latest_wx.seq != s_last_wx_seq) {
        s_last_wx_seq = latest_wx.seq;
        s_wx_msg = latest_wx;
        s_wx_showing = true;
        s_wx_ticks_left = 500;
        wake_backlight();
        s_full_redraw = true;
    }
    if (s_wx_showing) {
        if (s_full_redraw) {
            draw_wechat_overlay(&s_wx_msg);
            s_full_redraw = false;
        }
        if (--s_wx_ticks_left <= 0) {
            s_wx_showing = false;
            s_full_redraw = true;
        }
        return;
    }

    // 5. SoftAP Wi-Fi setup hotspot overlay (192.168.1.4)
    bool portal_on = web_portal_is_active();
    if (portal_on != s_was_portal) {
        s_was_portal = portal_on;
        if (portal_on) {
            s_portal_dismissed = false;
            wake_backlight();
        }
        s_full_redraw = true;
    }
    if (portal_on && !s_portal_dismissed) {
        if (s_full_redraw) {
            draw_portal_overlay();
            s_full_redraw = false;
        }
        return;
    }

    // 6. Auto-Dim Power Saving Check
    if (s_auto_dim_sec > 0 && !s_is_dimmed) {
        if ((now_ms() - s_last_activity_ms) >= (int64_t)s_auto_dim_sec * 1000) {
            s_is_dimmed = true;
            lcd_st7789_set_backlight(DIM_BACKLIGHT_PCT);
        }
    }

    // 7. Full Redraw ONLY on page transition / overlay exit
    struct tm now = {0};
    wifi_time_get_local(&now);
    if (s_full_redraw) {
        s_full_redraw = false;
        if (s_page == PAGE_OVERVIEW)      draw_page_overview_full(&now);
        else if (s_page == PAGE_TOKENS)   draw_page_tokens_full(&now);
        else if (s_page == PAGE_ALERTS)   draw_page_alerts_full(&now);
        else if (s_page == PAGE_SYSTEM)   draw_page_system_full(&now);
        else                              draw_page_detail();
        return;
    }

    // 8. Zero-Flicker Differential Partial Refresh (check ~3 times/sec, redraw ONLY changed elements!)
    if (++s_poll_div < 10) {
        return;
    }
    s_poll_div = 0;

    if (s_page == PAGE_DETAIL) {
        return;
    }

    // 8a. Bottom timestamp (only when minute changes)
    if (now.tm_min != s_rendered_min) {
        draw_bottom_time_partial(&now);
        if (s_page == PAGE_ALERTS) {
            char tbuf[16];
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d", now.tm_hour & 0xFF, now.tm_min & 0xFF);
            gfx_draw_big_num(18, 66, tbuf, COL_WHITE, COL_CARD);
        }
    }

    // 8b. Top-right Wi-Fi status (only when connection state changes)
    bool cur_wifi = wifi_time_is_connected();
    bool cur_ap = web_portal_is_active();
    if (cur_wifi != s_rendered_wifi || cur_ap != s_rendered_ap) {
        draw_header_wifi_status();
    }

    // 8c. Page-specific differential data updates
    if (s_page == PAGE_OVERVIEW) {
        deepseek_usage_t ds = {0};
        deepseek_service_get_usage(&ds);
        int bat = axp2101_get_battery_percent();
        bool ds_changed = (!s_rendered_ds_valid || memcmp(&ds, &s_rendered_ds, sizeof(ds)) != 0);
        if (ds_changed || bat != s_rendered_bat) {
            // Only redraw the circular ring if ring-affecting metrics actually changed
            bool ring_changed = !s_rendered_ds_valid ||
                                ds.calls != s_rendered_ds.calls ||
                                ds.errors != s_rendered_ds.errors ||
                                ds.balance_valid != s_rendered_ds.balance_valid ||
                                ds.is_available != s_rendered_ds.is_available ||
                                ds.today_cost != s_rendered_ds.today_cost ||
                                ds.daily_budget != s_rendered_ds.daily_budget ||
                                strcmp(ds.status_text, s_rendered_ds.status_text) != 0;
            draw_overview_data_partial(&ds, bat, ring_changed);
        }
    } else if (s_page == PAGE_TOKENS) {
        deepseek_usage_t ds = {0};
        deepseek_service_get_usage(&ds);
        if (!s_rendered_ds_valid || memcmp(&ds, &s_rendered_ds, sizeof(ds)) != 0) {
            draw_tokens_data_partial(&ds);
        }
    } else if (s_page == PAGE_ALERTS) {
        alert_stats_t st = {0};
        alert_server_get_stats(&st);
        weather_data_t w = {0};
        weather_service_get_data(&w);
        if (st.total != s_rendered_alert_total ||
            w.valid != s_rendered_weather_valid ||
            (w.valid && (int)w.temp_c != s_rendered_weather_temp)) {
            draw_page_alerts_full(&now);
        }
    } else if (s_page == PAGE_SYSTEM) {
        // Sync if brightness was changed remotely via HTTP /api/brightness or serial 'bl'
        uint8_t hw_bl = lcd_st7789_get_backlight();
        if (!s_is_dimmed && hw_bl >= 10 && hw_bl != s_brightness) {
            s_brightness = hw_bl;
        }
        if (s_brightness != s_rendered_bl || s_auto_dim_sec != s_rendered_dim) {
            draw_system_brightness_partial();
        }
    }
}
