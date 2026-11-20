/*
 * Waveshare ESP32-C6-Touch-LCD-1.83
 * Smart Calendar, Weather & AI Voice Timed Alarm
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

#include "services/wifi_time.h"
#include "services/weather_service.h"
#include "services/alarm_manager.h"
#include "services/ai_voice_alarm.h"

#include "ui/app_ui.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-C6-Touch-LCD-1.83 Smart Calendar, Weather & AI Alarm ===");

    // 1. NVS (required for Wi-Fi & persistent alarms)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // 2. Shared I2C bus (GPIO7 SDA, GPIO8 SCL) & PMIC (AXP2101) first so all rails are powered
    ESP_ERROR_CHECK(board_i2c_init());
    axp2101_init();
    vTaskDelay(pdMS_TO_TICKS(50));

    // 3. Hardware RTC (PCF85063), LCD (ST7789P + unified GPIO4 reset), Touch (CST816D), Audio (ES8311+ES7210)
    pcf85063_init();
    ESP_ERROR_CHECK(lcd_st7789_init());
    touch_cst816d_init();
    audio_codec_init();

    // 4. Core Services: Wi-Fi + SNTP, Weather (Open-Meteo), Alarm Manager, AI Voice & Serial NLP
    wifi_time_init();
    weather_service_start();
    alarm_manager_init();
    ai_voice_alarm_init();

    // 5. Touch UI Loop
    app_ui_init();
    ESP_LOGI(TAG, "System ready. Tap 'AI Mic' or press BOOT (GPIO9) for voice alarm, or type 'ai <cmd>' in monitor.");

    while (1) {
        app_ui_tick();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
