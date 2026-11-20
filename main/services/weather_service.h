#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    float temp_c;
    int humidity_pct;
    float wind_kmh;
    int weather_code;
    float daily_max[3];
    float daily_min[3];
    int daily_code[3];
    char city[24];
    char desc[20];
} weather_data_t;

esp_err_t weather_service_start(void);
void weather_service_request_refresh(void);
void weather_service_get_data(weather_data_t *out);
const char *weather_code_to_desc(int wmo_code);

#ifdef __cplusplus
}
#endif
