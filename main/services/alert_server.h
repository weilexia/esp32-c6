#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALERT_HISTORY_MAX 16

typedef enum {
    ALERT_LEVEL_INFO = 0,
    ALERT_LEVEL_WARN,
    ALERT_LEVEL_ERROR,
} alert_level_t;

typedef struct {
    uint32_t id;
    alert_level_t level;
    char time_str[16];   // "HH:MM:SS"
    char title[64];
    char message[192];
    bool acked;
    bool has_voice;
} alert_record_t;

typedef struct {
    bool active;         // True while voice alert is ringing & awaiting user button confirmation
    uint32_t seq;
    alert_record_t current;
} alert_state_t;

typedef struct {
    uint32_t total;
    uint32_t error_cnt;
    uint32_t warn_cnt;
    uint32_t unacked_cnt;
} alert_stats_t;

esp_err_t alert_server_init(void);
esp_err_t alert_server_start(void);
void alert_server_stop_httpd(void);
bool alert_server_get_state(alert_state_t *out);
void alert_server_stop(void);

// Dashboard history & statistics APIs
void alert_server_get_stats(alert_stats_t *out);
int alert_server_get_history(alert_record_t *out_list, int max_count);
void alert_server_clear_history(void);

#ifdef __cplusplus
}
#endif
