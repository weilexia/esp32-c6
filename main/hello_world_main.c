/*
 * Waveshare ESP32-C6-Touch-LCD-1.83
 * System Alert Monitoring Dashboard & Voice Broadcast Terminal
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "drivers/board_i2c.h"
#include "drivers/axp2101.h"
#include "drivers/pcf85063.h"
#include "drivers/lcd_st7789.h"
#include "drivers/touch_cst816d.h"
#include "drivers/audio_codec.h"

#include "services/alert_server.h"
#include "services/app_config.h"
#include "services/ble_ancs.h"
#include "services/deepseek_service.h"
#include "services/wifi_time.h"
#include "services/weather_service.h"

#include "ui/app_ui.h"

static const char *TAG = "main";

static void delayed_ble_init_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));
    ble_ancs_init();
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-C6 Alert Monitoring Dashboard ===");

    // 1. NVS (required for Wi-Fi & runtime configuration)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // 2. Shared I2C bus (GPIO7 SDA, GPIO8 SCL) & PMIC (AXP2101)
    ESP_ERROR_CHECK(board_i2c_init());
    axp2101_init();
    vTaskDelay(pdMS_TO_TICKS(50));

    // 3. Hardware RTC, LCD, Touch, Audio
    pcf85063_init();
    ESP_ERROR_CHECK(lcd_st7789_init());
    touch_cst816d_init();
    audio_codec_init();

    // 4. Core Services: Config, Alert Dashboard + Console, Wi-Fi + SNTP, Weather, BLE ANCS (delayed 6s)
    app_config_init();
    alert_server_init();
    wifi_time_init();
    xTaskCreate(delayed_ble_init_task, "ble_delay", 4096, NULL, 3, NULL);
    weather_service_start();
    deepseek_service_start();

    // 5. Touch Dashboard UI Loop (raise priority so background tasks never stutter LVGL swipes)
    vTaskPrioritySet(NULL, 4);
    app_ui_init();
    ESP_LOGI(TAG, "Dashboard ready. Type 'ip', 'test', 'stop', or 'setup' in monitor.");

    while (1) {
        app_ui_tick();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
