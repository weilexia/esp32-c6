#include "weather_service.h"
#include "wifi_time.h"
#include "sdkconfig.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "weather";

static weather_data_t s_weather = {0};
static SemaphoreHandle_t s_mutex = NULL;
static volatile bool s_force_refresh = false;

const char *weather_code_to_desc(int wmo_code)
{
    if (wmo_code == 0) return "Clear Sky";
    if (wmo_code <= 2) return "Partly Cloudy";
    if (wmo_code == 3) return "Overcast";
    if (wmo_code == 45 || wmo_code == 48) return "Foggy";
    if (wmo_code >= 51 && wmo_code <= 57) return "Drizzle";
    if (wmo_code >= 61 && wmo_code <= 67) return "Rainy";
    if (wmo_code >= 71 && wmo_code <= 77) return "Snowy";
    if (wmo_code >= 80 && wmo_code <= 82) return "Showers";
    if (wmo_code >= 85 && wmo_code <= 86) return "Snow Showers";
    if (wmo_code >= 95) return "Thunderstorm";
    return "Cloudy";
}

// Lightweight zero-dependency JSON number extractor
static bool json_find_float_in_section(const char *json, const char *section, const char *key, float *out_val)
{
    const char *p = section ? strstr(json, section) : json;
    if (!p) return false;
    p = strstr(p, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    float v = strtof(p, &end);
    if (end == p) return false;
    *out_val = v;
    return true;
}

static int json_find_float_array(const char *json, const char *section, const char *key, float *arr, int max_n)
{
    const char *p = section ? strstr(json, section) : json;
    if (!p) return 0;
    p = strstr(p, key);
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;
    int count = 0;
    while (*p && *p != ']' && count < max_n) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (*p == ']') break;
        char *end = NULL;
        arr[count] = strtof(p, &end);
        if (end == p) break;
        count++;
        p = end;
    }
    return count;
}

static bool fetch_open_meteo(weather_data_t *out)
{
    char url[280];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min"
             "&timezone=auto&forecast_days=3",
             CONFIG_APP_WEATHER_LAT, CONFIG_APP_WEATHER_LON);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return false;
    }

    char *buf = malloc(2048);
    if (!buf) {
        esp_http_client_cleanup(client);
        return false;
    }

    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        int total = 0;
        int r = 0;
        while (total < 2047 && (r = esp_http_client_read(client, buf + total, 2047 - total)) > 0) {
            total += r;
        }
        buf[total] = '\0';

        if (status == 200 && total > 32) {
            float temp = 0, hum = 0, code = 0, wind = 0;
            if (json_find_float_in_section(buf, "\"current\":", "\"temperature_2m\"", &temp)) {
                json_find_float_in_section(buf, "\"current\":", "\"relative_humidity_2m\"", &hum);
                json_find_float_in_section(buf, "\"current\":", "\"weather_code\"", &code);
                json_find_float_in_section(buf, "\"current\":", "\"wind_speed_10m\"", &wind);

                out->temp_c = temp;
                out->humidity_pct = (int)hum;
                out->weather_code = (int)code;
                out->wind_kmh = wind;
                strncpy(out->desc, weather_code_to_desc(out->weather_code), sizeof(out->desc) - 1);

                float dmax[3] = {temp, temp, temp};
                float dmin[3] = {temp, temp, temp};
                float dcode[3] = {code, code, code};
                json_find_float_array(buf, "\"daily\":", "\"temperature_2m_max\"", dmax, 3);
                json_find_float_array(buf, "\"daily\":", "\"temperature_2m_min\"", dmin, 3);
                json_find_float_array(buf, "\"daily\":", "\"weather_code\"", dcode, 3);
                for (int i = 0; i < 3; i++) {
                    out->daily_max[i] = dmax[i];
                    out->daily_min[i] = dmin[i];
                    out->daily_code[i] = (int)dcode[i];
                }
                out->valid = true;
                ok = true;
                ESP_LOGI(TAG, "Weather updated: %s %.1fC, %s, RH %d%%", out->city, out->temp_c, out->desc, out->humidity_pct);
            }
        } else {
            ESP_LOGW(TAG, "Open-Meteo HTTP status=%d len=%d", status, total);
        }
    } else {
        ESP_LOGW(TAG, "Failed to connect to Open-Meteo");
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

static void weather_task(void *arg)
{
    (void)arg;
    TickType_t last_fetch = 0;
    const TickType_t interval = pdMS_TO_TICKS((uint32_t)CONFIG_APP_WEATHER_REFRESH_MIN * 60 * 1000);

    while (1) {
        bool due = (last_fetch == 0) || ((xTaskGetTickCount() - last_fetch) >= interval) || s_force_refresh;
        if (wifi_time_is_connected() && due) {
            s_force_refresh = false;
            weather_data_t tmp = {0};
            strncpy(tmp.city, CONFIG_APP_WEATHER_CITY, sizeof(tmp.city) - 1);
            if (fetch_open_meteo(&tmp)) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_weather = tmp;
                xSemaphoreGive(s_mutex);
                last_fetch = xTaskGetTickCount();
            } else {
                // Retry in 60s on failure
                vTaskDelay(pdMS_TO_TICKS(60000));
                continue;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t weather_service_start(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_weather, 0, sizeof(s_weather));
    strncpy(s_weather.city, CONFIG_APP_WEATHER_CITY, sizeof(s_weather.city) - 1);
    strncpy(s_weather.desc, "Offline", sizeof(s_weather.desc) - 1);
    xSemaphoreGive(s_mutex);

    xTaskCreate(weather_task, "weather_task", 6144, NULL, 3, NULL);
    return ESP_OK;
}

void weather_service_request_refresh(void)
{
    s_force_refresh = true;
}

void weather_service_get_data(weather_data_t *out)
{
    if (!out || !s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_weather;
    xSemaphoreGive(s_mutex);
}
