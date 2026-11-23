#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char ai_api_key[128];
    char ai_stt_url[128];
    char ai_stt_model[64];
    char ai_llm_url[128];
    char ai_llm_model[64];
    char deepseek_api_key[128];
    char deepseek_daily_budget[16];
    uint8_t brightness;       // 10..100 (%)
    uint16_t auto_dim_sec;    // 0 = disabled, else idle seconds before dimming to 5%
    bool muted;               // Global audio mute (disables speaker PA)
} app_config_t;

esp_err_t app_config_init(void);
void app_config_get(app_config_t *out);
esp_err_t app_config_save(const app_config_t *cfg);
esp_err_t app_config_set_display(uint8_t brightness, uint16_t auto_dim_sec);
esp_err_t app_config_set_mute(bool muted);

#ifdef __cplusplus
}
#endif
