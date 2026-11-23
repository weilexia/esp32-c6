#include "app_ui.h"
#include "alert_server.h"
#include "app_config.h"
#include "audio_codec.h"
#include "axp2101.h"
#include "ble_ancs.h"
#include "board_pins.h"
#include "deepseek_service.h"
#include "lcd_st7789.h"
#include "lv_port.h"
#include "weather_service.h"
#include "web_portal.h"
#include "wifi_time.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_ui";

// === Usage Deck OLED Color Palette ===
#define C_BG        lv_color_hex(0x000000)
#define C_CARD      lv_color_hex(0x0D121A)
#define C_CARD_ALT  lv_color_hex(0x16202E)
#define C_BORDER    lv_color_hex(0x1E2C3E)
#define C_TRACK     lv_color_hex(0x192430)
#define C_GREEN     lv_color_hex(0x4ADE80)
#define C_CYAN      lv_color_hex(0x38BDF8)
#define C_ORANGE    lv_color_hex(0xFB923C)
#define C_AMBER     lv_color_hex(0xF59E0B)
#define C_RED       lv_color_hex(0xF43F5E)
#define C_PURPLE    lv_color_hex(0xA855F7)
#define C_MUTED     lv_color_hex(0x7391B2)
#define C_WHITE     lv_color_hex(0xF8FAFC)

#define MAIN_PAGE_COUNT        4
#define DIM_BACKLIGHT_PCT      5
#define DAILY_TOKEN_QUOTA      50000000.0f // Daily token budget: 50,000,000 (5000万) tokens

// === Brightness & Power Saving State ===
static uint8_t s_brightness = 80;
static uint16_t s_auto_dim_sec = 60;
static int64_t s_last_activity_ms = 0;
static bool s_is_dimmed = false;

// === Hardware BOOT Button State ===
static bool s_prev_boot_btn = false;
static int s_boot_press_ticks = 0;
static bool s_boot_long_fired = false;

// === Overlay & Service Sync Tracking ===
static bool s_was_alert = false;
static uint32_t s_last_alert_seq = 0;
static uint32_t s_last_wx_seq = 0;
static int64_t s_wx_hide_at_ms = 0;
static bool s_was_portal = false;
static int s_active_page = 0;
static int64_t s_last_data_sync_ms = 0;

// === LVGL Widget Handles ===
static lv_obj_t *s_hdr_wifi_lbl = NULL;
static lv_obj_t *s_hdr_mute_lbl = NULL;
static lv_obj_t *s_tileview = NULL;
static lv_obj_t *s_dots[MAIN_PAGE_COUNT] = {0};
static lv_obj_t *s_bot_time_lbl = NULL;

// Tile 0: OVERVIEW
static lv_obj_t *s_ov_arc = NULL;
static lv_obj_t *s_ov_ring_num = NULL;
static lv_obj_t *s_ov_ring_pct = NULL;
static lv_obj_t *s_ov_ring_lbl = NULL;
static lv_obj_t *s_ov_tok_lbl = NULL;
static lv_obj_t *s_ov_tok_val = NULL;
static lv_obj_t *s_ov_bal_lbl = NULL;
static lv_obj_t *s_ov_bal_val = NULL;
static lv_obj_t *s_ov_cost_val = NULL;
static lv_obj_t *s_ov_meta_lbl = NULL;

// Tile 1: TOKENS
static lv_obj_t *s_tk_sync_lbl = NULL;
static lv_obj_t *s_tk_bal_val = NULL;
static lv_obj_t *s_tk_bal_sub = NULL;
static lv_obj_t *s_tk_prompt_val = NULL;
static lv_obj_t *s_tk_comp_val = NULL;
static lv_obj_t *s_tk_calls_val = NULL;
static lv_obj_t *s_tk_err_val = NULL;

// Tile 2: ALERTS
static lv_obj_t *s_al_clock_lbl = NULL;
static lv_obj_t *s_al_wx_lbl = NULL;
static lv_obj_t *s_al_city_lbl = NULL;
static lv_obj_t *s_al_empty_card = NULL;
static lv_obj_t *s_al_rows[3] = {0};
static lv_obj_t *s_al_row_bar[3] = {0};
static lv_obj_t *s_al_row_time[3] = {0};
static lv_obj_t *s_al_row_ack[3] = {0};
static lv_obj_t *s_al_row_title[3] = {0};
static alert_record_t s_al_cache[3] = {0};
static int s_al_cache_cnt = 0;

// Tile 3: SYSTEM
static lv_obj_t *s_sys_wifi_lbl = NULL;
static lv_obj_t *s_sys_ip_lbl = NULL;
static lv_obj_t *s_sys_bat_lbl = NULL;
static lv_obj_t *s_sys_key_lbl = NULL;
static lv_obj_t *s_sys_ble_lbl = NULL;
static lv_obj_t *s_sys_bl_lbl = NULL;
static lv_obj_t *s_sys_mute_btn = NULL;
static lv_obj_t *s_sys_mute_lbl = NULL;
static lv_obj_t *s_sys_dim_lbl = NULL;
static lv_obj_t *s_sys_slider = NULL;

// Overlays (on lv_layer_top)
static lv_obj_t *s_ovl_alert = NULL;
static lv_obj_t *s_ovl_alert_hdr = NULL;
static lv_obj_t *s_ovl_alert_hdr_lbl = NULL;
static lv_obj_t *s_ovl_alert_card = NULL;
static lv_obj_t *s_ovl_alert_time = NULL;
static lv_obj_t *s_ovl_alert_title = NULL;
static lv_obj_t *s_ovl_alert_msg = NULL;

static lv_obj_t *s_ovl_wechat = NULL;
static lv_obj_t *s_ovl_wx_title = NULL;
static lv_obj_t *s_ovl_wx_msg = NULL;

static lv_obj_t *s_ovl_portal = NULL;
static lv_obj_t *s_ovl_portal_ssid = NULL;
static lv_obj_t *s_ovl_portal_url = NULL;

static lv_obj_t *s_ovl_detail = NULL;
static lv_obj_t *s_ovl_det_badge = NULL;
static lv_obj_t *s_ovl_det_badge_lbl = NULL;
static lv_obj_t *s_ovl_det_time = NULL;
static lv_obj_t *s_ovl_det_title = NULL;
static lv_obj_t *s_ovl_det_msg = NULL;

// === Helpers ===
static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

static bool wake_backlight_if_dimmed(void)
{
    s_last_activity_ms = now_ms();
    if (s_is_dimmed) {
        s_is_dimmed = false;
        lcd_st7789_set_backlight(s_brightness);
        return true;
    }
    return false;
}

static void force_wake_backlight(void)
{
    s_last_activity_ms = now_ms();
    if (s_is_dimmed) {
        s_is_dimmed = false;
        lcd_st7789_set_backlight(s_brightness);
    }
}

static lv_color_t level_lv_color(alert_level_t lvl)
{
    if (lvl == ALERT_LEVEL_ERROR) return C_RED;
    if (lvl == ALERT_LEVEL_WARN)  return C_AMBER;
    return C_CYAN;
}

static const char *level_name(alert_level_t lvl)
{
    if (lvl == ALERT_LEVEL_ERROR) return "严重告警";
    if (lvl == ALERT_LEVEL_WARN)  return "系统预警";
    return "系统通知";
}

static void style_clean_container(lv_obj_t *obj)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *create_deck_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, C_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, C_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static void update_pagination_dots(int page)
{
    s_active_page = page;
    for (int i = 0; i < MAIN_PAGE_COUNT; i++) {
        if (!s_dots[i]) continue;
        if (i == page) {
            lv_obj_set_size(s_dots[i], 12, 6);
            lv_obj_set_style_bg_color(s_dots[i], C_GREEN, 0);
        } else {
            lv_obj_set_size(s_dots[i], 6, 6);
            lv_obj_set_style_bg_color(s_dots[i], C_BORDER, 0);
        }
    }
}

static void update_dim_button_label(void)
{
    if (!s_sys_dim_lbl) return;
    char dtxt[20];
    if (s_auto_dim_sec == 0) snprintf(dtxt, sizeof(dtxt), "常亮模式");
    else if (s_auto_dim_sec >= 120) snprintf(dtxt, sizeof(dtxt), "省电:%um", (unsigned)(s_auto_dim_sec / 60));
    else snprintf(dtxt, sizeof(dtxt), "省电:%us", (unsigned)s_auto_dim_sec);
    lv_label_set_text(s_sys_dim_lbl, dtxt);
    lv_obj_set_style_text_color(s_sys_dim_lbl, s_auto_dim_sec > 0 ? C_GREEN : C_MUTED, 0);
}

static void open_hotspot_portal(void)
{
    force_wake_backlight();
    web_portal_start();
    if (s_ovl_portal_ssid) lv_label_set_text(s_ovl_portal_ssid, web_portal_get_ap_ssid());
    if (s_ovl_portal_url) {
        char url[32];
        snprintf(url, sizeof(url), "http://%s", web_portal_get_ap_ip());
        lv_label_set_text(s_ovl_portal_url, url);
    }
    lv_obj_clear_flag(s_ovl_portal, LV_OBJ_FLAG_HIDDEN);
}

// === LVGL Event Callbacks ===
static void on_tile_changed(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    lv_obj_t *act = lv_tileview_get_tile_active(tv);
    if (!act) return;
    int col = (int)lv_obj_get_x(act) / BOARD_LCD_WIDTH;
    if (col < 0) col = 0;
    if (col >= MAIN_PAGE_COUNT) col = MAIN_PAGE_COUNT - 1;
    update_pagination_dots(col);
}

static void on_home_clicked(lv_event_t *e)
{
    (void)e;
    lv_tileview_set_tile_by_index(s_tileview, 0, 0, LV_ANIM_ON);
    update_pagination_dots(0);
}

static void on_setup_clicked(lv_event_t *e)
{
    (void)e;
    open_hotspot_portal();
}

static void on_refresh_deepseek_clicked(lv_event_t *e)
{
    (void)e;
    deepseek_service_request_refresh();
}

static void on_refresh_weather_clicked(lv_event_t *e)
{
    (void)e;
    weather_service_request_refresh();
}

static void on_brightness_changed(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int val = (int)lv_slider_get_value(slider);
    if (val < 10) val = 10;
    if (val > 100) val = 100;
    s_brightness = (uint8_t)val;
    s_is_dimmed = false;
    s_last_activity_ms = now_ms();
    lcd_st7789_set_backlight(s_brightness);

    char btxt[24];
    snprintf(btxt, sizeof(btxt), "亮度 %u%%", (unsigned)s_brightness);
    lv_label_set_text(s_sys_bl_lbl, btxt);

    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        app_config_set_display(s_brightness, s_auto_dim_sec);
    }
}

static void update_mute_ui(void)
{
    bool muted = audio_is_muted();
    if (s_hdr_mute_lbl) {
        if (muted) lv_obj_clear_flag(s_hdr_mute_lbl, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_hdr_mute_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_sys_mute_lbl && s_sys_mute_btn) {
        lv_label_set_text(s_sys_mute_lbl, muted ? "已静音" : "声音开");
        lv_obj_set_style_text_color(s_sys_mute_lbl, muted ? C_ORANGE : C_GREEN, 0);
        lv_obj_set_style_border_color(s_sys_mute_btn, muted ? C_ORANGE : C_BORDER, 0);
    }
}

static void on_mute_toggle_clicked(lv_event_t *e)
{
    (void)e;
    bool next = !audio_is_muted();
    audio_set_mute(next);
    app_config_set_mute(next);
    update_mute_ui();
}

static void on_dim_toggle_clicked(lv_event_t *e)
{
    (void)e;
    if (s_auto_dim_sec == 30) s_auto_dim_sec = 60;
    else if (s_auto_dim_sec == 60) s_auto_dim_sec = 120;
    else if (s_auto_dim_sec == 120) s_auto_dim_sec = 0;
    else s_auto_dim_sec = 30;
    app_config_set_display(s_brightness, s_auto_dim_sec);
    update_dim_button_label();
}

static void on_alert_row_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_al_cache_cnt) return;
    const alert_record_t *rec = &s_al_cache[idx];

    lv_color_t col = level_lv_color(rec->level);
    lv_obj_set_style_bg_color(s_ovl_det_badge, col, 0);
    lv_label_set_text(s_ovl_det_badge_lbl, level_name(rec->level));
    lv_obj_set_style_text_color(s_ovl_det_badge_lbl, rec->level == ALERT_LEVEL_WARN ? C_BG : C_WHITE, 0);
    lv_label_set_text(s_ovl_det_time, rec->time_str);
    lv_label_set_text(s_ovl_det_title, rec->title);
    lv_label_set_text(s_ovl_det_msg, rec->message);
    lv_obj_clear_flag(s_ovl_detail, LV_OBJ_FLAG_HIDDEN);
}

static void on_cancel_alert_clicked(lv_event_t *e)
{
    (void)e;
    alert_server_stop();
    lv_obj_add_flag(s_ovl_alert, LV_OBJ_FLAG_HIDDEN);
}

static void on_close_wechat_clicked(lv_event_t *e)
{
    (void)e;
    s_wx_hide_at_ms = 0;
    lv_obj_add_flag(s_ovl_wechat, LV_OBJ_FLAG_HIDDEN);
}

static void on_close_portal_clicked(lv_event_t *e)
{
    (void)e;
    web_portal_stop();
    lv_obj_add_flag(s_ovl_portal, LV_OBJ_FLAG_HIDDEN);
}

static void on_close_detail_clicked(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_ovl_detail, LV_OBJ_FLAG_HIDDEN);
}

// === Build Main Usage Deck UI ===
static void build_header(lv_obj_t *scr)
{
    // Neon-green "DS" badge
    lv_obj_t *badge = lv_obj_create(scr);
    lv_obj_set_pos(badge, 10, 8);
    lv_obj_set_size(badge, 24, 24);
    lv_obj_set_style_bg_color(badge, C_GREEN, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_radius(badge, 6, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ds_lbl = lv_label_create(badge);
    lv_label_set_text(ds_lbl, "DS");
    lv_obj_set_style_text_font(ds_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ds_lbl, C_BG, 0);
    lv_obj_center(ds_lbl);

    // Brand Title + Subtitle
    lv_obj_t *t1 = lv_label_create(scr);
    lv_label_set_text(t1, "DEEPSEEK");
    lv_obj_set_style_text_font(t1, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(t1, C_GREEN, 0);
    lv_obj_set_pos(t1, 40, 7);

    lv_obj_t *t2 = lv_label_create(scr);
    lv_label_set_text(t2, "USAGE DECK");
    lv_obj_set_style_text_font(t2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(t2, C_MUTED, 0);
    lv_obj_set_pos(t2, 40, 20);

    // Mute indicator icon (shown when globally muted)
    s_hdr_mute_lbl = lv_label_create(scr);
    lv_label_set_text(s_hdr_mute_lbl, LV_SYMBOL_MUTE);
    lv_obj_set_style_text_font(s_hdr_mute_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_hdr_mute_lbl, C_ORANGE, 0);
    lv_obj_align(s_hdr_mute_lbl, LV_ALIGN_TOP_RIGHT, -84, 12);
    lv_obj_add_flag(s_hdr_mute_lbl, LV_OBJ_FLAG_HIDDEN);

    // Right Wi-Fi status
    s_hdr_wifi_lbl = lv_label_create(scr);
    lv_label_set_text(s_hdr_wifi_lbl, LV_SYMBOL_WIFI " OFFLINE");
    lv_obj_set_style_text_font(s_hdr_wifi_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_hdr_wifi_lbl, C_RED, 0);
    lv_obj_align(s_hdr_wifi_lbl, LV_ALIGN_TOP_RIGHT, -10, 12);
}

static void build_tile_overview(lv_obj_t *tile)
{
    // Sub-header
    lv_obj_t *sub_l = lv_label_create(tile);
    lv_label_set_text(sub_l, "OVERVIEW");
    lv_obj_set_style_text_font(sub_l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub_l, C_GREEN, 0);
    lv_obj_set_pos(sub_l, 10, 2);

    lv_obj_t *sub_r = lv_label_create(tile);
    lv_label_set_text(sub_r, "TODAY");
    lv_obj_set_style_text_font(sub_r, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub_r, C_MUTED, 0);
    lv_obj_align(sub_r, LV_ALIGN_TOP_RIGHT, -10, 2);

    // Left Circular Arc (112x112, rounded neon-green indicator)
    s_ov_arc = lv_arc_create(tile);
    lv_obj_set_pos(s_ov_arc, 8, 28);
    lv_obj_set_size(s_ov_arc, 112, 112);
    lv_arc_set_rotation(s_ov_arc, 270);
    lv_arc_set_bg_angles(s_ov_arc, 0, 360);
    lv_arc_set_range(s_ov_arc, 0, 1000);
    lv_arc_set_value(s_ov_arc, 0);
    lv_obj_remove_style(s_ov_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_ov_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ov_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ov_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ov_arc, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ov_arc, C_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_ov_arc, true, LV_PART_INDICATOR);

    // Center text inside Arc
    s_ov_ring_num = lv_label_create(s_ov_arc);
    lv_label_set_text(s_ov_ring_num, "--");
    lv_obj_set_style_text_font(s_ov_ring_num, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_ov_ring_num, C_WHITE, 0);
    lv_obj_align(s_ov_ring_num, LV_ALIGN_CENTER, 0, -10);

    s_ov_ring_pct = lv_label_create(s_ov_arc);
    lv_label_set_text(s_ov_ring_pct, "%");
    lv_obj_set_style_text_font(s_ov_ring_pct, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_ov_ring_pct, C_GREEN, 0);
    lv_obj_align(s_ov_ring_pct, LV_ALIGN_CENTER, 0, 14);

    s_ov_ring_lbl = lv_label_create(s_ov_arc);
    lv_label_set_text(s_ov_ring_lbl, "WAIT");
    lv_obj_set_style_text_font(s_ov_ring_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_ov_ring_lbl, C_MUTED, 0);
    lv_obj_align(s_ov_ring_lbl, LV_ALIGN_CENTER, 0, 29);

    // Right 3 Stacked Dark Cards
    lv_obj_t *c1 = create_deck_card(tile, 128, 20, 102, 48);
    lv_obj_add_event_cb(c1, on_refresh_deepseek_clicked, LV_EVENT_CLICKED, NULL);
    s_ov_tok_lbl = lv_label_create(c1);
    lv_label_set_text(s_ov_tok_lbl, "TOKENS");
    lv_obj_set_style_text_font(s_ov_tok_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_ov_tok_lbl, C_MUTED, 0);
    lv_obj_set_pos(s_ov_tok_lbl, 8, 5);
    s_ov_tok_val = lv_label_create(c1);
    lv_label_set_text(s_ov_tok_val, "0");
    lv_obj_set_style_text_font(s_ov_tok_val, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_ov_tok_val, C_WHITE, 0);
    lv_obj_set_pos(s_ov_tok_val, 8, 21);

    lv_obj_t *c2 = create_deck_card(tile, 128, 74, 102, 48);
    lv_obj_add_event_cb(c2, on_refresh_deepseek_clicked, LV_EVENT_CLICKED, NULL);
    s_ov_bal_lbl = lv_label_create(c2);
    lv_label_set_text(s_ov_bal_lbl, "BAL (CNY)");
    lv_obj_set_style_text_font(s_ov_bal_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_ov_bal_lbl, C_MUTED, 0);
    lv_obj_set_pos(s_ov_bal_lbl, 8, 5);
    s_ov_bal_val = lv_label_create(c2);
    lv_label_set_text(s_ov_bal_val, "--");
    lv_obj_set_style_text_font(s_ov_bal_val, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_ov_bal_val, C_CYAN, 0);
    lv_obj_set_pos(s_ov_bal_val, 8, 21);

    lv_obj_t *c3 = create_deck_card(tile, 128, 128, 102, 48);
    lv_obj_add_event_cb(c3, on_refresh_deepseek_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *c3_lbl = lv_label_create(c3);
    lv_label_set_text(c3_lbl, "TODAY (RMB)");
    lv_obj_set_style_text_font(c3_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(c3_lbl, C_MUTED, 0);
    lv_obj_set_pos(c3_lbl, 8, 5);
    s_ov_cost_val = lv_label_create(c3);
    lv_label_set_text(s_ov_cost_val, "0.00");
    lv_obj_set_style_text_font(s_ov_cost_val, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_ov_cost_val, C_WHITE, 0);
    lv_obj_set_pos(s_ov_cost_val, 8, 21);

    // Bottom meta strip
    s_ov_meta_lbl = lv_label_create(tile);
    lv_label_set_text(s_ov_meta_lbl, "CALL:0  ERR:0  BAT --");
    lv_obj_set_style_text_font(s_ov_meta_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_ov_meta_lbl, C_MUTED, 0);
    lv_obj_align(s_ov_meta_lbl, LV_ALIGN_BOTTOM_MID, 0, -6);
}

static void build_tile_tokens(lv_obj_t *tile)
{
    lv_obj_t *sub_l = lv_label_create(tile);
    lv_label_set_text(sub_l, "TOKENS");
    lv_obj_set_style_text_font(sub_l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub_l, C_GREEN, 0);
    lv_obj_set_pos(sub_l, 10, 2);

    s_tk_sync_lbl = lv_label_create(tile);
    lv_label_set_text(s_tk_sync_lbl, "SYNC");
    lv_obj_set_style_text_font(s_tk_sync_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_tk_sync_lbl, C_MUTED, 0);
    lv_obj_align(s_tk_sync_lbl, LV_ALIGN_TOP_RIGHT, -10, 2);

    // Top Balance Hero Card
    lv_obj_t *bal_card = create_deck_card(tile, 10, 20, 220, 52);
    lv_obj_add_event_cb(bal_card, on_refresh_deepseek_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(bal_card);
    lv_label_set_text(bl, "DEEPSEEK BALANCE");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bl, C_MUTED, 0);
    lv_obj_set_pos(bl, 10, 6);

    s_tk_bal_val = lv_label_create(bal_card);
    lv_label_set_text(s_tk_bal_val, "未配置 API Key");
    lv_obj_set_style_text_font(s_tk_bal_val, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(s_tk_bal_val, C_AMBER, 0);
    lv_obj_set_pos(s_tk_bal_val, 10, 24);

    s_tk_bal_sub = lv_label_create(bal_card);
    lv_label_set_text(s_tk_bal_sub, "");
    lv_obj_set_style_text_font(s_tk_bal_sub, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(s_tk_bal_sub, C_MUTED, 0);
    lv_obj_align(s_tk_bal_sub, LV_ALIGN_BOTTOM_RIGHT, -10, -8);

    // 2x2 Grid
    const char *lbls[4] = {"PROMPT", "COMPLETION", "API CALLS", "ERRORS"};
    lv_color_t cols[4] = {C_CYAN, C_PURPLE, C_WHITE, C_GREEN};
    lv_obj_t **ptrs[4] = {&s_tk_prompt_val, &s_tk_comp_val, &s_tk_calls_val, &s_tk_err_val};
    const int gx[2] = {10, 124};
    const int gy[2] = {78, 134};

    for (int i = 0; i < 4; i++) {
        lv_obj_t *c = create_deck_card(tile, gx[i % 2], gy[i / 2], 106, 50);
        lv_obj_add_event_cb(c, on_refresh_deepseek_clicked, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(c);
        lv_label_set_text(l, lbls[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, C_MUTED, 0);
        lv_obj_set_pos(l, 8, 6);

        *ptrs[i] = lv_label_create(c);
        lv_label_set_text(*ptrs[i], "0");
        lv_obj_set_style_text_font(*ptrs[i], &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(*ptrs[i], cols[i], 0);
        lv_obj_set_pos(*ptrs[i], 8, 22);
    }

    lv_obj_t *hint = lv_label_create(tile);
    lv_label_set_text(hint, LV_SYMBOL_REFRESH " TAP CARD TO REFRESH");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, C_MUTED, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void build_tile_alerts(lv_obj_t *tile)
{
    lv_obj_t *sub_l = lv_label_create(tile);
    lv_label_set_text(sub_l, "ALERTS");
    lv_obj_set_style_text_font(sub_l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub_l, C_GREEN, 0);
    lv_obj_set_pos(sub_l, 10, 2);

    lv_obj_t *sub_r = lv_label_create(tile);
    lv_label_set_text(sub_r, "MONITOR");
    lv_obj_set_style_text_font(sub_r, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub_r, C_MUTED, 0);
    lv_obj_align(sub_r, LV_ALIGN_TOP_RIGHT, -10, 2);

    // Clock + Weather Hero Card
    lv_obj_t *hero = create_deck_card(tile, 10, 20, 220, 52);
    lv_obj_add_event_cb(hero, on_refresh_weather_clicked, LV_EVENT_CLICKED, NULL);

    s_al_clock_lbl = lv_label_create(hero);
    lv_label_set_text(s_al_clock_lbl, "00:00");
    lv_obj_set_style_text_font(s_al_clock_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_al_clock_lbl, C_WHITE, 0);
    lv_obj_align(s_al_clock_lbl, LV_ALIGN_LEFT_MID, 12, 0);

    s_al_wx_lbl = lv_label_create(hero);
    lv_label_set_text(s_al_wx_lbl, "天气离线");
    lv_obj_set_style_text_font(s_al_wx_lbl, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(s_al_wx_lbl, C_AMBER, 0);
    lv_obj_align(s_al_wx_lbl, LV_ALIGN_TOP_RIGHT, -10, 7);

    s_al_city_lbl = lv_label_create(hero);
    lv_label_set_text(s_al_city_lbl, "");
    lv_obj_set_style_text_font(s_al_city_lbl, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(s_al_city_lbl, C_MUTED, 0);
    lv_obj_align(s_al_city_lbl, LV_ALIGN_BOTTOM_RIGHT, -10, -7);

    // Empty State Card
    s_al_empty_card = create_deck_card(tile, 10, 78, 220, 122);
    lv_obj_t *ok1 = lv_label_create(s_al_empty_card);
    lv_label_set_text(ok1, "系统运行正常");
    lv_obj_set_style_text_font(ok1, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(ok1, C_GREEN, 0);
    lv_obj_align(ok1, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *ok2 = lv_label_create(s_al_empty_card);
    lv_label_set_text(ok2, "暂无告警记录");
    lv_obj_set_style_text_font(ok2, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(ok2, C_MUTED, 0);
    lv_obj_align(ok2, LV_ALIGN_CENTER, 0, 12);

    // 3 Alert Rows (hidden when empty)
    for (int i = 0; i < 3; i++) {
        s_al_rows[i] = create_deck_card(tile, 10, 78 + i * 42, 220, 38);
        lv_obj_add_flag(s_al_rows[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(s_al_rows[i], on_alert_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        s_al_row_bar[i] = lv_obj_create(s_al_rows[i]);
        lv_obj_set_pos(s_al_row_bar[i], 6, 6);
        lv_obj_set_size(s_al_row_bar[i], 4, 24);
        lv_obj_set_style_bg_color(s_al_row_bar[i], C_RED, 0);
        lv_obj_set_style_bg_opa(s_al_row_bar[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_al_row_bar[i], 0, 0);
        lv_obj_set_style_radius(s_al_row_bar[i], 2, 0);

        s_al_row_time[i] = lv_label_create(s_al_rows[i]);
        lv_obj_set_style_text_font(s_al_row_time[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(s_al_row_time[i], C_MUTED, 0);
        lv_obj_set_pos(s_al_row_time[i], 16, 3);

        s_al_row_ack[i] = lv_label_create(s_al_rows[i]);
        lv_obj_set_style_text_font(s_al_row_ack[i], &g_lv_font_cn16, 0);
        lv_obj_align(s_al_row_ack[i], LV_ALIGN_TOP_RIGHT, -8, 1);

        s_al_row_title[i] = lv_label_create(s_al_rows[i]);
        lv_obj_set_style_text_font(s_al_row_title[i], &g_lv_font_cn16, 0);
        lv_obj_set_style_text_color(s_al_row_title[i], C_WHITE, 0);
        lv_obj_set_pos(s_al_row_title[i], 16, 18);
        lv_obj_set_width(s_al_row_title[i], 194);
        lv_label_set_long_mode(s_al_row_title[i], LV_LABEL_LONG_CLIP);
    }
}

static void build_tile_system(lv_obj_t *tile)
{
    lv_obj_t *sub_l = lv_label_create(tile);
    lv_label_set_text(sub_l, "SYSTEM");
    lv_obj_set_style_text_font(sub_l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub_l, C_GREEN, 0);
    lv_obj_set_pos(sub_l, 10, 2);

    lv_obj_t *sub_r = lv_label_create(tile);
    lv_label_set_text(sub_r, "POWER");
    lv_obj_set_style_text_font(sub_r, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub_r, C_MUTED, 0);
    lv_obj_align(sub_r, LV_ALIGN_TOP_RIGHT, -10, 2);

    // Device Status Card
    lv_obj_t *info = create_deck_card(tile, 10, 18, 220, 64);

    s_sys_wifi_lbl = lv_label_create(info);
    lv_obj_set_style_text_font(s_sys_wifi_lbl, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(s_sys_wifi_lbl, C_WHITE, 0);
    lv_obj_set_pos(s_sys_wifi_lbl, 8, 4);
    lv_obj_set_width(s_sys_wifi_lbl, 204);
    lv_label_set_long_mode(s_sys_wifi_lbl, LV_LABEL_LONG_CLIP);

    s_sys_ip_lbl = lv_label_create(info);
    lv_obj_set_style_text_font(s_sys_ip_lbl, &g_lv_font_cn16, 0);
    lv_obj_set_pos(s_sys_ip_lbl, 8, 23);

    s_sys_bat_lbl = lv_label_create(info);
    lv_obj_set_style_text_font(s_sys_bat_lbl, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(s_sys_bat_lbl, C_MUTED, 0);
    lv_obj_align(s_sys_bat_lbl, LV_ALIGN_RIGHT_MID, -8, 0);

    s_sys_key_lbl = lv_label_create(info);
    lv_obj_set_style_text_font(s_sys_key_lbl, &g_lv_font_cn16, 0);
    lv_obj_set_pos(s_sys_key_lbl, 8, 42);

    s_sys_ble_lbl = lv_label_create(info);
    lv_obj_set_style_text_font(s_sys_ble_lbl, &g_lv_font_cn16, 0);
    lv_obj_align(s_sys_ble_lbl, LV_ALIGN_BOTTOM_RIGHT, -8, -4);

    // Brightness, Mute & Auto-Dim Power Saving Card (Native LVGL Slider!)
    lv_obj_t *pwr = create_deck_card(tile, 10, 88, 220, 68);

    s_sys_bl_lbl = lv_label_create(pwr);
    lv_obj_set_style_text_font(s_sys_bl_lbl, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(s_sys_bl_lbl, C_WHITE, 0);
    lv_obj_set_pos(s_sys_bl_lbl, 8, 8);

    s_sys_mute_btn = lv_button_create(pwr);
    lv_obj_set_size(s_sys_mute_btn, 58, 24);
    lv_obj_set_pos(s_sys_mute_btn, 84, 6);
    lv_obj_set_ext_click_area(s_sys_mute_btn, 10);
    lv_obj_set_style_bg_color(s_sys_mute_btn, C_CARD_ALT, 0);
    lv_obj_set_style_bg_color(s_sys_mute_btn, C_BORDER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(s_sys_mute_btn, C_BORDER, 0);
    lv_obj_set_style_border_width(s_sys_mute_btn, 1, 0);
    lv_obj_set_style_radius(s_sys_mute_btn, 7, 0);
    lv_obj_set_style_shadow_width(s_sys_mute_btn, 0, 0);
    lv_obj_add_event_cb(s_sys_mute_btn, on_mute_toggle_clicked, LV_EVENT_CLICKED, NULL);

    s_sys_mute_lbl = lv_label_create(s_sys_mute_btn);
    lv_obj_set_style_text_font(s_sys_mute_lbl, &g_lv_font_cn16, 0);
    lv_obj_center(s_sys_mute_lbl);

    lv_obj_t *dim_btn = lv_button_create(pwr);
    lv_obj_set_size(dim_btn, 68, 24);
    lv_obj_align(dim_btn, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_ext_click_area(dim_btn, 10);
    lv_obj_set_style_bg_color(dim_btn, C_CARD_ALT, 0);
    lv_obj_set_style_bg_color(dim_btn, C_BORDER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(dim_btn, C_BORDER, 0);
    lv_obj_set_style_border_width(dim_btn, 1, 0);
    lv_obj_set_style_radius(dim_btn, 7, 0);
    lv_obj_set_style_shadow_width(dim_btn, 0, 0);
    lv_obj_add_event_cb(dim_btn, on_dim_toggle_clicked, LV_EVENT_CLICKED, NULL);

    s_sys_dim_lbl = lv_label_create(dim_btn);
    lv_obj_set_style_text_font(s_sys_dim_lbl, &g_lv_font_cn16, 0);
    lv_obj_center(s_sys_dim_lbl);

    s_sys_slider = lv_slider_create(pwr);
    lv_obj_set_size(s_sys_slider, 192, 10);
    lv_obj_align(s_sys_slider, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_ext_click_area(s_sys_slider, 14);
    lv_obj_remove_flag(s_sys_slider, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_slider_set_range(s_sys_slider, 10, 100);
    lv_slider_set_value(s_sys_slider, s_brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_sys_slider, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_sys_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sys_slider, C_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_sys_slider, C_WHITE, LV_PART_KNOB);
    lv_obj_set_style_border_color(s_sys_slider, C_GREEN, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_sys_slider, 2, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_sys_slider, 5, LV_PART_KNOB);
    lv_obj_add_event_cb(s_sys_slider, on_brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_sys_slider, on_brightness_changed, LV_EVENT_RELEASED, NULL);

    // One-Tap Hotspot Button
    lv_obj_t *ap_btn = lv_button_create(tile);
    lv_obj_set_pos(ap_btn, 10, 162);
    lv_obj_set_size(ap_btn, 220, 38);
    lv_obj_set_ext_click_area(ap_btn, 8);
    lv_obj_set_style_bg_color(ap_btn, C_GREEN, 0);
    lv_obj_set_style_bg_opa(ap_btn, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_radius(ap_btn, 10, 0);
    lv_obj_set_style_shadow_width(ap_btn, 0, 0);
    lv_obj_add_event_cb(ap_btn, on_setup_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ap_lbl = lv_label_create(ap_btn);
    lv_label_set_text(ap_lbl, "开启配网热点 (192.168.1.4)");
    lv_obj_set_style_text_font(ap_lbl, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(ap_lbl, C_BG, 0);
    lv_obj_center(ap_lbl);
}

static void build_bottom_bar(lv_obj_t *scr)
{
    lv_obj_t *bar = lv_obj_create(scr);
    style_clean_container(bar);
    lv_obj_set_pos(bar, 0, 248);
    lv_obj_set_size(bar, 240, 36);

    // HOME pill button (with generous 12px invisible hit-area expansion)
    lv_obj_t *home_btn = lv_button_create(bar);
    lv_obj_set_pos(home_btn, 8, 4);
    lv_obj_set_size(home_btn, 48, 26);
    lv_obj_set_ext_click_area(home_btn, 12);
    lv_obj_set_style_bg_color(home_btn, C_GREEN, 0);
    lv_obj_set_style_bg_opa(home_btn, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_radius(home_btn, 9, 0);
    lv_obj_set_style_shadow_width(home_btn, 0, 0);
    lv_obj_add_event_cb(home_btn, on_home_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *home_lbl = lv_label_create(home_btn);
    lv_label_set_text(home_lbl, "HOME");
    lv_obj_set_style_text_font(home_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(home_lbl, C_BG, 0);
    lv_obj_center(home_lbl);

    // Time HH:MM
    s_bot_time_lbl = lv_label_create(bar);
    lv_label_set_text(s_bot_time_lbl, "00:00");
    lv_obj_set_style_text_font(s_bot_time_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_bot_time_lbl, C_MUTED, 0);
    lv_obj_set_pos(s_bot_time_lbl, 62, 10);

    // 4 Pagination Dots
    lv_obj_t *dot_row = lv_obj_create(bar);
    style_clean_container(dot_row);
    lv_obj_set_size(dot_row, 54, 14);
    lv_obj_set_pos(dot_row, 104, 10);
    lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dot_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dot_row, 5, 0);

    for (int i = 0; i < MAIN_PAGE_COUNT; i++) {
        s_dots[i] = lv_obj_create(dot_row);
        lv_obj_set_style_border_width(s_dots[i], 0, 0);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_dots[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    update_pagination_dots(0);

    // Crisp "设置" Hotspot Pill Button (with 12px invisible hit-area expansion)
    lv_obj_t *set_btn = lv_button_create(bar);
    lv_obj_set_pos(set_btn, 168, 4);
    lv_obj_set_size(set_btn, 64, 26);
    lv_obj_set_ext_click_area(set_btn, 12);
    lv_obj_set_style_bg_color(set_btn, C_CARD, 0);
    lv_obj_set_style_bg_color(set_btn, C_CARD_ALT, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(set_btn, C_GREEN, 0);
    lv_obj_set_style_border_width(set_btn, 1, 0);
    lv_obj_set_style_radius(set_btn, 9, 0);
    lv_obj_set_style_shadow_width(set_btn, 0, 0);
    lv_obj_add_event_cb(set_btn, on_setup_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *set_lbl = lv_label_create(set_btn);
    lv_label_set_text(set_lbl, "设置");
    lv_obj_set_style_text_font(set_lbl, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(set_lbl, C_GREEN, 0);
    lv_obj_center(set_lbl);
}

static lv_obj_t *create_fullscreen_overlay(void)
{
    lv_obj_t *ovl = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(ovl, 0, 0);
    lv_obj_set_size(ovl, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT);
    lv_obj_set_style_bg_color(ovl, C_BG, 0);
    lv_obj_set_style_bg_opa(ovl, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ovl, 0, 0);
    lv_obj_set_style_radius(ovl, 0, 0);
    lv_obj_set_style_pad_all(ovl, 0, 0);
    lv_obj_clear_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl, LV_OBJ_FLAG_HIDDEN);
    return ovl;
}

static void build_overlays(void)
{
    // 1. Alert Detail Overlay
    s_ovl_detail = create_fullscreen_overlay();
    s_ovl_det_badge = lv_obj_create(s_ovl_detail);
    lv_obj_set_pos(s_ovl_det_badge, 12, 14);
    lv_obj_set_size(s_ovl_det_badge, 96, 28);
    lv_obj_set_style_border_width(s_ovl_det_badge, 0, 0);
    lv_obj_set_style_radius(s_ovl_det_badge, 8, 0);
    s_ovl_det_badge_lbl = lv_label_create(s_ovl_det_badge);
    lv_obj_set_style_text_font(s_ovl_det_badge_lbl, &g_lv_font_cn16, 0);
    lv_obj_center(s_ovl_det_badge_lbl);

    s_ovl_det_time = lv_label_create(s_ovl_detail);
    lv_obj_set_style_text_font(s_ovl_det_time, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ovl_det_time, C_MUTED, 0);
    lv_obj_set_pos(s_ovl_det_time, 122, 20);

    lv_obj_t *dc = create_deck_card(s_ovl_detail, 12, 50, 216, 176);
    s_ovl_det_title = lv_label_create(dc);
    lv_obj_set_style_text_font(s_ovl_det_title, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(s_ovl_det_title, C_CYAN, 0);
    lv_obj_set_pos(s_ovl_det_title, 10, 10);
    lv_obj_set_width(s_ovl_det_title, 196);
    lv_label_set_long_mode(s_ovl_det_title, LV_LABEL_LONG_CLIP);

    s_ovl_det_msg = lv_label_create(dc);
    lv_obj_set_style_text_font(s_ovl_det_msg, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(s_ovl_det_msg, C_WHITE, 0);
    lv_obj_set_pos(s_ovl_det_msg, 10, 38);
    lv_obj_set_width(s_ovl_det_msg, 196);
    lv_label_set_long_mode(s_ovl_det_msg, LV_LABEL_LONG_WRAP);

    lv_obj_t *d_back = lv_button_create(s_ovl_detail);
    lv_obj_set_pos(d_back, 12, 236);
    lv_obj_set_size(d_back, 216, 36);
    lv_obj_set_style_bg_color(d_back, C_CARD_ALT, 0);
    lv_obj_set_style_border_color(d_back, C_BORDER, 0);
    lv_obj_set_style_border_width(d_back, 1, 0);
    lv_obj_set_style_radius(d_back, 10, 0);
    lv_obj_add_event_cb(d_back, on_close_detail_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *d_bl = lv_label_create(d_back);
    lv_label_set_text(d_bl, "返回告警列表");
    lv_obj_set_style_text_font(d_bl, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(d_bl, C_WHITE, 0);
    lv_obj_center(d_bl);

    // 2. SoftAP Hotspot Setup Overlay (192.168.1.4)
    s_ovl_portal = create_fullscreen_overlay();
    lv_obj_t *ph = lv_obj_create(s_ovl_portal);
    lv_obj_set_pos(ph, 12, 14);
    lv_obj_set_size(ph, 216, 36);
    lv_obj_set_style_bg_color(ph, C_GREEN, 0);
    lv_obj_set_style_border_width(ph, 0, 0);
    lv_obj_set_style_radius(ph, 10, 0);
    lv_obj_t *phl = lv_label_create(ph);
    lv_label_set_text(phl, "Wi-Fi 热点配网已开启");
    lv_obj_set_style_text_font(phl, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(phl, C_BG, 0);
    lv_obj_center(phl);

    lv_obj_t *pc = create_deck_card(s_ovl_portal, 12, 58, 216, 154);
    lv_obj_set_style_border_color(pc, C_GREEN, 0);

    lv_obj_t *p1 = lv_label_create(pc);
    lv_label_set_text(p1, "1. 手机连接无密码热点:");
    lv_obj_set_style_text_font(p1, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(p1, C_MUTED, 0);
    lv_obj_align(p1, LV_ALIGN_TOP_MID, 0, 12);

    s_ovl_portal_ssid = lv_label_create(pc);
    lv_label_set_text(s_ovl_portal_ssid, web_portal_get_ap_ssid());
    lv_obj_set_style_text_font(s_ovl_portal_ssid, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ovl_portal_ssid, C_CYAN, 0);
    lv_obj_align(s_ovl_portal_ssid, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t *p2 = lv_label_create(pc);
    lv_label_set_text(p2, "2. 手机浏览器访问:");
    lv_obj_set_style_text_font(p2, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(p2, C_MUTED, 0);
    lv_obj_align(p2, LV_ALIGN_TOP_MID, 0, 66);

    s_ovl_portal_url = lv_label_create(pc);
    lv_label_set_text(s_ovl_portal_url, "http://192.168.1.4");
    lv_obj_set_style_text_font(s_ovl_portal_url, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ovl_portal_url, C_AMBER, 0);
    lv_obj_align(s_ovl_portal_url, LV_ALIGN_TOP_MID, 0, 90);

    lv_obj_t *p3 = lv_label_create(pc);
    lv_label_set_text(p3, "配置 Wi-Fi 及 DeepSeek Key");
    lv_obj_set_style_text_font(p3, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(p3, C_GREEN, 0);
    lv_obj_align(p3, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_obj_t *p_cls = lv_button_create(s_ovl_portal);
    lv_obj_set_pos(p_cls, 16, 224);
    lv_obj_set_size(p_cls, 208, 40);
    lv_obj_set_style_bg_color(p_cls, C_CARD_ALT, 0);
    lv_obj_set_style_border_color(p_cls, C_BORDER, 0);
    lv_obj_set_style_border_width(p_cls, 1, 0);
    lv_obj_set_style_radius(p_cls, 10, 0);
    lv_obj_add_event_cb(p_cls, on_close_portal_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *p_cl = lv_label_create(p_cls);
    lv_label_set_text(p_cl, "关闭热点并返回");
    lv_obj_set_style_text_font(p_cl, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(p_cl, C_WHITE, 0);
    lv_obj_center(p_cl);

    // 3. iPhone WeChat Notification Overlay
    s_ovl_wechat = create_fullscreen_overlay();
    lv_obj_t *wh = lv_obj_create(s_ovl_wechat);
    lv_obj_set_pos(wh, 12, 14);
    lv_obj_set_size(wh, 216, 36);
    lv_obj_set_style_bg_color(wh, C_GREEN, 0);
    lv_obj_set_style_border_width(wh, 0, 0);
    lv_obj_set_style_radius(wh, 10, 0);
    lv_obj_t *whl = lv_label_create(wh);
    lv_label_set_text(whl, "微信消息");
    lv_obj_set_style_text_font(whl, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(whl, C_BG, 0);
    lv_obj_center(whl);

    lv_obj_t *wc = create_deck_card(s_ovl_wechat, 12, 58, 216, 166);
    s_ovl_wx_title = lv_label_create(wc);
    lv_obj_set_style_text_font(s_ovl_wx_title, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(s_ovl_wx_title, C_CYAN, 0);
    lv_obj_set_pos(s_ovl_wx_title, 10, 10);
    lv_obj_set_width(s_ovl_wx_title, 196);
    lv_label_set_long_mode(s_ovl_wx_title, LV_LABEL_LONG_CLIP);

    s_ovl_wx_msg = lv_label_create(wc);
    lv_obj_set_style_text_font(s_ovl_wx_msg, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(s_ovl_wx_msg, C_WHITE, 0);
    lv_obj_set_pos(s_ovl_wx_msg, 10, 38);
    lv_obj_set_width(s_ovl_wx_msg, 196);
    lv_label_set_long_mode(s_ovl_wx_msg, LV_LABEL_LONG_WRAP);

    lv_obj_t *w_cls = lv_button_create(s_ovl_wechat);
    lv_obj_set_pos(w_cls, 20, 234);
    lv_obj_set_size(w_cls, 200, 36);
    lv_obj_set_style_bg_color(w_cls, C_CARD_ALT, 0);
    lv_obj_set_style_border_color(w_cls, C_BORDER, 0);
    lv_obj_set_style_border_width(w_cls, 1, 0);
    lv_obj_set_style_radius(w_cls, 10, 0);
    lv_obj_add_event_cb(w_cls, on_close_wechat_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *w_cl = lv_label_create(w_cls);
    lv_label_set_text(w_cl, "点击关闭");
    lv_obj_set_style_text_font(w_cl, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(w_cl, C_WHITE, 0);
    lv_obj_center(w_cl);

    // 4. Active Voice Alert Overlay (highest priority — created last so it stays on top!)
    s_ovl_alert = create_fullscreen_overlay();
    s_ovl_alert_hdr = lv_obj_create(s_ovl_alert);
    lv_obj_set_pos(s_ovl_alert_hdr, 12, 12);
    lv_obj_set_size(s_ovl_alert_hdr, 216, 40);
    lv_obj_set_style_bg_color(s_ovl_alert_hdr, C_RED, 0);
    lv_obj_set_style_border_width(s_ovl_alert_hdr, 0, 0);
    lv_obj_set_style_radius(s_ovl_alert_hdr, 12, 0);
    s_ovl_alert_hdr_lbl = lv_label_create(s_ovl_alert_hdr);
    lv_obj_set_style_text_font(s_ovl_alert_hdr_lbl, &g_lv_font_cn16, 0);
    lv_obj_center(s_ovl_alert_hdr_lbl);

    s_ovl_alert_card = create_deck_card(s_ovl_alert, 12, 60, 216, 158);
    s_ovl_alert_time = lv_label_create(s_ovl_alert_card);
    lv_obj_set_style_text_font(s_ovl_alert_time, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_ovl_alert_time, C_MUTED, 0);
    lv_obj_set_pos(s_ovl_alert_time, 10, 8);

    s_ovl_alert_title = lv_label_create(s_ovl_alert_card);
    lv_obj_set_style_text_font(s_ovl_alert_title, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(s_ovl_alert_title, C_CYAN, 0);
    lv_obj_set_pos(s_ovl_alert_title, 10, 26);
    lv_obj_set_width(s_ovl_alert_title, 196);
    lv_label_set_long_mode(s_ovl_alert_title, LV_LABEL_LONG_CLIP);

    s_ovl_alert_msg = lv_label_create(s_ovl_alert_card);
    lv_obj_set_style_text_font(s_ovl_alert_msg, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(s_ovl_alert_msg, C_WHITE, 0);
    lv_obj_set_pos(s_ovl_alert_msg, 10, 52);
    lv_obj_set_width(s_ovl_alert_msg, 196);
    lv_label_set_long_mode(s_ovl_alert_msg, LV_LABEL_LONG_WRAP);

    lv_obj_t *a_btn = lv_button_create(s_ovl_alert);
    lv_obj_set_pos(a_btn, 12, 226);
    lv_obj_set_size(a_btn, 216, 44);
    lv_obj_set_style_bg_color(a_btn, C_GREEN, 0);
    lv_obj_set_style_radius(a_btn, 12, 0);
    lv_obj_add_event_cb(a_btn, on_cancel_alert_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *a_bl = lv_label_create(a_btn);
    lv_label_set_text(a_bl, "取消语音提醒 (点此/BOOT)");
    lv_obj_set_style_text_font(a_bl, &g_lv_font_cn16, 0);
    lv_obj_set_style_text_color(a_bl, C_BG, 0);
    lv_obj_center(a_bl);
}

static void set_label_if_changed(lv_obj_t *lbl, const char *txt)
{
    if (!lbl || !txt) return;
    const char *cur = lv_label_get_text(lbl);
    if (!cur || strcmp(cur, txt) != 0) {
        lv_label_set_text(lbl, txt);
    }
}

// === Sync Live Backend Data to LVGL Widgets (Only invalidates labels whose text actually changed!) ===
static void sync_ui_data(void)
{
    struct tm now = {0};
    wifi_time_get_local(&now);

    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", now.tm_hour & 0xFF, now.tm_min & 0xFF);
    set_label_if_changed(s_bot_time_lbl, tbuf);
    set_label_if_changed(s_al_clock_lbl, tbuf);

    // Header Wi-Fi / AP status
    bool online = wifi_time_is_connected();
    bool ap_on = web_portal_is_active();
    if (ap_on) {
        set_label_if_changed(s_hdr_wifi_lbl, LV_SYMBOL_WIFI " AP MODE");
        lv_obj_set_style_text_color(s_hdr_wifi_lbl, C_AMBER, 0);
    } else if (online) {
        set_label_if_changed(s_hdr_wifi_lbl, LV_SYMBOL_WIFI " ONLINE");
        lv_obj_set_style_text_color(s_hdr_wifi_lbl, C_GREEN, 0);
    } else {
        set_label_if_changed(s_hdr_wifi_lbl, LV_SYMBOL_WIFI " OFFLINE");
        lv_obj_set_style_text_color(s_hdr_wifi_lbl, C_RED, 0);
    }

    // DeepSeek Usage Data (Tile 0 + Tile 1)
    deepseek_usage_t ds = {0};
    deepseek_service_get_usage(&ds);
    int bat = axp2101_get_battery_percent();
    bool chg = axp2101_is_charging();

    float ring_pct = 0.0f;
    const char *ring_label = "WAIT";
    char ring_num[12] = "--";

    if (ds.calls > 0 || ds.balance_valid || ds.total_tokens > 0) {
        // Daily token usage percentage against 50M (5000万) daily token quota
        float used_ratio = (float)ds.total_tokens / DAILY_TOKEN_QUOTA;
        if (used_ratio < 0.0f) used_ratio = 0.0f;
        if (used_ratio > 1.0f) used_ratio = 1.0f;
        ring_pct = used_ratio * 100.0f;
        ring_label = (ds.balance_valid && !ds.is_available) ? "EMPTY" : "USED";
        if (ring_pct >= 99.95f) snprintf(ring_num, sizeof(ring_num), "100");
        else snprintf(ring_num, sizeof(ring_num), "%.1f", ring_pct);
    } else {
        ring_label = ds.status_text[0] ? ds.status_text : "WAIT";
    }

    lv_color_t ring_col = C_GREEN;
    if (ds.balance_valid && !ds.is_available) ring_col = C_RED;
    else if (ring_pct >= 80.0f) ring_col = C_ORANGE;

    int32_t arc_val = (int32_t)(ring_pct * 10.0f + 0.5f);
    if (lv_arc_get_value(s_ov_arc) != arc_val) {
        lv_arc_set_value(s_ov_arc, arc_val);
    }
    lv_obj_set_style_arc_color(s_ov_arc, ring_col, LV_PART_INDICATOR);
    set_label_if_changed(s_ov_ring_num, ring_num);
    lv_obj_set_style_text_color(s_ov_ring_num, strcmp(ring_num, "--") == 0 ? C_MUTED : C_WHITE, 0);
    lv_obj_set_style_text_color(s_ov_ring_pct, ring_col, 0);
    set_label_if_changed(s_ov_ring_lbl, ring_label);

    // Tile 0 Cards
    set_label_if_changed(s_ov_tok_lbl, ds.tokens_estimated ? "TOKENS (EST)" : "TOKENS");
    char tok_s[20];
    deepseek_format_tokens(ds.total_tokens, tok_s, sizeof(tok_s));
    set_label_if_changed(s_ov_tok_val, tok_s);

    if (ds.balance_valid) {
        char blbl[20], bval[20];
        snprintf(blbl, sizeof(blbl), "BAL (%s)", ds.currency[0] ? ds.currency : "CNY");
        snprintf(bval, sizeof(bval), "%.2f", ds.total_balance);
        set_label_if_changed(s_ov_bal_lbl, blbl);
        set_label_if_changed(s_ov_bal_val, bval);
        lv_obj_set_style_text_color(s_ov_bal_val, ds.total_balance < 5.0f ? C_ORANGE : C_CYAN, 0);
    } else {
        char eval[16];
        snprintf(eval, sizeof(eval), "%lu", (unsigned long)ds.errors);
        set_label_if_changed(s_ov_bal_lbl, "ERRORS");
        set_label_if_changed(s_ov_bal_val, eval);
        lv_obj_set_style_text_color(s_ov_bal_val, ds.errors > 0 ? C_ORANGE : C_GREEN, 0);
    }

    char cval[20];
    snprintf(cval, sizeof(cval), "%.2f", ds.today_cost);
    set_label_if_changed(s_ov_cost_val, cval);

    char meta[48];
    if (bat >= 0) snprintf(meta, sizeof(meta), "CALL:%lu  ERR:%lu  BAT %d%%",
                           (unsigned long)ds.calls, (unsigned long)ds.errors, bat);
    else snprintf(meta, sizeof(meta), "CALL:%lu  ERR:%lu  USB",
                  (unsigned long)ds.calls, (unsigned long)ds.errors);
    set_label_if_changed(s_ov_meta_lbl, meta);

    // Tile 1 Details
    set_label_if_changed(s_tk_sync_lbl, ds.last_sync[0] ? ds.last_sync : "SYNC");
    if (ds.balance_valid) {
        char b1[32], b2[32];
        snprintf(b1, sizeof(b1), "%s %.2f", ds.currency, ds.total_balance);
        snprintf(b2, sizeof(b2), "赠:%.1f 充:%.1f", ds.granted_balance, ds.topped_up_balance);
        set_label_if_changed(s_tk_bal_val, b1);
        lv_obj_set_style_text_color(s_tk_bal_val, C_GREEN, 0);
        set_label_if_changed(s_tk_bal_sub, b2);
    } else {
        set_label_if_changed(s_tk_bal_val, "未配置 API Key");
        lv_obj_set_style_text_color(s_tk_bal_val, C_AMBER, 0);
        set_label_if_changed(s_tk_bal_sub, "");
    }

    char ps[16], cs[16], calls_s[16], errs_s[16];
    deepseek_format_tokens(ds.prompt_tokens, ps, sizeof(ps));
    deepseek_format_tokens(ds.completion_tokens, cs, sizeof(cs));
    snprintf(calls_s, sizeof(calls_s), "%lu", (unsigned long)ds.calls);
    snprintf(errs_s, sizeof(errs_s), "%lu", (unsigned long)ds.errors);
    set_label_if_changed(s_tk_prompt_val, ps);
    set_label_if_changed(s_tk_comp_val, cs);
    set_label_if_changed(s_tk_calls_val, calls_s);
    set_label_if_changed(s_tk_err_val, errs_s);
    lv_obj_set_style_text_color(s_tk_err_val, ds.errors > 0 ? C_ORANGE : C_GREEN, 0);

    // Tile 2: Weather & Alerts
    weather_data_t w = {0};
    weather_service_get_data(&w);
    if (w.valid) {
        char ws[32];
        snprintf(ws, sizeof(ws), "%.0f° %s", w.temp_c, w.desc);
        set_label_if_changed(s_al_wx_lbl, ws);
        lv_obj_set_style_text_color(s_al_wx_lbl, C_AMBER, 0);
        set_label_if_changed(s_al_city_lbl, w.city);
    } else {
        set_label_if_changed(s_al_wx_lbl, "天气离线");
        lv_obj_set_style_text_color(s_al_wx_lbl, C_MUTED, 0);
        set_label_if_changed(s_al_city_lbl, "");
    }

    s_al_cache_cnt = alert_server_get_history(s_al_cache, 3);
    if (s_al_cache_cnt == 0) {
        lv_obj_clear_flag(s_al_empty_card, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 3; i++) lv_obj_add_flag(s_al_rows[i], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_al_empty_card, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 3; i++) {
            if (i < s_al_cache_cnt) {
                lv_obj_clear_flag(s_al_rows[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(s_al_row_bar[i], level_lv_color(s_al_cache[i].level), 0);
                set_label_if_changed(s_al_row_time[i], s_al_cache[i].time_str);
                set_label_if_changed(s_al_row_ack[i], s_al_cache[i].acked ? "已确认" : "未处理");
                lv_obj_set_style_text_color(s_al_row_ack[i], s_al_cache[i].acked ? C_GREEN : C_RED, 0);
                set_label_if_changed(s_al_row_title[i], s_al_cache[i].title);
            } else {
                lv_obj_add_flag(s_al_rows[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // Tile 3: System Info & Brightness Sync
    app_config_t cfg;
    app_config_get(&cfg);
    char ip_s[20], line[56];
    wifi_time_get_ip(ip_s, sizeof(ip_s));

    snprintf(line, sizeof(line), "Wi-Fi: %s", cfg.wifi_ssid[0] ? cfg.wifi_ssid : "未配置");
    set_label_if_changed(s_sys_wifi_lbl, line);

    snprintf(line, sizeof(line), "IP: %s", online ? ip_s : "未连接");
    set_label_if_changed(s_sys_ip_lbl, line);
    lv_obj_set_style_text_color(s_sys_ip_lbl, online ? C_GREEN : C_RED, 0);

    if (bat >= 0) snprintf(line, sizeof(line), "电量:%d%%%s", bat, chg ? "+" : "");
    else snprintf(line, sizeof(line), "USB供电");
    set_label_if_changed(s_sys_bat_lbl, line);

    bool has_key = (cfg.deepseek_api_key[0] || cfg.ai_api_key[0]);
    set_label_if_changed(s_sys_key_lbl, has_key ? "Key:已配" : "Key:未配");
    lv_obj_set_style_text_color(s_sys_key_lbl, has_key ? C_CYAN : C_AMBER, 0);

    const char *ble_st = ble_ancs_is_ready() ? "BLE:已连" :
                         (ble_ancs_is_connected() ? "BLE:配对中" : "BLE:待连");
    lv_color_t ble_col = ble_ancs_is_ready() ? C_GREEN :
                         (ble_ancs_is_connected() ? C_CYAN : C_MUTED);
    set_label_if_changed(s_sys_ble_lbl, ble_st);
    lv_obj_set_style_text_color(s_sys_ble_lbl, ble_col, 0);

    // Sync brightness if changed remotely via HTTP/serial
    uint8_t hw_bl = lcd_st7789_get_backlight();
    if (!s_is_dimmed && hw_bl >= 10 && hw_bl != s_brightness) {
        s_brightness = hw_bl;
        lv_slider_set_value(s_sys_slider, s_brightness, LV_ANIM_OFF);
    }
    snprintf(line, sizeof(line), "亮度 %u%%", (unsigned)s_brightness);
    set_label_if_changed(s_sys_bl_lbl, line);
    update_dim_button_label();
    update_mute_ui();
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
    audio_set_mute(cfg.muted);
    s_last_activity_ms = now_ms();
    s_is_dimmed = false;

    lv_port_init();
    lv_port_set_wake_cb(wake_backlight_if_dimmed);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_header(scr);

    // Smooth Swipeable 4-Page Tileview
    s_tileview = lv_tileview_create(scr);
    lv_obj_set_pos(s_tileview, 0, 38);
    lv_obj_set_size(s_tileview, BOARD_LCD_WIDTH, 210);
    lv_obj_set_style_bg_color(s_tileview, C_BG, 0);
    lv_obj_set_style_bg_opa(s_tileview, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_tileview, on_tile_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *t0 = lv_tileview_add_tile(s_tileview, 0, 0, LV_DIR_RIGHT);
    lv_obj_t *t1 = lv_tileview_add_tile(s_tileview, 1, 0, LV_DIR_HOR);
    lv_obj_t *t2 = lv_tileview_add_tile(s_tileview, 2, 0, LV_DIR_HOR);
    lv_obj_t *t3 = lv_tileview_add_tile(s_tileview, 3, 0, LV_DIR_LEFT);

    build_tile_overview(t0);
    build_tile_tokens(t1);
    build_tile_alerts(t2);
    build_tile_system(t3);

    build_bottom_bar(scr);
    build_overlays();

    sync_ui_data();
    lv_timer_handler();
    ESP_LOGI(TAG, "LVGL Usage Deck UI ready (brightness=%u%%, auto_dim=%us)",
             (unsigned)s_brightness, (unsigned)s_auto_dim_sec);
}

void app_ui_tick(void)
{
    int64_t t_now = now_ms();

    // 1. Hardware BOOT button (short press = cancel alert / close popup / next page; long press 2.5s = Hotspot)
    bool boot_pressed = (gpio_get_level(BOARD_BTN_BOOT_GPIO) == 0);
    if (boot_pressed) {
        if (wake_backlight_if_dimmed()) {
            s_prev_boot_btn = true;
            s_boot_long_fired = true;
        } else {
            s_boot_press_ticks++;
            if (s_boot_press_ticks >= 500 && !s_boot_long_fired) { // ~2.5s @ 5ms tick
                s_boot_long_fired = true;
                open_hotspot_portal();
            }
        }
    } else if (s_prev_boot_btn) {
        if (!s_boot_long_fired) {
            if (alert_server_get_state(NULL)) {
                alert_server_stop();
                lv_obj_add_flag(s_ovl_alert, LV_OBJ_FLAG_HIDDEN);
            } else if (!lv_obj_has_flag(s_ovl_wechat, LV_OBJ_FLAG_HIDDEN)) {
                s_wx_hide_at_ms = 0;
                lv_obj_add_flag(s_ovl_wechat, LV_OBJ_FLAG_HIDDEN);
            } else if (web_portal_is_active()) {
                web_portal_stop();
                lv_obj_add_flag(s_ovl_portal, LV_OBJ_FLAG_HIDDEN);
            } else if (!lv_obj_has_flag(s_ovl_detail, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(s_ovl_detail, LV_OBJ_FLAG_HIDDEN);
            } else {
                int next = (s_active_page + 1) % MAIN_PAGE_COUNT;
                lv_tileview_set_tile_by_index(s_tileview, next, 0, LV_ANIM_ON);
                update_pagination_dots(next);
            }
        }
        s_boot_press_ticks = 0;
        s_boot_long_fired = false;
    }
    s_prev_boot_btn = boot_pressed;

    // 2. Active Voice Alert Overlay
    alert_state_t alert_st = {0};
    bool alert_on = alert_server_get_state(&alert_st);
    if (alert_on != s_was_alert || (alert_on && alert_st.seq != s_last_alert_seq)) {
        s_was_alert = alert_on;
        s_last_alert_seq = alert_st.seq;
        if (alert_on) {
            force_wake_backlight();
            lv_color_t hc = level_lv_color(alert_st.current.level);
            lv_obj_set_style_bg_color(s_ovl_alert_hdr, hc, 0);
            lv_obj_set_style_border_color(s_ovl_alert_card, hc, 0);
            lv_label_set_text(s_ovl_alert_hdr_lbl,
                              alert_st.current.has_voice ? "告警 · 语音循环播报中" : level_name(alert_st.current.level));
            lv_obj_set_style_text_color(s_ovl_alert_hdr_lbl,
                                        alert_st.current.level == ALERT_LEVEL_WARN ? C_BG : C_WHITE, 0);
            lv_label_set_text(s_ovl_alert_time, alert_st.current.time_str);
            lv_label_set_text(s_ovl_alert_title, alert_st.current.title);
            lv_label_set_text(s_ovl_alert_msg, alert_st.current.message);
            lv_obj_clear_flag(s_ovl_alert, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ovl_alert, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 3. iPhone WeChat Notification Overlay (15s auto-dismiss)
    ble_ancs_msg_t latest_wx = {0};
    if (ble_ancs_get_latest_msg(&latest_wx) && latest_wx.seq != s_last_wx_seq) {
        s_last_wx_seq = latest_wx.seq;
        force_wake_backlight();
        lv_label_set_text(s_ovl_wx_title, latest_wx.title[0] ? latest_wx.title : "微信");
        lv_label_set_text(s_ovl_wx_msg, latest_wx.message);
        lv_obj_clear_flag(s_ovl_wechat, LV_OBJ_FLAG_HIDDEN);
        s_wx_hide_at_ms = t_now + 15000LL;
    }
    if (s_wx_hide_at_ms > 0 && t_now >= s_wx_hide_at_ms) {
        s_wx_hide_at_ms = 0;
        lv_obj_add_flag(s_ovl_wechat, LV_OBJ_FLAG_HIDDEN);
    }

    // 4. SoftAP Portal Overlay Sync (if triggered from serial 'setup' or Wi-Fi failure)
    bool portal_on = web_portal_is_active();
    if (portal_on != s_was_portal) {
        s_was_portal = portal_on;
        if (portal_on) {
            force_wake_backlight();
            lv_label_set_text(s_ovl_portal_ssid, web_portal_get_ap_ssid());
            char url[32];
            snprintf(url, sizeof(url), "http://%s", web_portal_get_ap_ip());
            lv_label_set_text(s_ovl_portal_url, url);
            lv_obj_clear_flag(s_ovl_portal, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ovl_portal, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 5. Auto-Dim Power Saving
    if (s_auto_dim_sec > 0 && !s_is_dimmed && !alert_on) {
        if ((t_now - s_last_activity_ms) >= (int64_t)s_auto_dim_sec * 1000LL) {
            s_is_dimmed = true;
            lcd_st7789_set_backlight(DIM_BACKLIGHT_PCT);
        }
    }

    // 6. Periodic Data Sync to LVGL labels (every 500ms — LVGL skips redraws when text is unchanged!)
    if (t_now - s_last_data_sync_ms >= 500LL) {
        s_last_data_sync_ms = t_now;
        sync_ui_data();
    }

    // 7. Pump LVGL rendering, animations & touch events
    lv_timer_handler();
}
