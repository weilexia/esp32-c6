#include "deepseek_service.h"
#include "app_config.h"
#include "wifi_time.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "deepseek";
static const char *NVS_NS = "ds_stat";

// Approximate blended price for DeepSeek-V3 (~2.0 RMB per 1M tokens) when estimating tokens from balance delta
#define DEEPSEEK_EST_TOKENS_PER_RMB 500000.0f
#define DEEPSEEK_POLL_INTERVAL_MS   (60 * 1000)

static deepseek_usage_t s_usage = {0};
static uint32_t s_stat_date = 0; // YYYYMMDD
static bool s_webhook_cost_active = false;
static SemaphoreHandle_t s_mutex = NULL;
static volatile bool s_force_refresh = false;

static uint32_t get_today_yyyymmdd(void)
{
    struct tm now = {0};
    wifi_time_get_local(&now);
    return (uint32_t)((now.tm_year + 1900) * 10000 + (now.tm_mon + 1) * 100 + now.tm_mday);
}

static void save_nvs_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u32(h, "date", s_stat_date);
    nvs_set_u32(h, "calls", s_usage.calls);
    nvs_set_u32(h, "errors", s_usage.errors);
    nvs_set_u64(h, "p_tok", s_usage.prompt_tokens);
    nvs_set_u64(h, "c_tok", s_usage.completion_tokens);
    nvs_set_u64(h, "t_tok", s_usage.total_tokens);
    nvs_set_u8(h, "tok_est", s_usage.tokens_estimated ? 1 : 0);
    nvs_set_u8(h, "wh_cost", s_webhook_cost_active ? 1 : 0);
    nvs_set_blob(h, "start_bal", &s_usage.day_start_balance, sizeof(float));
    nvs_set_blob(h, "today_cost", &s_usage.today_cost, sizeof(float));
    nvs_commit(h);
    nvs_close(h);
}

static void load_nvs_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint32_t saved_date = 0;
    if (nvs_get_u32(h, "date", &saved_date) == ESP_OK) {
        uint32_t today = get_today_yyyymmdd();
        if (saved_date == today || today < 20250101) {
            s_stat_date = saved_date;
            nvs_get_u32(h, "calls", &s_usage.calls);
            nvs_get_u32(h, "errors", &s_usage.errors);
            nvs_get_u64(h, "p_tok", &s_usage.prompt_tokens);
            nvs_get_u64(h, "c_tok", &s_usage.completion_tokens);
            nvs_get_u64(h, "t_tok", &s_usage.total_tokens);
            uint8_t u8 = 0;
            if (nvs_get_u8(h, "tok_est", &u8) == ESP_OK) s_usage.tokens_estimated = (u8 != 0);
            if (nvs_get_u8(h, "wh_cost", &u8) == ESP_OK) s_webhook_cost_active = (u8 != 0);
            size_t sz = sizeof(float);
            nvs_get_blob(h, "start_bal", &s_usage.day_start_balance, &sz);
            sz = sizeof(float);
            nvs_get_blob(h, "today_cost", &s_usage.today_cost, &sz);
            ESP_LOGI(TAG, "Restored today DeepSeek stats: calls=%lu tokens=%llu cost=%.2f",
                     (unsigned long)s_usage.calls,
                     (unsigned long long)s_usage.total_tokens,
                     s_usage.today_cost);
        }
    }
    nvs_close(h);
}

static void check_day_rollover_locked(void)
{
    uint32_t today = get_today_yyyymmdd();
    if (today < 20250101) {
        return;
    }
    if (s_stat_date != 0 && s_stat_date != today) {
        ESP_LOGI(TAG, "New day detected (%lu -> %lu), resetting daily DeepSeek stats",
                 (unsigned long)s_stat_date, (unsigned long)today);
        s_usage.calls = 0;
        s_usage.errors = 0;
        s_usage.prompt_tokens = 0;
        s_usage.completion_tokens = 0;
        s_usage.total_tokens = 0;
        s_usage.today_cost = 0.0f;
        s_usage.day_start_balance = s_usage.balance_valid ? s_usage.total_balance : 0.0f;
        s_usage.tokens_estimated = false;
        s_webhook_cost_active = false;
        s_stat_date = today;
        save_nvs_locked();
    } else if (s_stat_date == 0) {
        s_stat_date = today;
    }
}

static bool json_extract_string(const char *json, const char *key, char *out, size_t out_sz)
{
    char pat[40];
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
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

static bool json_extract_bool(const char *json, const char *key, bool *out)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool json_extract_num_or_str_float(const char *json, const char *key, float *out)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '"') p++;
    char *end = NULL;
    float v = strtof(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

static bool fetch_deepseek_balance(const char *api_key)
{
    esp_http_client_config_t cfg = {
        .url = "https://api.deepseek.com/user/balance",
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return false;
    }

    char auth_hdr[160];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_header(client, "Accept", "application/json");

    char *buf = malloc(1024);
    if (!buf) {
        esp_http_client_cleanup(client);
        return false;
    }

    bool ok = false;
    int status = 0;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        int total = 0;
        int r = 0;
        while (total < 1023 && (r = esp_http_client_read(client, buf + total, 1023 - total)) > 0) {
            total += r;
        }
        buf[total] = '\0';

        if (status == 200 && total > 10) {
            bool avail = false;
            float total_bal = 0.0f, granted_bal = 0.0f, topped_bal = 0.0f;
            char curr[8] = "CNY";

            json_extract_bool(buf, "is_available", &avail);
            json_extract_string(buf, "currency", curr, sizeof(curr));
            bool got_bal = json_extract_num_or_str_float(buf, "total_balance", &total_bal);
            json_extract_num_or_str_float(buf, "granted_balance", &granted_bal);
            json_extract_num_or_str_float(buf, "topped_up_balance", &topped_bal);

            if (got_bal) {
                struct tm now = {0};
                wifi_time_get_local(&now);

                xSemaphoreTake(s_mutex, portMAX_DELAY);
                check_day_rollover_locked();
                s_usage.balance_valid = true;
                s_usage.is_available = avail;
                strncpy(s_usage.currency, curr, sizeof(s_usage.currency) - 1);
                s_usage.total_balance = total_bal;
                s_usage.granted_balance = granted_bal;
                s_usage.topped_up_balance = topped_bal;

                if (s_usage.day_start_balance <= 0.0001f || total_bal > s_usage.day_start_balance + 0.01f) {
                    // First sample today or user topped up balance
                    s_usage.day_start_balance = total_bal;
                }

                float delta = s_usage.day_start_balance - total_bal;
                if (delta < 0.0f) delta = 0.0f;

                if (!s_webhook_cost_active) {
                    s_usage.today_cost = delta;
                    if (s_usage.tokens_estimated || s_usage.total_tokens == 0) {
                        s_usage.total_tokens = (uint64_t)(delta * DEEPSEEK_EST_TOKENS_PER_RMB);
                        s_usage.tokens_estimated = true;
                    }
                }

                snprintf(s_usage.last_sync, sizeof(s_usage.last_sync), "%02d:%02d",
                         now.tm_hour & 0xFF, now.tm_min & 0xFF);
                strncpy(s_usage.status_text, avail ? "ONLINE" : "LOW BAL", sizeof(s_usage.status_text) - 1);
                save_nvs_locked();
                xSemaphoreGive(s_mutex);

                ESP_LOGI(TAG, "DeepSeek balance: %s %.2f (granted=%.2f, topped=%.2f, today=%.2f)",
                         curr, total_bal, granted_bal, topped_bal, s_usage.today_cost);
                ok = true;
            }
        } else {
            ESP_LOGW(TAG, "DeepSeek API HTTP status=%d", status);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            snprintf(s_usage.status_text, sizeof(s_usage.status_text), "HTTP %d", status);
            xSemaphoreGive(s_mutex);
        }
    } else {
        ESP_LOGW(TAG, "Failed to connect to api.deepseek.com");
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        strncpy(s_usage.status_text, "NET ERR", sizeof(s_usage.status_text) - 1);
        xSemaphoreGive(s_mutex);
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

static void deepseek_task(void *arg)
{
    (void)arg;
    TickType_t last_fetch = 0;
    const TickType_t interval = pdMS_TO_TICKS(DEEPSEEK_POLL_INTERVAL_MS);

    while (1) {
        app_config_t cfg;
        app_config_get(&cfg);

        float budget = strtof(cfg.deepseek_daily_budget, NULL);
        if (budget <= 0.01f) budget = 20.0f;

        const char *key = cfg.deepseek_api_key[0] ? cfg.deepseek_api_key : cfg.ai_api_key;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_usage.daily_budget = budget;
        if (!key || key[0] == '\0') {
            if (!s_webhook_cost_active && s_usage.calls == 0) {
                strncpy(s_usage.status_text, "NO KEY", sizeof(s_usage.status_text) - 1);
            }
        }
        xSemaphoreGive(s_mutex);

        bool due = (last_fetch == 0) || ((xTaskGetTickCount() - last_fetch) >= interval) || s_force_refresh;
        if (wifi_time_is_connected() && due && key && key[0] != '\0') {
            s_force_refresh = false;
            fetch_deepseek_balance(key);
            last_fetch = xTaskGetTickCount();
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t deepseek_service_start(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_usage, 0, sizeof(s_usage));
    strncpy(s_usage.currency, "CNY", sizeof(s_usage.currency) - 1);
    strncpy(s_usage.status_text, "WAIT", sizeof(s_usage.status_text) - 1);
    s_usage.daily_budget = 20.0f;
    load_nvs_locked();
    xSemaphoreGive(s_mutex);

    xTaskCreate(deepseek_task, "ds_task", 6144, NULL, 3, NULL);
    return ESP_OK;
}

void deepseek_service_get_usage(deepseek_usage_t *out)
{
    if (!out || !s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_usage;
    xSemaphoreGive(s_mutex);
}

void deepseek_service_request_refresh(void)
{
    s_force_refresh = true;
}

void deepseek_service_set_usage(uint32_t calls, uint32_t errors,
                                uint64_t prompt_tokens, uint64_t completion_tokens,
                                uint64_t total_tokens, float cost_rmb, bool has_cost)
{
    if (!s_mutex) return;
    struct tm now = {0};
    wifi_time_get_local(&now);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    check_day_rollover_locked();
    s_usage.calls = calls;
    s_usage.errors = errors;
    s_usage.prompt_tokens = prompt_tokens;
    s_usage.completion_tokens = completion_tokens;
    s_usage.total_tokens = (total_tokens > 0) ? total_tokens : (prompt_tokens + completion_tokens);
    s_usage.tokens_estimated = false;
    if (has_cost) {
        s_usage.today_cost = cost_rmb;
        s_webhook_cost_active = true;
    }
    snprintf(s_usage.last_sync, sizeof(s_usage.last_sync), "%02d:%02d",
             now.tm_hour & 0xFF, now.tm_min & 0xFF);
    strncpy(s_usage.status_text, "ONLINE", sizeof(s_usage.status_text) - 1);
    save_nvs_locked();
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Usage pushed: calls=%lu err=%lu tokens=%llu cost=%.2f",
             (unsigned long)calls, (unsigned long)errors,
             (unsigned long long)s_usage.total_tokens, s_usage.today_cost);
}

void deepseek_service_add_call(uint64_t prompt_tokens, uint64_t completion_tokens,
                               float cost_rmb, bool is_error)
{
    if (!s_mutex) return;
    struct tm now = {0};
    wifi_time_get_local(&now);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    check_day_rollover_locked();
    if (s_usage.tokens_estimated) {
        // Switch from estimated mode to exact mode cleanly
        s_usage.total_tokens = 0;
        s_usage.prompt_tokens = 0;
        s_usage.completion_tokens = 0;
        s_usage.tokens_estimated = false;
    }
    s_usage.calls++;
    if (is_error) s_usage.errors++;
    s_usage.prompt_tokens += prompt_tokens;
    s_usage.completion_tokens += completion_tokens;
    s_usage.total_tokens += (prompt_tokens + completion_tokens);
    if (cost_rmb > 0.0f) {
        if (!s_webhook_cost_active) {
            s_usage.today_cost = 0.0f;
            s_webhook_cost_active = true;
        }
        s_usage.today_cost += cost_rmb;
    }
    snprintf(s_usage.last_sync, sizeof(s_usage.last_sync), "%02d:%02d",
             now.tm_hour & 0xFF, now.tm_min & 0xFF);
    strncpy(s_usage.status_text, "ONLINE", sizeof(s_usage.status_text) - 1);
    save_nvs_locked();
    xSemaphoreGive(s_mutex);
}

void deepseek_service_reset_today(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_usage.calls = 0;
    s_usage.errors = 0;
    s_usage.prompt_tokens = 0;
    s_usage.completion_tokens = 0;
    s_usage.total_tokens = 0;
    s_usage.today_cost = 0.0f;
    s_usage.day_start_balance = s_usage.balance_valid ? s_usage.total_balance : 0.0f;
    s_usage.tokens_estimated = false;
    s_webhook_cost_active = false;
    save_nvs_locked();
    xSemaphoreGive(s_mutex);
}

void deepseek_format_tokens(uint64_t tokens, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    if (tokens >= 1000000ULL) {
        snprintf(out, out_sz, "%.2fM", (double)tokens / 1000000.0);
    } else if (tokens >= 10000ULL) {
        snprintf(out, out_sz, "%.1fK", (double)tokens / 1000.0);
    } else if (tokens >= 1000ULL) {
        snprintf(out, out_sz, "%.2fK", (double)tokens / 1000.0);
    } else {
        snprintf(out, out_sz, "%llu", (unsigned long long)tokens);
    }
}
