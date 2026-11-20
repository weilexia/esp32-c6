#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t axp2101_init(void);
int axp2101_get_battery_percent(void);
bool axp2101_is_charging(void);
bool axp2101_is_vbus_present(void);

#ifdef __cplusplus
}
#endif
