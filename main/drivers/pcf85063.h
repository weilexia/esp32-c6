#pragma once

#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pcf85063_init(void);
esp_err_t pcf85063_get_time(struct tm *out_tm);
esp_err_t pcf85063_set_time(const struct tm *in_tm);

#ifdef __cplusplus
}
#endif
