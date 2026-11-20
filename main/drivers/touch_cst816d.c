#include "touch_cst816d.h"
#include "board_i2c.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "touch_cst816d";
static i2c_master_dev_handle_t s_dev = NULL;

#define CST816D_REG_GESTURE     0x01
#define CST816D_REG_CHIP_ID     0xA7
#define CST816D_REG_DIS_SLEEP   0xFE

esp_err_t touch_cst816d_init(void)
{
    // Note: Do NOT reset GPIO4 here — it is shared with ST7789P and already reset in lcd_st7789_init().
    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << BOARD_TOUCH_INT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_cfg);

    esp_err_t err = board_i2c_add_device(BOARD_I2C_ADDR_CST816D, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t chip_id = 0;
    if (board_i2c_read_regs(s_dev, CST816D_REG_CHIP_ID, &chip_id, 1) == ESP_OK) {
        ESP_LOGI(TAG, "CST816D detected, chip ID=0x%02X", chip_id);
        // Disable auto-sleep so I2C polling stays responsive
        board_i2c_write_reg(s_dev, CST816D_REG_DIS_SLEEP, 0xFF);
    } else {
        ESP_LOGW(TAG, "CST816D not yet awake (will wake on first touch)");
    }

    return ESP_OK;
}

esp_err_t touch_cst816d_read(touch_point_t *out_pt)
{
    if (!out_pt) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_pt, 0, sizeof(*out_pt));
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    // CST816D may sleep until touched; if INT is high and I2C NACKs, treat as not pressed.
    uint8_t raw[6] = {0};
    esp_err_t err = board_i2c_read_regs(s_dev, CST816D_REG_GESTURE, raw, sizeof(raw));
    if (err != ESP_OK) {
        return ESP_OK;
    }

    uint8_t gesture = raw[0];
    uint8_t fingers = raw[1];
    uint16_t x = ((uint16_t)(raw[2] & 0x0F) << 8) | raw[3];
    uint16_t y = ((uint16_t)(raw[4] & 0x0F) << 8) | raw[5];

    if (x >= BOARD_LCD_WIDTH) {
        x = BOARD_LCD_WIDTH - 1;
    }
    if (y >= BOARD_LCD_HEIGHT) {
        y = BOARD_LCD_HEIGHT - 1;
    }

    out_pt->gesture = (touch_gesture_t)gesture;
    out_pt->pressed = (fingers > 0);
    out_pt->x = x;
    out_pt->y = y;
    return ESP_OK;
}
