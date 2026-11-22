#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_PORTAL_AP_SSID "ESP32-UsageDeck"
#define WEB_PORTAL_AP_IP   "192.168.1.4"

esp_err_t web_portal_start(void);
esp_err_t web_portal_stop(void);
bool web_portal_is_active(void);
const char *web_portal_get_ap_ssid(void);
const char *web_portal_get_ap_ip(void);

#ifdef __cplusplus
}
#endif
