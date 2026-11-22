#pragma once

#include <stdbool.h>
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
} app_config_t;

esp_err_t app_config_init(void);
void app_config_get(app_config_t *out);
esp_err_t app_config_save(const app_config_t *cfg);

#ifdef __cplusplus
}
#endif
