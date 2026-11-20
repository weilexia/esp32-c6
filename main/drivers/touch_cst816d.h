#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOUCH_GESTURE_NONE = 0x00,
    TOUCH_GESTURE_SLIDE_DOWN = 0x01,
    TOUCH_GESTURE_SLIDE_UP = 0x02,
    TOUCH_GESTURE_SLIDE_LEFT = 0x03,
    TOUCH_GESTURE_SLIDE_RIGHT = 0x04,
    TOUCH_GESTURE_SINGLE_CLICK = 0x05,
    TOUCH_GESTURE_DOUBLE_CLICK = 0x0B,
    TOUCH_GESTURE_LONG_PRESS = 0x0C,
} touch_gesture_t;

typedef struct {
    bool pressed;
    uint16_t x;
    uint16_t y;
    touch_gesture_t gesture;
} touch_point_t;

esp_err_t touch_cst816d_init(void);
esp_err_t touch_cst816d_read(touch_point_t *out_pt);

#ifdef __cplusplus
}
#endif
