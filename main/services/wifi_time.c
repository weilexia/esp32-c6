#include "wifi_time.h"
#include "app_config.h"
#include "web_portal.h"
#include "pcf85063.h"
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "wifi_time";

static volatile bool s_wifi_connected = false;
static volatile bool s_time_synced = false;
static bool s_sntp_started = false;
static int s_retry_count = 0;
#define WIFI_MAX_RETRY_BEFORE_PORTAL 5

static void portal_launch_task(void *arg)
{
    (void)arg;
    web_portal_start();
    vTaskDelete(NULL);
}

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    struct tm now_tm = {0};
    wifi_time_get_local(&now_tm);
    ESP_LOGI(TAG, "SNTP synced: %04d-%02d-%02d %02d:%02d:%02d",
             now_tm.tm_year + 1900, now_tm.tm_mon + 1, now_tm.tm_mday,
             now_tm.tm_hour, now_tm.tm_min, now_tm.tm_sec);
    // Persist synced time to battery-backed PCF85063 hardware RTC
    pcf85063_set_time(&now_tm);
}

static void start_sntp_once(void)
{
    if (s_sntp_started) {
        return;
    }
    s_sntp_started = true;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started");
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (web_portal_is_active()) {
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        s_retry_count++;
        if (s_retry_count >= WIFI_MAX_RETRY_BEFORE_PORTAL) {
            ESP_LOGW(TAG, "Wi-Fi failed %d times, switching to SoftAP setup portal...", s_retry_count);
            xTaskCreate(portal_launch_task, "portal_go", 4096, NULL, 4, NULL);
        } else {
            ESP_LOGW(TAG, "Wi-Fi disconnected (%d/%d), retrying...", s_retry_count, WIFI_MAX_RETRY_BEFORE_PORTAL);
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = true;
        s_retry_count = 0;
        ESP_LOGI(TAG, "Wi-Fi connected with IP");
        start_sntp_once();
    }
}

esp_err_t wifi_time_init(void)
{
    // Set configured POSIX timezone
    setenv("TZ", CONFIG_APP_TIMEZONE, 1);
    tzset();

    // Seed system clock from hardware PCF85063 RTC (or fallback date if uninitialized)
    struct tm rtc_tm = {0};
    if (pcf85063_get_time(&rtc_tm) == ESP_OK) {
        time_t t = mktime(&rtc_tm);
        struct timeval tv = {.tv_sec = t, .tv_usec = 0};
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "System clock seeded from PCF85063: %04d-%02d-%02d %02d:%02d:%02d",
                 rtc_tm.tm_year + 1900, rtc_tm.tm_mon + 1, rtc_tm.tm_mday,
                 rtc_tm.tm_hour, rtc_tm.tm_min, rtc_tm.tm_sec);
    } else {
        struct tm def_tm = {
            .tm_year = 2026 - 1900,
            .tm_mon = 10, // November
            .tm_mday = 20,
            .tm_hour = 8,
            .tm_min = 0,
            .tm_sec = 0,
        };
        time_t t = mktime(&def_tm);
        struct timeval tv = {.tv_sec = t, .tv_usec = 0};
        settimeofday(&tv, NULL);
        ESP_LOGW(TAG, "PCF85063 uninitialized, using fallback time until SNTP sync");
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    app_config_t app_cfg;
    app_config_get(&app_cfg);

    if (strlen(app_cfg.wifi_ssid) == 0) {
        ESP_LOGW(TAG, "No Wi-Fi SSID configured — starting SoftAP setup portal (http://192.168.4.1)");
        web_portal_start();
        return ESP_OK;
    }

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, app_cfg.wifi_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, app_cfg.wifi_password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = strlen(app_cfg.wifi_password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi STA starting for SSID: %s", app_cfg.wifi_ssid);
    return ESP_OK;
}

bool wifi_time_is_connected(void)
{
    return s_wifi_connected;
}

bool wifi_time_is_synced(void)
{
    return s_time_synced;
}

void wifi_time_get_local(struct tm *out_tm)
{
    time_t now = 0;
    time(&now);
    localtime_r(&now, out_tm);
}
