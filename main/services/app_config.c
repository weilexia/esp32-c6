#include "app_config.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "app_config";
static const char *NVS_NS = "app_cfg";

static app_config_t s_cfg;
static SemaphoreHandle_t s_mutex = NULL;

static void copy_str(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
}

static void load_nvs_str(nvs_handle_t h, const char *key, char *dst, size_t dst_sz)
{
    size_t len = dst_sz;
    if (nvs_get_str(h, key, dst, &len) == ESP_OK) {
        dst[dst_sz - 1] = '\0';
    }
}

esp_err_t app_config_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    // 1. Seed defaults from menuconfig (sdkconfig.h)
    memset(&s_cfg, 0, sizeof(s_cfg));
    copy_str(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), CONFIG_APP_WIFI_SSID);
    copy_str(s_cfg.wifi_password, sizeof(s_cfg.wifi_password), CONFIG_APP_WIFI_PASSWORD);
    copy_str(s_cfg.ai_api_key, sizeof(s_cfg.ai_api_key), CONFIG_APP_AI_API_KEY);
    copy_str(s_cfg.ai_stt_url, sizeof(s_cfg.ai_stt_url), CONFIG_APP_AI_STT_URL);
    copy_str(s_cfg.ai_stt_model, sizeof(s_cfg.ai_stt_model), CONFIG_APP_AI_STT_MODEL);
    copy_str(s_cfg.ai_llm_url, sizeof(s_cfg.ai_llm_url), CONFIG_APP_AI_LLM_URL);
    copy_str(s_cfg.ai_llm_model, sizeof(s_cfg.ai_llm_model), CONFIG_APP_AI_LLM_MODEL);
    copy_str(s_cfg.deepseek_api_key, sizeof(s_cfg.deepseek_api_key), CONFIG_APP_DEEPSEEK_API_KEY);
    copy_str(s_cfg.deepseek_daily_budget, sizeof(s_cfg.deepseek_daily_budget), CONFIG_APP_DEEPSEEK_DAILY_BUDGET);
    s_cfg.brightness = 80;
    s_cfg.auto_dim_sec = 60;

    // 2. Override with persisted NVS values if present.
    // If Kconfig Wi-Fi SSID changed since last boot, apply the new compile-time Wi-Fi once.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        char last_kcfg_ssid[33] = {0};
        load_nvs_str(h, "kcfg_ssid", last_kcfg_ssid, sizeof(last_kcfg_ssid));
        bool kcfg_wifi_changed = (CONFIG_APP_WIFI_SSID[0] != '\0' &&
                                  strcmp(CONFIG_APP_WIFI_SSID, last_kcfg_ssid) != 0);

        if (kcfg_wifi_changed) {
            nvs_set_str(h, "wifi_ssid", s_cfg.wifi_ssid);
            nvs_set_str(h, "wifi_pass", s_cfg.wifi_password);
            nvs_set_str(h, "kcfg_ssid", CONFIG_APP_WIFI_SSID);
            nvs_commit(h);
            ESP_LOGI(TAG, "Applied updated Kconfig Wi-Fi SSID='%s' to NVS", s_cfg.wifi_ssid);
        } else {
            load_nvs_str(h, "wifi_ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
            load_nvs_str(h, "wifi_pass", s_cfg.wifi_password, sizeof(s_cfg.wifi_password));
        }
        load_nvs_str(h, "ai_key", s_cfg.ai_api_key, sizeof(s_cfg.ai_api_key));
        load_nvs_str(h, "stt_url", s_cfg.ai_stt_url, sizeof(s_cfg.ai_stt_url));
        load_nvs_str(h, "stt_model", s_cfg.ai_stt_model, sizeof(s_cfg.ai_stt_model));
        load_nvs_str(h, "llm_url", s_cfg.ai_llm_url, sizeof(s_cfg.ai_llm_url));
        load_nvs_str(h, "llm_model", s_cfg.ai_llm_model, sizeof(s_cfg.ai_llm_model));
        load_nvs_str(h, "ds_key", s_cfg.deepseek_api_key, sizeof(s_cfg.deepseek_api_key));
        load_nvs_str(h, "ds_budget", s_cfg.deepseek_daily_budget, sizeof(s_cfg.deepseek_daily_budget));
        uint8_t bl = 80;
        if (nvs_get_u8(h, "bl_pct", &bl) == ESP_OK && bl >= 10 && bl <= 100) {
            s_cfg.brightness = bl;
        }
        uint16_t dim = 60;
        if (nvs_get_u16(h, "dim_sec", &dim) == ESP_OK) {
            s_cfg.auto_dim_sec = dim;
        }
        nvs_close(h);
        ESP_LOGI(TAG, "Loaded runtime config (SSID='%s', DSKey=%s)",
                 s_cfg.wifi_ssid, s_cfg.deepseek_api_key[0] ? "set" : "empty");
    } else {
        ESP_LOGI(TAG, "No NVS config yet, using Kconfig defaults (SSID='%s')", s_cfg.wifi_ssid);
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void app_config_get(app_config_t *out)
{
    if (!out) return;
    if (!s_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_mutex);
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    if (!cfg || !s_mutex) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cfg = *cfg;
    nvs_set_str(h, "wifi_ssid", s_cfg.wifi_ssid);
    nvs_set_str(h, "wifi_pass", s_cfg.wifi_password);
    nvs_set_str(h, "ai_key", s_cfg.ai_api_key);
    nvs_set_str(h, "stt_url", s_cfg.ai_stt_url);
    nvs_set_str(h, "stt_model", s_cfg.ai_stt_model);
    nvs_set_str(h, "llm_url", s_cfg.ai_llm_url);
    nvs_set_str(h, "llm_model", s_cfg.ai_llm_model);
    nvs_set_str(h, "ds_key", s_cfg.deepseek_api_key);
    nvs_set_str(h, "ds_budget", s_cfg.deepseek_daily_budget);
    nvs_set_u8(h, "bl_pct", s_cfg.brightness);
    nvs_set_u16(h, "dim_sec", s_cfg.auto_dim_sec);
    err = nvs_commit(h);
    xSemaphoreGive(s_mutex);

    nvs_close(h);
    ESP_LOGI(TAG, "Saved runtime config to NVS (SSID='%s', Key=%s)",
             cfg->wifi_ssid, cfg->ai_api_key[0] ? "set" : "empty");
    return err;
}

esp_err_t app_config_set_display(uint8_t brightness, uint16_t auto_dim_sec)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;
    if (brightness < 10) brightness = 10;
    if (brightness > 100) brightness = 100;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cfg.brightness = brightness;
    s_cfg.auto_dim_sec = auto_dim_sec;
    xSemaphoreGive(s_mutex);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_u8(h, "bl_pct", brightness);
        nvs_set_u16(h, "dim_sec", auto_dim_sec);
        err = nvs_commit(h);
        nvs_close(h);
    }
    return err;
}
