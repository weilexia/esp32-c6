#include "alarm_manager.h"
#include "audio_codec.h"
#include "wifi_time.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "alarm_mgr";

static alarm_entry_t s_alarms[ALARM_MAX_COUNT];
static SemaphoreHandle_t s_mutex = NULL;
static volatile bool s_ringing = false;
static alarm_entry_t s_ringing_copy = {0};

static void save_to_nvs_locked(void)
{
    nvs_handle_t h = 0;
    if (nvs_open("alarms", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "list", s_alarms, sizeof(s_alarms));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load_from_nvs_locked(void)
{
    nvs_handle_t h = 0;
    if (nvs_open("alarms", NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_alarms);
        if (nvs_get_blob(h, "list", s_alarms, &sz) == ESP_OK && sz == sizeof(s_alarms)) {
            ESP_LOGI(TAG, "Loaded saved alarms from NVS");
        }
        nvs_close(h);
    }
}

static bool stop_ring_cb(void)
{
    return !s_ringing;
}

static void alarm_task(void *arg)
{
    (void)arg;
    TickType_t ring_start = 0;

    while (1) {
        if (s_ringing) {
            audio_play_alarm_chime(stop_ring_cb);
            if ((xTaskGetTickCount() - ring_start) > pdMS_TO_TICKS(60000)) {
                ESP_LOGI(TAG, "Alarm auto-stopped after 60s");
                s_ringing = false;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        struct tm now = {0};
        wifi_time_get_local(&now);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        for (int i = 0; i < ALARM_MAX_COUNT; i++) {
            alarm_entry_t *a = &s_alarms[i];
            if (!a->used || !a->enabled) {
                continue;
            }
            if (a->hour == (uint8_t)now.tm_hour && a->minute == (uint8_t)now.tm_min && now.tm_sec <= 5) {
                if (a->last_fired_mday != (uint8_t)now.tm_mday || a->last_fired_min != (uint8_t)now.tm_min) {
                    a->last_fired_mday = (uint8_t)now.tm_mday;
                    a->last_fired_min = (uint8_t)now.tm_min;
                    s_ringing_copy = *a;
                    if (!a->repeat_daily) {
                        a->enabled = false;
                    }
                    save_to_nvs_locked();
                    s_ringing = true;
                    ring_start = xTaskGetTickCount();
                    ESP_LOGI(TAG, "ALARM TRIGGERED: %02u:%02u (%s)", a->hour, a->minute, a->label);
                    break;
                }
            }
        }
        xSemaphoreGive(s_mutex);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t alarm_manager_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_alarms, 0, sizeof(s_alarms));
    load_from_nvs_locked();
    xSemaphoreGive(s_mutex);

    xTaskCreate(alarm_task, "alarm_task", 4096, NULL, 5, NULL);
    return ESP_OK;
}

int alarm_manager_add(uint8_t hour, uint8_t minute, bool repeat_daily, const char *label)
{
    if (hour > 23 || minute > 59 || !s_mutex) {
        return -1;
    }

    int slot = -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < ALARM_MAX_COUNT; i++) {
        if (!s_alarms[i].used) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        alarm_entry_t *a = &s_alarms[slot];
        memset(a, 0, sizeof(*a));
        a->used = true;
        a->enabled = true;
        a->repeat_daily = repeat_daily;
        a->hour = hour;
        a->minute = minute;
        a->last_fired_mday = 0xFF;
        a->last_fired_min = 0xFF;
        if (label && label[0] != '\0') {
            strncpy(a->label, label, sizeof(a->label) - 1);
        } else {
            strncpy(a->label, "Alarm", sizeof(a->label) - 1);
        }
        save_to_nvs_locked();
        ESP_LOGI(TAG, "Added alarm #%d: %02u:%02u %s (%s)", slot, hour, minute, repeat_daily ? "Daily" : "Once", a->label);
    }
    xSemaphoreGive(s_mutex);
    return slot;
}

esp_err_t alarm_manager_toggle(int index)
{
    if (index < 0 || index >= ALARM_MAX_COUNT || !s_mutex) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_alarms[index].used) {
        s_alarms[index].enabled = !s_alarms[index].enabled;
        s_alarms[index].last_fired_mday = 0xFF;
        save_to_nvs_locked();
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t alarm_manager_delete(int index)
{
    if (index < 0 || index >= ALARM_MAX_COUNT || !s_mutex) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_alarms[index], 0, sizeof(s_alarms[index]));
    save_to_nvs_locked();
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void alarm_manager_clear_all(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_alarms, 0, sizeof(s_alarms));
    save_to_nvs_locked();
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "All alarms cleared");
}

int alarm_manager_get_all(alarm_entry_t *out_list, int max_count)
{
    if (!out_list || max_count <= 0 || !s_mutex) return 0;
    int n = max_count < ALARM_MAX_COUNT ? max_count : ALARM_MAX_COUNT;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out_list, s_alarms, (size_t)n * sizeof(alarm_entry_t));
    xSemaphoreGive(s_mutex);
    return n;
}

int alarm_manager_active_count(void)
{
    if (!s_mutex) return 0;
    int cnt = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < ALARM_MAX_COUNT; i++) {
        if (s_alarms[i].used && s_alarms[i].enabled) {
            cnt++;
        }
    }
    xSemaphoreGive(s_mutex);
    return cnt;
}

bool alarm_manager_is_ringing(alarm_entry_t *out_ringing_alarm)
{
    if (!s_ringing) return false;
    if (out_ringing_alarm) {
        *out_ringing_alarm = s_ringing_copy;
    }
    return true;
}

void alarm_manager_dismiss(void)
{
    s_ringing = false;
    audio_pa_enable(false);
}

void alarm_manager_snooze(uint8_t minutes)
{
    s_ringing = false;
    audio_pa_enable(false);
    struct tm now = {0};
    wifi_time_get_local(&now);
    int total_min = now.tm_hour * 60 + now.tm_min + minutes;
    uint8_t nh = (uint8_t)((total_min / 60) % 24);
    uint8_t nm = (uint8_t)(total_min % 60);
    alarm_manager_add(nh, nm, false, "Snooze");
}
