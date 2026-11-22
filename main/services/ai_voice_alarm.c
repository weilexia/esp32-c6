#include "ai_voice_alarm.h"
#include "alarm_manager.h"
#include "app_config.h"
#include "audio_codec.h"
#include "ble_ancs.h"
#include "web_portal.h"
#include "wifi_time.h"
#include "sdkconfig.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ai_alarm";

static ai_voice_status_t s_status = {0};
static SemaphoreHandle_t s_mutex = NULL;
static volatile bool s_busy = false;

static void set_status(ai_voice_state_t st, const char *heard, const char *reply)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.state = st;
    if (heard) {
        strncpy(s_status.heard_text, heard, sizeof(s_status.heard_text) - 1);
        s_status.heard_text[sizeof(s_status.heard_text) - 1] = '\0';
    }
    if (reply) {
        strncpy(s_status.reply_text, reply, sizeof(s_status.reply_text) - 1);
        s_status.reply_text[sizeof(s_status.reply_text) - 1] = '\0';
    }
    xSemaphoreGive(s_mutex);
}

// Helper: extract a JSON string value for a given key (handles escaped quotes minimally)
static bool json_extract_string(const char *json, const char *key, char *out, size_t out_sz)
{
    const char *p = strstr(json, key);
    if (!p) return false;
    p = strchr(p + strlen(key), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz) {
        if (*p == '\\' && *(p + 1)) {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

static bool json_extract_int(const char *json, const char *key, int *out_val)
{
    const char *p = strstr(json, key);
    if (!p) return false;
    p = strchr(p + strlen(key), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out_val = (int)v;
    return true;
}

static bool json_extract_bool(const char *json, const char *key, bool *out_val)
{
    const char *p = strstr(json, key);
    if (!p) return false;
    p = strchr(p + strlen(key), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0) {
        *out_val = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out_val = false;
        return true;
    }
    return false;
}

// === Local offline Chinese & English Natural Language Alarm Parser ===
static int parse_cn_or_ascii_num(const char *s, const char **end_out)
{
    while (*s == ' ') s++;
    if (isdigit((unsigned char)*s)) {
        char *e = NULL;
        int v = (int)strtol(s, &e, 10);
        if (end_out) *end_out = e;
        return v;
    }
    // Simple Chinese numerals (0..59)
    static const struct { const char *utf8; int val; } cn_digits[] = {
        {"零", 0}, {"一", 1}, {"二", 2}, {"两", 2}, {"三", 3}, {"四", 4},
        {"五", 5}, {"六", 6}, {"七", 7}, {"八", 8}, {"九", 9},
    };
    int val = -1;
    const char *p = s;
    bool has_ten = false;

    if (strncmp(p, "十", 3) == 0) {
        val = 10;
        p += 3;
        has_ten = true;
    } else {
        for (size_t i = 0; i < sizeof(cn_digits) / sizeof(cn_digits[0]); i++) {
            size_t len = strlen(cn_digits[i].utf8);
            if (strncmp(p, cn_digits[i].utf8, len) == 0) {
                val = cn_digits[i].val;
                p += len;
                break;
            }
        }
        if (val >= 0 && strncmp(p, "十", 3) == 0) {
            val *= 10;
            p += 3;
            has_ten = true;
        }
    }
    if (has_ten) {
        for (size_t i = 0; i < sizeof(cn_digits) / sizeof(cn_digits[0]); i++) {
            size_t len = strlen(cn_digits[i].utf8);
            if (strncmp(p, cn_digits[i].utf8, len) == 0) {
                val += cn_digits[i].val;
                p += len;
                break;
            }
        }
    }
    if (end_out) *end_out = p;
    return val;
}

static bool local_nlp_parse_and_apply(const char *text, char *reply, size_t reply_sz)
{
    if (!text || text[0] == '\0') return false;

    // 1. Delete / Clear commands
    if (strstr(text, "取消") || strstr(text, "删除") || strstr(text, "清空") ||
        strstr(text, "clear") || strstr(text, "delete")) {
        alarm_manager_clear_all();
        snprintf(reply, reply_sz, "All alarms cleared");
        return true;
    }

    struct tm now = {0};
    wifi_time_get_local(&now);
    bool repeat_daily = (strstr(text, "每天") != NULL || strstr(text, "每日") != NULL || strstr(text, "daily") != NULL);

    // 2. Relative time: "半小时后" / "X分钟后" / "X小时后" / "in X min"
    if (strstr(text, "半小时") || strstr(text, "half hour")) {
        int total_min = now.tm_hour * 60 + now.tm_min + 30;
        uint8_t h = (uint8_t)((total_min / 60) % 24);
        uint8_t m = (uint8_t)(total_min % 60);
        alarm_manager_add(h, m, false, "AI +30m");
        snprintf(reply, reply_sz, "Set %02u:%02u (+30m)", h, m);
        return true;
    }

    const char *p_min = strstr(text, "分钟");
    if (!p_min) p_min = strstr(text, "min");
    if (p_min && (strstr(text, "后") || strstr(text, "in ") || strstr(text, "after"))) {
        // Search backwards or from start for a number
        for (const char *scan = text; scan < p_min; scan++) {
            int n = parse_cn_or_ascii_num(scan, NULL);
            if (n > 0 && n <= 1440) {
                int total_min = now.tm_hour * 60 + now.tm_min + n;
                uint8_t h = (uint8_t)((total_min / 60) % 24);
                uint8_t m = (uint8_t)(total_min % 60);
                alarm_manager_add(h, m, false, "AI Timer");
                snprintf(reply, reply_sz, "Set %02u:%02u (+%dm)", h, m, n);
                return true;
            }
        }
    }

    const char *p_hr = strstr(text, "小时");
    if (!p_hr) p_hr = strstr(text, "hour");
    if (p_hr && (strstr(text, "后") || strstr(text, "in ") || strstr(text, "after"))) {
        for (const char *scan = text; scan < p_hr; scan++) {
            int n = parse_cn_or_ascii_num(scan, NULL);
            if (n > 0 && n <= 24) {
                int total_min = now.tm_hour * 60 + now.tm_min + n * 60;
                uint8_t h = (uint8_t)((total_min / 60) % 24);
                uint8_t m = (uint8_t)(total_min % 60);
                alarm_manager_add(h, m, false, "AI Timer");
                snprintf(reply, reply_sz, "Set %02u:%02u (+%dh)", h, m, n);
                return true;
            }
        }
    }

    // 3. Absolute time: "HH:MM" or "X点Y分" / "X点半"
    int hour = -1, minute = 0;
    bool pm = (strstr(text, "下午") || strstr(text, "晚上") || strstr(text, "pm") || strstr(text, "PM"));

    const char *colon = strchr(text, ':');
    if (colon && colon > text && isdigit((unsigned char)*(colon - 1)) && isdigit((unsigned char)*(colon + 1))) {
        const char *hs = colon - 1;
        while (hs > text && isdigit((unsigned char)*(hs - 1))) hs--;
        hour = atoi(hs);
        minute = atoi(colon + 1);
    } else {
        const char *dian = strstr(text, "点");
        if (dian) {
            for (const char *scan = text; scan < dian; scan++) {
                int n = parse_cn_or_ascii_num(scan, NULL);
                if (n >= 0 && n <= 23) {
                    hour = n;
                    break;
                }
            }
            const char *after = dian + strlen("点");
            if (strstr(after, "半")) {
                minute = 30;
            } else if (strstr(after, "一刻")) {
                minute = 15;
            } else if (strstr(after, "三刻")) {
                minute = 45;
            } else {
                int m = parse_cn_or_ascii_num(after, NULL);
                if (m >= 0 && m <= 59) {
                    minute = m;
                }
            }
        }
    }

    if (hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59) {
        if (pm && hour < 12) {
            hour += 12;
        }
        alarm_manager_add((uint8_t)hour, (uint8_t)minute, repeat_daily, repeat_daily ? "AI Daily" : "AI Alarm");
        snprintf(reply, reply_sz, "Set %02d:%02d%s", hour, minute, repeat_daily ? " Daily" : "");
        return true;
    }

    return false;
}

// === Cloud OpenAI-compatible LLM Intent Parser ===
static bool cloud_llm_parse_and_apply(const char *utterance, char *reply, size_t reply_sz)
{
    app_config_t app_cfg;
    app_config_get(&app_cfg);
    if (!wifi_time_is_connected() || strlen(app_cfg.ai_api_key) == 0 || strlen(app_cfg.ai_llm_url) == 0) {
        return false;
    }

    struct tm now = {0};
    wifi_time_get_local(&now);

    char *req_body = malloc(1024);
    char *resp_buf = malloc(2048);
    if (!req_body || !resp_buf) {
        free(req_body);
        free(resp_buf);
        return false;
    }

    // Sanitize quotes in utterance
    char safe_utt[128];
    size_t j = 0;
    for (size_t i = 0; utterance[i] && j + 1 < sizeof(safe_utt); i++) {
        char c = utterance[i];
        safe_utt[j++] = (c == '"' || c == '\\' || c == '\n' || c == '\r') ? ' ' : c;
    }
    safe_utt[j] = '\0';

    snprintf(req_body, 1024,
             "{\"model\":\"%s\",\"temperature\":0,\"messages\":["
             "{\"role\":\"system\",\"content\":\"You are an alarm clock parser. Current local time is %04d-%02d-%02d %02d:%02d. "
             "Return ONLY raw JSON without markdown: "
             "{\\\"action\\\":\\\"set\\\"|\\\"delete_all\\\"|\\\"none\\\",\\\"hour\\\":0-23,\\\"minute\\\":0-59,"
             "\\\"repeat_daily\\\":false,\\\"label\\\":\\\"short ASCII label\\\",\\\"reply\\\":\\\"short ASCII reply\\\"}.\"},"
             "{\"role\":\"user\",\"content\":\"%s\"}]}",
             app_cfg.ai_llm_model,
             now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour, now.tm_min,
             safe_utt);

    char auth_hdr[192];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", app_cfg.ai_api_key);

    esp_http_client_config_t cfg = {
        .url = app_cfg.ai_llm_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 12000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(req_body);
        free(resp_buf);
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_hdr);

    bool handled = false;
    int req_len = (int)strlen(req_body);
    if (esp_http_client_open(client, req_len) == ESP_OK) {
        esp_http_client_write(client, req_body, req_len);
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        int total = 0, r = 0;
        while (total < 2047 && (r = esp_http_client_read(client, resp_buf + total, 2047 - total)) > 0) {
            total += r;
        }
        resp_buf[total] = '\0';

        if (status == 200 && total > 10) {
            char content[384] = {0};
            // Extract "content":"..." and unescape inner JSON
            if (json_extract_string(resp_buf, "\"content\"", content, sizeof(content))) {
                char action[24] = {0};
                char label[28] = "AI Alarm";
                int hour = -1, minute = -1;
                bool daily = false;

                json_extract_string(content, "action", action, sizeof(action));
                json_extract_int(content, "hour", &hour);
                json_extract_int(content, "minute", &minute);
                json_extract_bool(content, "repeat_daily", &daily);
                json_extract_string(content, "label", label, sizeof(label));
                json_extract_string(content, "reply", reply, reply_sz);

                if (strcmp(action, "set") == 0 && hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59) {
                    alarm_manager_add((uint8_t)hour, (uint8_t)minute, daily, label);
                    if (reply[0] == '\0') {
                        snprintf(reply, reply_sz, "AI Set %02d:%02d", hour, minute);
                    }
                    handled = true;
                } else if (strcmp(action, "delete_all") == 0) {
                    alarm_manager_clear_all();
                    if (reply[0] == '\0') {
                        snprintf(reply, reply_sz, "All alarms cleared");
                    }
                    handled = true;
                }
            }
        } else {
            ESP_LOGW(TAG, "Cloud LLM HTTP status=%d", status);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(req_body);
    free(resp_buf);
    return handled;
}

// === Cloud OpenAI-compatible Whisper/SenseVoice STT ===
static bool cloud_stt_transcribe(const uint8_t *wav_data, size_t wav_len, char *out_text, size_t out_sz)
{
    app_config_t app_cfg;
    app_config_get(&app_cfg);
    if (!wifi_time_is_connected() || strlen(app_cfg.ai_api_key) == 0 || strlen(app_cfg.ai_stt_url) == 0) {
        return false;
    }

    const char *boundary = "----ESP32C6VoiceBoundary";
    char part_hdr[256];
    char part_ftr[160];

    int hdr_len = snprintf(part_hdr, sizeof(part_hdr),
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n"
                           "Content-Type: audio/wav\r\n\r\n",
                           boundary);
    int ftr_len = snprintf(part_ftr, sizeof(part_ftr),
                           "\r\n--%s\r\n"
                           "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                           "%s\r\n--%s--\r\n",
                           boundary, app_cfg.ai_stt_model, boundary);

    int total_len = hdr_len + (int)wav_len + ftr_len;

    char content_type[96];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    char auth_hdr[192];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", app_cfg.ai_api_key);

    esp_http_client_config_t cfg = {
        .url = app_cfg.ai_stt_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "Authorization", auth_hdr);

    bool ok = false;
    char *resp = malloc(1024);
    if (resp && esp_http_client_open(client, total_len) == ESP_OK) {
        esp_http_client_write(client, part_hdr, hdr_len);
        // Stream WAV in 2KB chunks
        size_t sent = 0;
        while (sent < wav_len) {
            size_t chunk = (wav_len - sent) > 2048 ? 2048 : (wav_len - sent);
            int w = esp_http_client_write(client, (const char *)(wav_data + sent), (int)chunk);
            if (w <= 0) break;
            sent += (size_t)w;
        }
        esp_http_client_write(client, part_ftr, ftr_len);

        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        int rtot = 0, r = 0;
        while (rtot < 1023 && (r = esp_http_client_read(client, resp + rtot, 1023 - rtot)) > 0) {
            rtot += r;
        }
        resp[rtot] = '\0';

        if (status == 200 && json_extract_string(resp, "\"text\"", out_text, out_sz)) {
            ESP_LOGI(TAG, "STT transcribed: %s", out_text);
            ok = true;
        } else {
            ESP_LOGW(TAG, "STT HTTP status=%d resp=%s", status, resp);
        }
    }

    free(resp);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

bool ai_voice_alarm_process_text(const char *utterance)
{
    if (!utterance || utterance[0] == '\0') return false;
    set_status(AI_VOICE_THINKING, utterance, "Parsing...");

    char reply[64] = {0};
    bool ok = cloud_llm_parse_and_apply(utterance, reply, sizeof(reply));
    if (!ok) {
        ok = local_nlp_parse_and_apply(utterance, reply, sizeof(reply));
    }

    if (ok) {
        set_status(AI_VOICE_DONE, utterance, reply);
        audio_play_beep_ok();
        ESP_LOGI(TAG, "AI Alarm OK: [%s] -> %s", utterance, reply);
    } else {
        set_status(AI_VOICE_ERROR, utterance, "Not understood");
        audio_play_beep_err();
        ESP_LOGW(TAG, "AI Alarm could not parse: %s", utterance);
    }
    return ok;
}

static void voice_capture_task(void *arg)
{
    (void)arg;
    set_status(AI_VOICE_LISTENING, "", "Speak now (3s)...");
    audio_play_beep_start();

    // 3.0s @ 16kHz 16-bit mono = 96,044 bytes
    const size_t wav_buf_sz = 44 + (AUDIO_SAMPLE_RATE_HZ * 3 * sizeof(int16_t));
    uint8_t *wav_buf = malloc(wav_buf_sz);
    if (!wav_buf) {
        set_status(AI_VOICE_ERROR, "", "Low memory");
        audio_play_beep_err();
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    size_t wav_len = audio_record_wav(wav_buf, wav_buf_sz, 3000, NULL);
    set_status(AI_VOICE_THINKING, "", "Transcribing...");

    char text[96] = {0};
    bool stt_ok = (wav_len > 1000) && cloud_stt_transcribe(wav_buf, wav_len, text, sizeof(text));
    free(wav_buf);

    app_config_t app_cfg;
    app_config_get(&app_cfg);

    if (stt_ok && text[0] != '\0') {
        ai_voice_alarm_process_text(text);
    } else if (strlen(app_cfg.ai_api_key) == 0) {
        set_status(AI_VOICE_ERROR, "No API Key", "Hold BOOT 3s: Setup");
        audio_play_beep_err();
    } else {
        set_status(AI_VOICE_ERROR, "", "STT failed");
        audio_play_beep_err();
    }

    s_busy = false;
    vTaskDelete(NULL);
}

// USB Serial/JTAG console listener: allows typing "ai 5分钟后提醒我" or "ai 每天7点半起床"
static void console_ai_task(void *arg)
{
    (void)arg;
    char line[128];
    size_t pos = 0;

    ESP_LOGI(TAG, "Console AI ready! Type: ai 5分钟后提醒我  or  ai 每天早上7点半起床");

    while (1) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (pos > 0) {
                line[pos] = '\0';
                if (strncmp(line, "ai ", 3) == 0) {
                    ai_voice_alarm_process_text(line + 3);
                } else if (strcmp(line, "ip") == 0) {
                    char ip[20];
                    wifi_time_get_ip(ip, sizeof(ip));
                    ESP_LOGI(TAG, "Device IP: %s | Webhook: http://%s/api/alert/audio", ip, ip);
                } else if (strncmp(line, "wx ", 3) == 0) {
                    ble_ancs_inject_test_msg("微信测试", line + 3);
                } else if (strcmp(line, "setup") == 0) {
                    web_portal_start();
                } else if (strcmp(line, "alarms") == 0) {
                    alarm_entry_t list[ALARM_MAX_COUNT];
                    alarm_manager_get_all(list, ALARM_MAX_COUNT);
                    for (int i = 0; i < ALARM_MAX_COUNT; i++) {
                        if (list[i].used) {
                            ESP_LOGI(TAG, "  [%d] %02u:%02u %s %s (%s)", i, list[i].hour, list[i].minute,
                                     list[i].enabled ? "ON " : "OFF",
                                     list[i].repeat_daily ? "Daily" : "Once ", list[i].label);
                        }
                    }
                }
                pos = 0;
            }
        } else if (pos + 1 < sizeof(line)) {
            line[pos++] = (char)ch;
        }
    }
}

esp_err_t ai_voice_alarm_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    set_status(AI_VOICE_IDLE, "", "Tap Mic to Speak");
    xTaskCreate(console_ai_task, "console_ai", 4096, NULL, 2, NULL);
    return ESP_OK;
}

esp_err_t ai_voice_alarm_start_listening(void)
{
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;
    xTaskCreate(voice_capture_task, "ai_voice", 8192, NULL, 4, NULL);
    return ESP_OK;
}

void ai_voice_alarm_get_status(ai_voice_status_t *out)
{
    if (!out || !s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mutex);
}
