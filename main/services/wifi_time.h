#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_time_init(void);
bool wifi_time_is_connected(void);
bool wifi_time_is_synced(void);
void wifi_time_get_local(struct tm *out_tm);

#ifdef __cplusplus
}
#endif
