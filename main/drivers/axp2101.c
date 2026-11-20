#include "axp2101.h"
#include "board_i2c.h"
#include "board_pins.h"
#include "esp_log.h"

static const char *TAG = "axp2101";
static i2c_master_dev_handle_t s_dev = NULL;

// AXP2101 register addresses
#define AXP2101_REG_STATUS1      0x00
#define AXP2101_REG_STATUS2      0x01
#define AXP2101_REG_CHIP_ID      0x03
#define AXP2101_REG_ADC_CTRL     0x30
#define AXP2101_REG_LDO_ONOFF0   0x90
#define AXP2101_REG_ALDO1_VOLT   0x92
#define AXP2101_REG_ALDO2_VOLT   0x93
#define AXP2101_REG_ALDO3_VOLT   0x94
#define AXP2101_REG_ALDO4_VOLT   0x95
#define AXP2101_REG_BLDO2_VOLT   0x97
#define AXP2101_REG_BAT_PERCENT  0xA4

esp_err_t axp2101_init(void)
{
    esp_err_t err = board_i2c_add_device(BOARD_I2C_ADDR_AXP2101, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t chip_id = 0;
    err = board_i2c_read_regs(s_dev, AXP2101_REG_CHIP_ID, &chip_id, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not responding on I2C 0x%02X (%s)", BOARD_I2C_ADDR_AXP2101, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "AXP2101 detected, chip ID=0x%02X", chip_id);

    // Configure peripheral rails per Waveshare ESP32-C6-Touch-LCD-1.83 schematic:
    // ALDO1/2 = 3.3V (0x1C), ALDO3 = 3.0V (0x19), ALDO4 = 1.8V (0x0D), BLDO2 = 2.8V (0x17)
    // Formula: V = 0.5V + reg * 0.1V
    board_i2c_write_reg(s_dev, AXP2101_REG_ALDO1_VOLT, 28); // 3.3V
    board_i2c_write_reg(s_dev, AXP2101_REG_ALDO2_VOLT, 28); // 3.3V
    board_i2c_write_reg(s_dev, AXP2101_REG_ALDO3_VOLT, 25); // 3.0V
    board_i2c_write_reg(s_dev, AXP2101_REG_ALDO4_VOLT, 13); // 1.8V
    board_i2c_write_reg(s_dev, AXP2101_REG_BLDO2_VOLT, 23); // 2.8V

    // Enable ALDO1..4 + BLDO2 (bits 0,1,2,3,5)
    uint8_t ldo_en = 0;
    if (board_i2c_read_regs(s_dev, AXP2101_REG_LDO_ONOFF0, &ldo_en, 1) == ESP_OK) {
        ldo_en |= 0x2F;
        board_i2c_write_reg(s_dev, AXP2101_REG_LDO_ONOFF0, ldo_en);
    }

    // Enable battery voltage & gauge ADC
    uint8_t adc = 0;
    if (board_i2c_read_regs(s_dev, AXP2101_REG_ADC_CTRL, &adc, 1) == ESP_OK) {
        adc |= 0x01; // Battery voltage ADC enable
        board_i2c_write_reg(s_dev, AXP2101_REG_ADC_CTRL, adc);
    }

    return ESP_OK;
}

int axp2101_get_battery_percent(void)
{
    if (!s_dev) {
        return -1;
    }
    uint8_t pct = 0;
    if (board_i2c_read_regs(s_dev, AXP2101_REG_BAT_PERCENT, &pct, 1) != ESP_OK) {
        return -1;
    }
    if (pct > 100) {
        pct = 100;
    }
    return (int)pct;
}

bool axp2101_is_charging(void)
{
    if (!s_dev) {
        return false;
    }
    uint8_t st2 = 0;
    if (board_i2c_read_regs(s_dev, AXP2101_REG_STATUS2, &st2, 1) != ESP_OK) {
        return false;
    }
    // Bits [6:5] == 0b01 indicates charging
    return ((st2 >> 5) & 0x03) == 0x01;
}

bool axp2101_is_vbus_present(void)
{
    if (!s_dev) {
        return false;
    }
    uint8_t st1 = 0;
    if (board_i2c_read_regs(s_dev, AXP2101_REG_STATUS1, &st1, 1) != ESP_OK) {
        return false;
    }
    return (st1 & (1 << 5)) != 0;
}
