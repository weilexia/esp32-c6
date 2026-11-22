#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t seq;
    char app_id[48];
    char title[64];
    char message[192];
} ble_ancs_msg_t;

esp_err_t ble_ancs_init(void);
bool ble_ancs_is_connected(void);
bool ble_ancs_get_latest_msg(ble_ancs_msg_t *out);

// Helper for testing Chinese WeChat notification popup from serial console
void ble_ancs_inject_test_msg(const char *title, const char *message);

#ifdef __cplusplus
}
#endif
