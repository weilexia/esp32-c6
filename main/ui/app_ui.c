#include "app_ui.h"
#include "ai_voice_alarm.h"
#include "alarm_manager.h"
#include "axp2101.h"
#include "board_pins.h"
#include "gfx.h"
#include "touch_cst816d.h"
#include "weather_service.h"
#include "web_portal.h"
#include "wifi_time.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_ui";

typedef enum {
    PAGE_HOME = 0,
    PAGE_CALENDAR,
    PAGE_ALARMS,
    PAGE_AI_VOICE,
    PAGE_COUNT
} ui_page_t;

static ui_page_t s_page = PAGE_HOME;
static bool s_dirty = true;
static bool s_was_ringing = false;
static int s_cal_year = 2026;
static int s_cal_month = 11; // 1..12
static int s_last_sec = -1;
static int s_last_min = -1;
static ai_voice_state_t s_last_ai_state = AI_VOICE_IDLE;
static bool s_prev_pressed = false;
static uint16_t s_touch_start_x = 0;
static uint16_t s_touch_start_y = 0;
static bool s_prev_boot_btn = false;
static int s_boot_press_ticks = 0;
static bool s_boot_long_fired = false;
static bool s_was_portal = false;
static bool s_portal_dismissed = false;

static const char *DOW_NAMES[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static int days_in_month(int year, int month)
{
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int d = dim[(month - 1) % 12];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
        d = 29;
    }
    return d;
}

static int first_weekday_of_month(int year, int month)
{
    struct tm t = {0};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = 1;
    mktime(&t);
    return t.tm_wday; // 0=Sun..6=Sat
}

static void draw_status_bar(void)
{
    gfx_fill_rect(12, 8, 216, 20, COL_BG);

    // Wi-Fi status
    bool wifi = wifi_time_is_connected();
    gfx_draw_text(16, 12, wifi ? "WiFi" : "Off ", wifi ? COL_GREEN : COL_MUTED, COL_BG, 1);

    // Active alarm badge
    int ac = alarm_manager_active_count();
    char abuf[16];
    snprintf(abuf, sizeof(abuf), "AL:%d", ac);
    gfx_draw_text_centered(120, 12, abuf, ac > 0 ? COL_AMBER : COL_MUTED, COL_BG, 1);

    // Battery percentage from AXP2101
    int bat = axp2101_get_battery_percent();
    bool chg = axp2101_is_charging();
    char bbuf[16];
    if (bat >= 0) {
        snprintf(bbuf, sizeof(bbuf), "%d%%%s", bat, chg ? "+" : "");
    } else {
        snprintf(bbuf, sizeof(bbuf), "USB");
    }
    gfx_draw_text(182, 12, bbuf, (bat >= 0 && bat < 20) ? COL_RED : COL_CYAN, COL_BG, 1);
    gfx_draw_hline(14, 28, 212, COL_BORDER);
}

static void draw_bottom_nav(const char *b0, const char *b1, const char *b2, int active_idx)
{
    const int y = 236;
    const int h = 34;
    const int w = 66;
    const int xs[3] = {14, 87, 160};
    const char *labels[3] = {b0, b1, b2};

    for (int i = 0; i < 3; i++) {
        uint16_t bg = (i == active_idx) ? COL_BLUE : COL_CARD_ALT;
        uint16_t fg = (i == active_idx) ? COL_WHITE : COL_CYAN;
        gfx_fill_round_rect(xs[i], y, w, h, 8, bg);
        gfx_draw_text_centered(xs[i] + w / 2, y + 13, labels[i], fg, bg, 1);
    }
}

static void draw_home_clock_partial(const struct tm *now)
{
    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", now->tm_hour & 0xFF, now->tm_min & 0xFF);
    gfx_draw_text(28, 44, tbuf, COL_WHITE, COL_CARD, 4);

    char sbuf[16];
    snprintf(sbuf, sizeof(sbuf), ":%02d", now->tm_sec & 0xFF);
    gfx_draw_text(176, 60, sbuf, COL_CYAN, COL_CARD, 2);
}

static void draw_page_home(const struct tm *now)
{
    gfx_fill_screen(COL_BG);
    draw_status_bar();

    // Clock Card
    gfx_fill_round_rect(12, 34, 216, 78, 12, COL_CARD);
    draw_home_clock_partial(now);

    char dbuf[48];
    snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d  %s",
             (now->tm_year + 1900) % 10000, (now->tm_mon + 1) & 0xFF, now->tm_mday & 0xFF,
             DOW_NAMES[now->tm_wday % 7]);
    gfx_draw_text_centered(120, 90, dbuf, COL_MUTED, COL_CARD, 1);

    // Weather Card
    weather_data_t w = {0};
    weather_service_get_data(&w);
    gfx_fill_round_rect(12, 120, 216, 108, 12, COL_CARD);
    gfx_draw_text(22, 128, w.city, COL_CYAN, COL_CARD, 1);
    gfx_draw_text(168, 128, "[Refr]", COL_MUTED, COL_CARD, 1);

    if (w.valid) {
        gfx_draw_weather_icon(48, 168, w.weather_code, COL_CARD);
        char temp_s[24];
        snprintf(temp_s, sizeof(temp_s), "%.0fC", w.temp_c);
        gfx_draw_text(86, 148, temp_s, COL_WHITE, COL_CARD, 3);
        gfx_draw_text(86, 178, w.desc, COL_AMBER, COL_CARD, 1);

        char detail[64];
        snprintf(detail, sizeof(detail), "H:%.0f L:%.0f  RH:%d%%",
                 w.daily_max[0], w.daily_min[0], w.humidity_pct);
        gfx_draw_text(22, 206, detail, COL_MUTED, COL_CARD, 1);
    } else {
        gfx_draw_weather_icon(48, 168, 1, COL_CARD);
        gfx_draw_text(86, 156, "-- C", COL_WHITE, COL_CARD, 3);
        gfx_draw_text(86, 188, wifi_time_is_connected() ? "Updating..." : "No Wi-Fi", COL_MUTED, COL_CARD, 1);
    }

    draw_bottom_nav("Cal", "AI Mic", "Alarm", -1);
}

static void draw_page_calendar(const struct tm *now)
{
    gfx_fill_screen(COL_BG);
    draw_status_bar();

    // Month header with < and > buttons
    gfx_fill_round_rect(14, 34, 34, 22, 6, COL_CARD_ALT);
    gfx_draw_text_centered(31, 41, "<", COL_WHITE, COL_CARD_ALT, 1);

    char hdr[32];
    snprintf(hdr, sizeof(hdr), "%04d - %02d", s_cal_year % 10000, s_cal_month & 0xFF);
    gfx_draw_text_centered(120, 38, hdr, COL_WHITE, COL_BG, 2);

    gfx_fill_round_rect(192, 34, 34, 22, 6, COL_CARD_ALT);
    gfx_draw_text_centered(209, 41, ">", COL_WHITE, COL_CARD_ALT, 1);

    // Weekday header
    static const char *wd[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    for (int i = 0; i < 7; i++) {
        uint16_t col = (i == 0 || i == 6) ? COL_AMBER : COL_MUTED;
        gfx_draw_text(18 + i * 30, 62, wd[i], col, COL_BG, 1);
    }
    gfx_draw_hline(14, 73, 212, COL_BORDER);

    // Calendar grid
    int first_wd = first_weekday_of_month(s_cal_year, s_cal_month);
    int dim = days_in_month(s_cal_year, s_cal_month);
    int today_y = now->tm_year + 1900;
    int today_m = now->tm_mon + 1;
    int today_d = now->tm_mday;

    for (int d = 1; d <= dim; d++) {
        int idx = first_wd + (d - 1);
        int col = idx % 7;
        int row = idx / 7;
        int cx = 14 + col * 30;
        int cy = 78 + row * 19;

        bool is_today = (s_cal_year == today_y && s_cal_month == today_m && d == today_d);
        uint16_t bg = is_today ? COL_CYAN : COL_BG;
        uint16_t fg = is_today ? COL_BG : ((col == 0 || col == 6) ? COL_AMBER : COL_WHITE);
        if (is_today) {
            gfx_fill_round_rect(cx, cy - 2, 26, 15, 5, bg);
        }
        char ds[16];
        snprintf(ds, sizeof(ds), "%2d", d);
        gfx_draw_text(cx + 6, cy + 1, ds, fg, bg, 1);
    }

    // 3-day Forecast Strip
    weather_data_t w = {0};
    weather_service_get_data(&w);
    gfx_fill_round_rect(12, 198, 216, 30, 8, COL_CARD);
    if (w.valid) {
        char fbuf[64];
        snprintf(fbuf, sizeof(fbuf), "D0:%.0f/%.0f D1:%.0f/%.0f D2:%.0f/%.0f",
                 w.daily_min[0], w.daily_max[0],
                 w.daily_min[1], w.daily_max[1],
                 w.daily_min[2], w.daily_max[2]);
        gfx_draw_text_centered(120, 209, fbuf, COL_CYAN, COL_CARD, 1);
    } else {
        gfx_draw_text_centered(120, 209, "3-Day Forecast (Online)", COL_MUTED, COL_CARD, 1);
    }

    draw_bottom_nav("Home", "AI Mic", "Alarm", -1);
}

static void draw_page_alarms(void)
{
    gfx_fill_screen(COL_BG);
    draw_status_bar();

    gfx_draw_text(16, 36, "Alarms", COL_WHITE, COL_BG, 2);
    gfx_fill_round_rect(164, 34, 62, 22, 6, COL_PURPLE);
    gfx_draw_text_centered(195, 41, "+AI", COL_WHITE, COL_PURPLE, 1);

    alarm_entry_t list[ALARM_MAX_COUNT];
    alarm_manager_get_all(list, ALARM_MAX_COUNT);

    int shown = 0;
    for (int i = 0; i < ALARM_MAX_COUNT && shown < 5; i++) {
        if (!list[i].used) continue;
        int y = 62 + shown * 33;
        gfx_fill_round_rect(12, y, 216, 29, 8, COL_CARD);

        char tbuf[32];
        snprintf(tbuf, sizeof(tbuf), "%02u:%02u %s", list[i].hour, list[i].minute,
                 list[i].repeat_daily ? "D" : "1");
        gfx_draw_text(20, y + 6, tbuf, list[i].enabled ? COL_WHITE : COL_MUTED, COL_CARD, 2);

        // Toggle pill
        uint16_t tbg = list[i].enabled ? COL_GREEN : COL_CARD_ALT;
        gfx_fill_round_rect(152, y + 5, 38, 19, 6, tbg);
        gfx_draw_text_centered(171, y + 11, list[i].enabled ? "ON" : "OFF", COL_WHITE, tbg, 1);

        // Delete X button
        gfx_fill_round_rect(196, y + 5, 24, 19, 6, COL_RED);
        gfx_draw_text_centered(208, y + 11, "X", COL_WHITE, COL_RED, 1);
        shown++;
    }

    if (shown == 0) {
        gfx_draw_text_centered(120, 128, "No alarms set", COL_MUTED, COL_BG, 2);
        gfx_draw_text_centered(120, 154, "Tap +AI or +5m below", COL_CYAN, COL_BG, 1);
    }

    draw_bottom_nav("Home", "Cal", "+5m", -1);
}

static void draw_page_ai_voice(void)
{
    gfx_fill_screen(COL_BG);
    draw_status_bar();

    ai_voice_status_t st = {0};
    ai_voice_alarm_get_status(&st);

    gfx_draw_text_centered(120, 36, "AI Voice Alarm", COL_WHITE, COL_BG, 2);

    uint16_t mic_col = COL_CYAN;
    const char *mic_lbl = "TAP";
    if (st.state == AI_VOICE_LISTENING) {
        mic_col = COL_RED;
        mic_lbl = "REC";
    } else if (st.state == AI_VOICE_THINKING) {
        mic_col = COL_AMBER;
        mic_lbl = "...";
    } else if (st.state == AI_VOICE_DONE) {
        mic_col = COL_GREEN;
        mic_lbl = "OK!";
    }

    gfx_fill_circle(120, 102, 36, mic_col);
    gfx_draw_text_centered(120, 94, mic_lbl, COL_BG, mic_col, 2);

    // Status & Reply card
    gfx_fill_round_rect(14, 148, 212, 44, 10, COL_CARD);
    gfx_draw_text_centered(120, 156, st.reply_text[0] ? st.reply_text : "Tap Mic to Speak", COL_WHITE, COL_CARD, 1);
    if (st.heard_text[0]) {
        char trunc[32];
        strncpy(trunc, st.heard_text, 31);
        trunc[31] = '\0';
        gfx_draw_text_centered(120, 174, trunc, COL_MUTED, COL_CARD, 1);
    }

    // Quick timer pills (+5m / +15m / +30m)
    const char *q[3] = {"+5m", "+15m", "+30m"};
    for (int i = 0; i < 3; i++) {
        int qx = 14 + i * 73;
        gfx_fill_round_rect(qx, 200, 66, 26, 8, COL_CARD_ALT);
        gfx_draw_text_centered(qx + 33, 209, q[i], COL_AMBER, COL_CARD_ALT, 1);
    }

    draw_bottom_nav("Home", "Cal", "Alarm", -1);
}

static void draw_portal_overlay(void)
{
    gfx_fill_screen(COL_BG);
    gfx_fill_round_rect(14, 18, 212, 36, 10, COL_BLUE);
    gfx_draw_text_centered(120, 28, "Wi-Fi Setup", COL_WHITE, COL_BLUE, 2);

    gfx_fill_round_rect(14, 66, 212, 134, 12, COL_CARD);
    gfx_draw_text_centered(120, 78, "1. Connect Phone Wi-Fi:", COL_MUTED, COL_CARD, 1);
    gfx_draw_text_centered(120, 96, web_portal_get_ap_ssid(), COL_CYAN, COL_CARD, 1);

    gfx_draw_text_centered(120, 124, "2. Open in Browser:", COL_MUTED, COL_CARD, 1);
    gfx_draw_text_centered(120, 142, "http://192.168.4.1", COL_AMBER, COL_CARD, 1);

    gfx_draw_text_centered(120, 174, "Set Wi-Fi & AI API Key", COL_GREEN, COL_CARD, 1);

    gfx_fill_round_rect(26, 218, 188, 36, 10, COL_CARD_ALT);
    gfx_draw_text_centered(120, 231, "Tap to Use Offline", COL_WHITE, COL_CARD_ALT, 1);
}

static void draw_ringing_overlay(const alarm_entry_t *a)
{
    gfx_fill_screen(COL_BG);
    gfx_fill_round_rect(16, 20, 208, 44, 12, COL_RED);
    gfx_draw_text_centered(120, 34, "ALARM!", COL_WHITE, COL_RED, 2);

    char tbuf[12];
    snprintf(tbuf, sizeof(tbuf), "%02u:%02u", a->hour, a->minute);
    gfx_draw_text_centered(120, 86, tbuf, COL_AMBER, COL_BG, 4);
    gfx_draw_text_centered(120, 134, a->label, COL_WHITE, COL_BG, 2);

    // Snooze & Dismiss buttons
    gfx_fill_round_rect(18, 180, 96, 56, 12, COL_AMBER);
    gfx_draw_text_centered(66, 202, "SNOOZE", COL_BG, COL_AMBER, 2);

    gfx_fill_round_rect(126, 180, 96, 56, 12, COL_GREEN);
    gfx_draw_text_centered(174, 202, "STOP", COL_BG, COL_GREEN, 2);
}

static void add_quick_minutes(int mins)
{
    struct tm now = {0};
    wifi_time_get_local(&now);
    int tot = now.tm_hour * 60 + now.tm_min + mins;
    uint8_t h = (uint8_t)((tot / 60) % 24);
    uint8_t m = (uint8_t)(tot % 60);
    char lbl[24];
    snprintf(lbl, sizeof(lbl), "Quick +%dm", mins);
    alarm_manager_add(h, m, false, lbl);
}

static void handle_tap(uint16_t x, uint16_t y)
{
    alarm_entry_t ring_a = {0};
    if (alarm_manager_is_ringing(&ring_a)) {
        if (y >= 170) {
            if (x < 120) {
                alarm_manager_snooze(5);
            } else {
                alarm_manager_dismiss();
            }
            s_dirty = true;
        }
        return;
    }

    // Bottom nav dock (y >= 230)
    if (y >= 230) {
        int btn = (x < 82) ? 0 : (x < 156 ? 1 : 2);
        if (s_page == PAGE_HOME) {
            if (btn == 0) s_page = PAGE_CALENDAR;
            else if (btn == 1) s_page = PAGE_AI_VOICE;
            else s_page = PAGE_ALARMS;
        } else if (s_page == PAGE_CALENDAR) {
            if (btn == 0) s_page = PAGE_HOME;
            else if (btn == 1) s_page = PAGE_AI_VOICE;
            else s_page = PAGE_ALARMS;
        } else if (s_page == PAGE_ALARMS) {
            if (btn == 0) s_page = PAGE_HOME;
            else if (btn == 1) s_page = PAGE_CALENDAR;
            else add_quick_minutes(5);
        } else if (s_page == PAGE_AI_VOICE) {
            if (btn == 0) s_page = PAGE_HOME;
            else if (btn == 1) s_page = PAGE_CALENDAR;
            else s_page = PAGE_ALARMS;
        }
        s_dirty = true;
        return;
    }

    // Per-page tap targets
    if (s_page == PAGE_HOME) {
        if (y >= 120 && y <= 228) {
            weather_service_request_refresh();
            s_dirty = true;
        }
    } else if (s_page == PAGE_CALENDAR) {
        if (y >= 30 && y <= 60) {
            if (x < 70) {
                s_cal_month--;
                if (s_cal_month < 1) { s_cal_month = 12; s_cal_year--; }
                s_dirty = true;
            } else if (x > 170) {
                s_cal_month++;
                if (s_cal_month > 12) { s_cal_month = 1; s_cal_year++; }
                s_dirty = true;
            }
        }
    } else if (s_page == PAGE_ALARMS) {
        if (y >= 30 && y <= 58 && x >= 160) {
            s_page = PAGE_AI_VOICE;
            ai_voice_alarm_start_listening();
            s_dirty = true;
            return;
        }
        if (y >= 62 && y < 228) {
            int row = (y - 62) / 33;
            alarm_entry_t list[ALARM_MAX_COUNT];
            alarm_manager_get_all(list, ALARM_MAX_COUNT);
            int shown = 0;
            for (int i = 0; i < ALARM_MAX_COUNT; i++) {
                if (!list[i].used) continue;
                if (shown == row) {
                    if (x >= 194) {
                        alarm_manager_delete(i);
                    } else if (x >= 148) {
                        alarm_manager_toggle(i);
                    }
                    s_dirty = true;
                    break;
                }
                shown++;
            }
        }
    } else if (s_page == PAGE_AI_VOICE) {
        if (y >= 60 && y <= 145) {
            ai_voice_alarm_start_listening();
            s_dirty = true;
        } else if (y >= 198 && y <= 228) {
            if (x < 84) add_quick_minutes(5);
            else if (x < 156) add_quick_minutes(15);
            else add_quick_minutes(30);
            s_page = PAGE_ALARMS;
            s_dirty = true;
        }
    }
}

void app_ui_init(void)
{
    // Configure BOOT button (GPIO9, active-low) as quick AI voice / alarm dismiss trigger
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOARD_BTN_BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    struct tm now = {0};
    wifi_time_get_local(&now);
    s_cal_year = now.tm_year + 1900;
    s_cal_month = now.tm_mon + 1;
    s_dirty = true;
    ESP_LOGI(TAG, "UI initialized (240x284 rounded-corner safe layout)");
}

void app_ui_tick(void)
{
    // 1. Hardware BOOT button handling (active-low: short press = AI/dismiss, long press 2.5s = Wi-Fi Setup)
    bool boot_pressed = (gpio_get_level(BOARD_BTN_BOOT_GPIO) == 0);
    if (boot_pressed) {
        s_boot_press_ticks++;
        if (s_boot_press_ticks >= 85 && !s_boot_long_fired) {
            s_boot_long_fired = true;
            s_portal_dismissed = false;
            web_portal_start();
            s_dirty = true;
        }
    } else if (s_prev_boot_btn) {
        if (!s_boot_long_fired) {
            if (alarm_manager_is_ringing(NULL)) {
                alarm_manager_dismiss();
            } else if (web_portal_is_active() && !s_portal_dismissed) {
                s_portal_dismissed = true;
            } else {
                s_page = PAGE_AI_VOICE;
                ai_voice_alarm_start_listening();
            }
            s_dirty = true;
        }
        s_boot_press_ticks = 0;
        s_boot_long_fired = false;
    }
    s_prev_boot_btn = boot_pressed;

    // 2. Touch & swipe handling
    touch_point_t pt = {0};
    touch_cst816d_read(&pt);
    if (pt.pressed && !s_prev_pressed) {
        s_touch_start_x = pt.x;
        s_touch_start_y = pt.y;
    } else if (!pt.pressed && s_prev_pressed) {
        if (web_portal_is_active() && !s_portal_dismissed && !alarm_manager_is_ringing(NULL)) {
            s_portal_dismissed = true;
            s_dirty = true;
        } else {
            int dx = (int)pt.x - (int)s_touch_start_x;
            if (dx > 45 && !alarm_manager_is_ringing(NULL)) {
                s_page = (ui_page_t)((s_page + PAGE_COUNT - 1) % PAGE_COUNT);
                s_dirty = true;
            } else if (dx < -45 && !alarm_manager_is_ringing(NULL)) {
                s_page = (ui_page_t)((s_page + 1) % PAGE_COUNT);
                s_dirty = true;
            } else {
                handle_tap(s_touch_start_x, s_touch_start_y);
            }
        }
    }
    s_prev_pressed = pt.pressed;

    // 3. Alarm ringing overlay state transitions
    alarm_entry_t ring_a = {0};
    bool ringing = alarm_manager_is_ringing(&ring_a);
    if (ringing != s_was_ringing) {
        s_was_ringing = ringing;
        s_dirty = true;
    }
    if (ringing) {
        if (s_dirty) {
            draw_ringing_overlay(&ring_a);
            s_dirty = false;
        }
        return;
    }

    // 3b. SoftAP Wi-Fi setup portal overlay
    bool portal_on = web_portal_is_active();
    if (portal_on != s_was_portal) {
        s_was_portal = portal_on;
        if (portal_on) {
            s_portal_dismissed = false;
        }
        s_dirty = true;
    }
    if (portal_on && !s_portal_dismissed) {
        if (s_dirty) {
            draw_portal_overlay();
            s_dirty = false;
        }
        return;
    }

    // 4. AI Voice state change triggers redraw
    ai_voice_status_t ai_st = {0};
    ai_voice_alarm_get_status(&ai_st);
    if (ai_st.state != s_last_ai_state) {
        s_last_ai_state = ai_st.state;
        if (s_page == PAGE_AI_VOICE) {
            s_dirty = true;
        }
    }

    // 5. Periodic clock & screen refresh
    struct tm now = {0};
    wifi_time_get_local(&now);
    if (s_dirty || now.tm_min != s_last_min) {
        s_last_min = now.tm_min;
        s_last_sec = now.tm_sec;
        s_dirty = false;
        switch (s_page) {
            case PAGE_HOME:     draw_page_home(&now); break;
            case PAGE_CALENDAR: draw_page_calendar(&now); break;
            case PAGE_ALARMS:   draw_page_alarms(); break;
            case PAGE_AI_VOICE: draw_page_ai_voice(); break;
            default: break;
        }
    } else if (s_page == PAGE_HOME && now.tm_sec != s_last_sec) {
        s_last_sec = now.tm_sec;
        draw_home_clock_partial(&now);
    }
}
