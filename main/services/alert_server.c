#include "alert_server.h"
#include "app_config.h"
#include "audio_codec.h"
#include "axp2101.h"
#include "ble_ancs.h"
#include "deepseek_service.h"
#include "lcd_st7789.h"
#include "web_portal.h"
#include "wifi_time.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "alert_srv";

#define ALERT_MAX_WAV_BYTES (96 * 1024) // Up to 6s @ 8kHz or 3s @ 16kHz 16-bit mono WAV

static httpd_handle_t s_server = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static alert_state_t s_state = {0};

// History ring (newest at index 0)
static alert_record_t s_history[ALERT_HISTORY_MAX];
static int s_history_count = 0;
static uint32_t s_next_id = 1;

static volatile bool s_stop_flag = false;
static volatile bool s_playback_running = false;
static uint8_t *s_wav_buf = NULL;
static size_t s_wav_len = 0;

static bool should_stop_playback(void)
{
    return s_stop_flag;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *src, char *dst, size_t dst_sz)
{
    size_t i = 0, j = 0;
    while (src[i] && j + 1 < dst_sz) {
        if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            int hi = hex_val(src[i + 1]);
            int lo = hex_val(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[j++] = (char)((hi << 4) | lo);
                i += 3;
            } else {
                dst[j++] = src[i++];
            }
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

static bool json_get_str(const char *json, const char *key, char *out, size_t out_sz)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            if (*p == 'n') { out[i++] = '\n'; p++; continue; }
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

static alert_level_t parse_level(const char *s)
{
    if (!s) return ALERT_LEVEL_ERROR;
    if (strcmp(s, "info") == 0) return ALERT_LEVEL_INFO;
    if (strcmp(s, "warn") == 0 || strcmp(s, "warning") == 0) return ALERT_LEVEL_WARN;
    return ALERT_LEVEL_ERROR;
}

static void alert_playback_task(void *arg)
{
    (void)arg;
    s_playback_running = true;

    // Continuously loop voice/chime reminder until user presses button/screen to cancel
    while (!s_stop_flag) {
        if (s_wav_buf && s_wav_len > 44) {
            audio_play_wav(s_wav_buf, s_wav_len, should_stop_playback);
        } else {
            audio_play_alarm_chime(should_stop_playback);
        }

        if (s_stop_flag) {
            break;
        }
        // Short 1.2s pause between voice reminders, checking cancel flag every 50ms
        for (int i = 0; i < 24 && !s_stop_flag; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    s_playback_running = false;
    vTaskDelete(NULL);
}

static void stop_current_playback(void)
{
    s_stop_flag = true;
    for (int i = 0; i < 50 && s_playback_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_wav_buf) {
        free(s_wav_buf);
        s_wav_buf = NULL;
        s_wav_len = 0;
    }
}

static void trigger_alert(alert_level_t level, const char *title, const char *message,
                          uint8_t *wav_buf, size_t wav_len)
{
    stop_current_playback();

    struct tm now = {0};
    wifi_time_get_local(&now);

    alert_record_t rec = {0};
    rec.id = s_next_id++;
    rec.level = level;
    snprintf(rec.time_str, sizeof(rec.time_str), "%02d:%02d:%02d",
             now.tm_hour & 0xFF, now.tm_min & 0xFF, now.tm_sec & 0xFF);
    strncpy(rec.title, (title && title[0]) ? title : "系统告警", sizeof(rec.title) - 1);
    strncpy(rec.message, message ? message : "", sizeof(rec.message) - 1);
    rec.acked = false;
    rec.has_voice = (wav_buf != NULL && wav_len > 44);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    // Shift history down so newest is always index 0
    int move_n = s_history_count < ALERT_HISTORY_MAX ? s_history_count : (ALERT_HISTORY_MAX - 1);
    if (move_n > 0) {
        memmove(&s_history[1], &s_history[0], (size_t)move_n * sizeof(alert_record_t));
    }
    s_history[0] = rec;
    if (s_history_count < ALERT_HISTORY_MAX) {
        s_history_count++;
    }

    s_state.active = true;
    s_state.seq++;
    s_state.current = rec;
    xSemaphoreGive(s_mutex);

    s_wav_buf = wav_buf;
    s_wav_len = wav_len;
    s_stop_flag = false;

    ESP_LOGI(TAG, "Alert #%lu [%s] (voice=%d): %s - %s",
             (unsigned long)rec.id, rec.time_str, rec.has_voice ? 1 : 0, rec.title, rec.message);
    xTaskCreate(alert_playback_task, "alert_play", 4096, NULL, 4, NULL);
}

static esp_err_t handle_get_status(httpd_req_t *req)
{
    char ip[20];
    wifi_time_get_ip(ip, sizeof(ip));
    alert_state_t st = {0};
    alert_server_get_state(&st);
    alert_stats_t stats = {0};
    alert_server_get_stats(&stats);

    char resp[340];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"ip\":\"%s\",\"battery\":%d,\"charging\":%s,\"brightness\":%u,"
             "\"alert_active\":%s,\"playing\":%s,\"total\":%lu,\"unacked\":%lu}",
             ip,
             axp2101_get_battery_percent(),
             axp2101_is_charging() ? "true" : "false",
             (unsigned)lcd_st7789_get_backlight(),
             st.active ? "true" : "false",
             s_playback_running ? "true" : "false",
             (unsigned long)stats.total,
             (unsigned long)stats.unacked_cnt);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_post_notify(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 1024) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body size must be 1..1024 bytes");
    }

    char *body = malloc((size_t)len + 1);
    if (!body) return httpd_resp_send_500(req);

    int rtot = 0;
    while (rtot < len) {
        int r = httpd_req_recv(req, body + rtot, len - rtot);
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        rtot += r;
    }
    body[rtot] = '\0';

    char lvl_s[16] = "error";
    char title[64] = "系统告警";
    char msg[192] = "";
    json_get_str(body, "level", lvl_s, sizeof(lvl_s));
    json_get_str(body, "title", title, sizeof(title));
    json_get_str(body, "message", msg, sizeof(msg));
    free(body);

    trigger_alert(parse_level(lvl_s), title, msg, NULL, 0);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_post_alert_audio(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 44 || len > ALERT_MAX_WAV_BYTES) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"WAV must be 45..98304 bytes (8kHz/16kHz 16-bit mono)\"}",
                               HTTPD_RESP_USE_STRLEN);
    }

    char lvl_s[16] = "error";
    char title[64] = "语音告警";
    char msg[192] = "收到新告警，请点击按钮停止播报";

    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 512) {
        char *qbuf = malloc(qlen + 1);
        if (qbuf && httpd_req_get_url_query_str(req, qbuf, qlen + 1) == ESP_OK) {
            char tmp[256];
            if (httpd_query_key_value(qbuf, "level", tmp, sizeof(tmp)) == ESP_OK) {
                url_decode(tmp, lvl_s, sizeof(lvl_s));
            }
            if (httpd_query_key_value(qbuf, "title", tmp, sizeof(tmp)) == ESP_OK) {
                url_decode(tmp, title, sizeof(title));
            }
            if (httpd_query_key_value(qbuf, "message", tmp, sizeof(tmp)) == ESP_OK) {
                url_decode(tmp, msg, sizeof(msg));
            }
        }
        free(qbuf);
    }

    stop_current_playback();

    uint8_t *wav = malloc((size_t)len);
    if (!wav) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Low memory for WAV");
    }

    int rtot = 0;
    while (rtot < len) {
        int r = httpd_req_recv(req, (char *)wav + rtot, len - rtot);
        if (r <= 0) {
            free(wav);
            return ESP_FAIL;
        }
        rtot += r;
    }

    // Always loop voice reminder until cancelled by button/screen
    trigger_alert(parse_level(lvl_s), title, msg, wav, (size_t)rtot);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static bool json_get_u64(const char *json, const char *key, uint64_t *out)
{
    char pat[36];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 10);
    if (end == p) return false;
    *out = (uint64_t)v;
    return true;
}

static bool json_get_float(const char *json, const char *key, float *out)
{
    char pat[36];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    char *end = NULL;
    float v = strtof(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

static bool json_get_bool(const char *json, const char *key, bool *out)
{
    char pat[36];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (strncmp(p, "true", 4) == 0 || *p == '1') { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0 || *p == '0') { *out = false; return true; }
    return false;
}

static esp_err_t handle_get_deepseek_status(httpd_req_t *req)
{
    deepseek_usage_t u = {0};
    deepseek_service_get_usage(&u);

    char resp[384];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"balance_valid\":%s,\"is_available\":%s,\"currency\":\"%s\","
             "\"total_balance\":%.2f,\"today_cost\":%.4f,\"daily_budget\":%.2f,"
             "\"calls\":%lu,\"errors\":%lu,\"prompt_tokens\":%llu,\"completion_tokens\":%llu,"
             "\"total_tokens\":%llu,\"tokens_estimated\":%s,\"status\":\"%s\"}",
             u.balance_valid ? "true" : "false",
             u.is_available ? "true" : "false",
             u.currency,
             u.total_balance, u.today_cost, u.daily_budget,
             (unsigned long)u.calls, (unsigned long)u.errors,
             (unsigned long long)u.prompt_tokens,
             (unsigned long long)u.completion_tokens,
             (unsigned long long)u.total_tokens,
             u.tokens_estimated ? "true" : "false",
             u.status_text);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_post_deepseek_usage(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 1024) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body size must be 1..1024 bytes");
    }
    char *body = malloc((size_t)len + 1);
    if (!body) return httpd_resp_send_500(req);
    int rtot = 0;
    while (rtot < len) {
        int r = httpd_req_recv(req, body + rtot, len - rtot);
        if (r <= 0) { free(body); return ESP_FAIL; }
        rtot += r;
    }
    body[rtot] = '\0';

    uint64_t calls = 0, errors = 0, p_tok = 0, c_tok = 0, t_tok = 0;
    float cost = 0.0f;
    json_get_u64(body, "calls", &calls);
    json_get_u64(body, "errors", &errors);
    json_get_u64(body, "prompt_tokens", &p_tok);
    json_get_u64(body, "completion_tokens", &c_tok);
    json_get_u64(body, "total_tokens", &t_tok);
    bool has_cost = json_get_float(body, "cost_rmb", &cost) || json_get_float(body, "today_cost", &cost);
    free(body);

    deepseek_service_set_usage((uint32_t)calls, (uint32_t)errors, p_tok, c_tok, t_tok, cost, has_cost);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_post_deepseek_add(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 1024) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body size must be 1..1024 bytes");
    }
    char *body = malloc((size_t)len + 1);
    if (!body) return httpd_resp_send_500(req);
    int rtot = 0;
    while (rtot < len) {
        int r = httpd_req_recv(req, body + rtot, len - rtot);
        if (r <= 0) { free(body); return ESP_FAIL; }
        rtot += r;
    }
    body[rtot] = '\0';

    uint64_t p_tok = 0, c_tok = 0;
    float cost = 0.0f;
    bool is_err = false;
    json_get_u64(body, "prompt_tokens", &p_tok);
    json_get_u64(body, "completion_tokens", &c_tok);
    json_get_float(body, "cost_rmb", &cost);
    json_get_bool(body, "error", &is_err);
    free(body);

    deepseek_service_add_call(p_tok, c_tok, cost, is_err);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_post_brightness(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 256) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body size must be 1..256 bytes");
    }
    char body[257];
    int rtot = 0;
    while (rtot < len) {
        int r = httpd_req_recv(req, body + rtot, len - rtot);
        if (r <= 0) return ESP_FAIL;
        rtot += r;
    }
    body[rtot] = '\0';

    uint64_t bl = 80;
    if (json_get_u64(body, "brightness", &bl)) {
        if (bl < 10) bl = 10;
        if (bl > 100) bl = 100;
        app_config_t cfg;
        app_config_get(&cfg);
        app_config_set_display((uint8_t)bl, cfg.auto_dim_sec);
        lcd_st7789_set_backlight((uint8_t)bl);
    }

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"brightness\":%u}", (unsigned)lcd_st7789_get_backlight());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_post_stop(httpd_req_t *req)
{
    alert_server_stop();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true,\"stopped\":true}", HTTPD_RESP_USE_STRLEN);
}

// Lightweight USB console helper: type 'ip', 'test', 'stop', 'wx <msg>', or 'setup'
static void console_cmd_task(void *arg)
{
    (void)arg;
    char line[128];
    size_t pos = 0;
    ESP_LOGI(TAG, "Console ready! Commands: ip | test | stop | wx <msg> | setup");

    while (1) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (pos > 0) {
                line[pos] = '\0';
                if (strcmp(line, "ip") == 0) {
                    char ip[20];
                    wifi_time_get_ip(ip, sizeof(ip));
                    ESP_LOGI(TAG, "Device IP: %s | Webhook: http://%s/api/alert/audio", ip, ip);
                } else if (strcmp(line, "test") == 0) {
                    trigger_alert(ALERT_LEVEL_ERROR, "数据库告警", "主库连接超时，请立即排查处理", NULL, 0);
                } else if (strcmp(line, "stop") == 0) {
                    alert_server_stop();
                } else if (strncmp(line, "wx ", 3) == 0) {
                    ble_ancs_inject_test_msg("微信消息", line + 3);
                } else if (strcmp(line, "setup") == 0) {
                    web_portal_start();
                } else if (strcmp(line, "ds") == 0) {
                    deepseek_usage_t u = {0};
                    deepseek_service_get_usage(&u);
                    ESP_LOGI(TAG, "DeepSeek: bal=%.2f %s | calls=%lu err=%lu | tokens=%llu | cost=%.2f",
                             u.total_balance, u.currency, (unsigned long)u.calls,
                             (unsigned long)u.errors, (unsigned long long)u.total_tokens, u.today_cost);
                } else if (strcmp(line, "dstest") == 0) {
                    deepseek_service_set_usage(729, 1, 682400, 162800, 845200, 10.83f, true);
                } else if (strncmp(line, "bl ", 3) == 0) {
                    int pct = atoi(line + 3);
                    if (pct < 5) pct = 5;
                    if (pct > 100) pct = 100;
                    app_config_t cfg;
                    app_config_get(&cfg);
                    if (pct >= 10) app_config_set_display((uint8_t)pct, cfg.auto_dim_sec);
                    lcd_st7789_set_backlight((uint8_t)pct);
                    ESP_LOGI(TAG, "Backlight set to %d%%", pct);
                }
                pos = 0;
            }
        } else if (pos + 1 < sizeof(line)) {
            line[pos++] = (char)ch;
        }
    }
}

esp_err_t alert_server_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    xTaskCreate(console_cmd_task, "console_cmd", 4096, NULL, 2, NULL);
    return ESP_OK;
}

esp_err_t alert_server_start(void)
{
    if (s_server || web_portal_is_active()) {
        return ESP_OK;
    }
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 12;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start alert HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/api/status",          .method = HTTP_GET,  .handler = handle_get_status },
        { .uri = "/api/notify",          .method = HTTP_POST, .handler = handle_post_notify },
        { .uri = "/api/alert/audio",     .method = HTTP_POST, .handler = handle_post_alert_audio },
        { .uri = "/api/alert/stop",      .method = HTTP_POST, .handler = handle_post_stop },
        { .uri = "/api/deepseek/status", .method = HTTP_GET,  .handler = handle_get_deepseek_status },
        { .uri = "/api/deepseek/usage",  .method = HTTP_POST, .handler = handle_post_deepseek_usage },
        { .uri = "/api/deepseek/add",    .method = HTTP_POST, .handler = handle_post_deepseek_add },
        { .uri = "/api/brightness",      .method = HTTP_POST, .handler = handle_post_brightness },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    char ip[20];
    wifi_time_get_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "Alert & DeepSeek Webhook ready at http://%s", ip);
    return ESP_OK;
}

void alert_server_stop_httpd(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Stopped STA HTTP server to free port 80");
    }
}

bool alert_server_get_state(alert_state_t *out)
{
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool active = s_state.active;
    if (out) {
        *out = s_state;
    }
    xSemaphoreGive(s_mutex);
    return active;
}

void alert_server_stop(void)
{
    stop_current_playback();
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state.active = false;
        s_state.current.acked = true;
        if (s_history_count > 0 && s_history[0].id == s_state.current.id) {
            s_history[0].acked = true;
        }
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "Alert acknowledged & playback stopped");
}

void alert_server_get_stats(alert_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    out->total = (uint32_t)s_history_count;
    for (int i = 0; i < s_history_count; i++) {
        if (s_history[i].level == ALERT_LEVEL_ERROR) out->error_cnt++;
        else if (s_history[i].level == ALERT_LEVEL_WARN) out->warn_cnt++;
        if (!s_history[i].acked) out->unacked_cnt++;
    }
    xSemaphoreGive(s_mutex);
}

int alert_server_get_history(alert_record_t *out_list, int max_count)
{
    if (!out_list || max_count <= 0 || !s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_history_count < max_count ? s_history_count : max_count;
    if (n > 0) {
        memcpy(out_list, s_history, (size_t)n * sizeof(alert_record_t));
    }
    xSemaphoreGive(s_mutex);
    return n;
}

void alert_server_clear_history(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_history_count = 0;
    xSemaphoreGive(s_mutex);
}
