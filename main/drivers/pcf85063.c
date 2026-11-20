#include "pcf85063.h"
#include "board_i2c.h"
#include "board_pins.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "pcf85063";
static i2c_master_dev_handle_t s_dev = NULL;

#define PCF85063_REG_CTRL1   0x00
#define PCF85063_REG_SECONDS 0x04

static uint8_t bcd_to_dec(uint8_t bcd)
{
    return (uint8_t)((bcd >> 4) * 10 + (bcd & 0x0F));
}

static uint8_t dec_to_bcd(uint8_t dec)
{
    return (uint8_t)(((dec / 10) << 4) | (dec % 10));
}

esp_err_t pcf85063_init(void)
{
    esp_err_t err = board_i2c_add_device(BOARD_I2C_ADDR_PCF85063, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t ctrl1 = 0;
    err = board_i2c_read_regs(s_dev, PCF85063_REG_CTRL1, &ctrl1, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 not responding on I2C 0x%02X (%s)", BOARD_I2C_ADDR_PCF85063, esp_err_to_name(err));
        return err;
    }

    // Ensure RTC oscillator is running (clear STOP bit 5) and 24-hour mode (clear 12_24 bit 1)
    ctrl1 &= (uint8_t)~((1 << 5) | (1 << 1));
    board_i2c_write_reg(s_dev, PCF85063_REG_CTRL1, ctrl1);

    ESP_LOGI(TAG, "PCF85063 RTC initialized");
    return ESP_OK;
}

esp_err_t pcf85063_get_time(struct tm *out_tm)
{
    if (!s_dev || !out_tm) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[7] = {0};
    esp_err_t err = board_i2c_read_regs(s_dev, PCF85063_REG_SECONDS, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    // Check OS (oscillator stop / clock integrity) bit 7 of seconds register
    if (raw[0] & 0x80) {
        ESP_LOGW(TAG, "PCF85063 oscillator stop flag set (time uninitialized)");
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(out_tm, 0, sizeof(*out_tm));
    out_tm->tm_sec  = bcd_to_dec(raw[0] & 0x7F);
    out_tm->tm_min  = bcd_to_dec(raw[1] & 0x7F);
    out_tm->tm_hour = bcd_to_dec(raw[2] & 0x3F);
    out_tm->tm_mday = bcd_to_dec(raw[3] & 0x3F);
    out_tm->tm_wday = raw[4] & 0x07;
    out_tm->tm_mon  = bcd_to_dec(raw[5] & 0x1F) - 1;
    out_tm->tm_year = bcd_to_dec(raw[6]) + 100; // Years since 1900 (2000..2099)

    if (out_tm->tm_year < 124 || out_tm->tm_mon < 0 || out_tm->tm_mon > 11 || out_tm->tm_mday < 1 || out_tm->tm_mday > 31) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t pcf85063_set_time(const struct tm *in_tm)
{
    if (!s_dev || !in_tm) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[7];
    raw[0] = dec_to_bcd((uint8_t)in_tm->tm_sec) & 0x7F; // Clear OS flag
    raw[1] = dec_to_bcd((uint8_t)in_tm->tm_min);
    raw[2] = dec_to_bcd((uint8_t)in_tm->tm_hour);
    raw[3] = dec_to_bcd((uint8_t)in_tm->tm_mday);
    raw[4] = (uint8_t)(in_tm->tm_wday & 0x07);
    raw[5] = dec_to_bcd((uint8_t)(in_tm->tm_mon + 1));
    int yr = in_tm->tm_year >= 100 ? (in_tm->tm_year - 100) : 0;
    raw[6] = dec_to_bcd((uint8_t)(yr % 100));

    return board_i2c_write_regs(s_dev, PCF85063_REG_SECONDS, raw, sizeof(raw));
}
