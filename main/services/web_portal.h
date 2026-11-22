#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_PORTAL_AP_SSID "ESP32-Clock-Setup"

esp_err_t web_portal_start(void);
bool web_portal_is_active(void);
const char *web_portal_get_ap_ssid(void);

#ifdef __cplusplus
}
#endif
