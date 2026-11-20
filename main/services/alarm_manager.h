#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_MAX_COUNT 8

typedef struct {
    bool used;
    bool enabled;
    bool repeat_daily;
    uint8_t hour;
    uint8_t minute;
    uint8_t last_fired_mday;
    uint8_t last_fired_min;
    char label[28];
} alarm_entry_t;

esp_err_t alarm_manager_init(void);

// Returns slot index [0..ALARM_MAX_COUNT-1] or -1 if full
int alarm_manager_add(uint8_t hour, uint8_t minute, bool repeat_daily, const char *label);
esp_err_t alarm_manager_toggle(int index);
esp_err_t alarm_manager_delete(int index);
void alarm_manager_clear_all(void);

int alarm_manager_get_all(alarm_entry_t *out_list, int max_count);
int alarm_manager_active_count(void);

// Ringing state controls
bool alarm_manager_is_ringing(alarm_entry_t *out_ringing_alarm);
void alarm_manager_dismiss(void);
void alarm_manager_snooze(uint8_t minutes);

#ifdef __cplusplus
}
#endif
