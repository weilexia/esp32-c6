#include "board_i2c.h"
#include "board_pins.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "board_i2c";
static i2c_master_bus_handle_t s_i2c_bus = NULL;

esp_err_t board_i2c_init(void)
{
    if (s_i2c_bus != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BOARD_I2C_PORT,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2C master bus: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Shared I2C bus initialized (SDA=%d, SCL=%d)", BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_get_bus(void)
{
    return s_i2c_bus;
}

esp_err_t board_i2c_add_device(uint8_t dev_addr, i2c_master_dev_handle_t *out_handle)
{
    if (!s_i2c_bus || !out_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev_addr,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, out_handle);
}

esp_err_t board_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t data[2] = {reg, val};
    return i2c_master_transmit(dev, data, sizeof(data), 100);
}

esp_err_t board_i2c_read_regs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, 100);
}

esp_err_t board_i2c_write_regs(i2c_master_dev_handle_t dev, uint8_t reg, const uint8_t *buf, size_t len)
{
    if (len > 32) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t tmp[33];
    tmp[0] = reg;
    if (len > 0 && buf != NULL) {
        memcpy(&tmp[1], buf, len);
    }
    return i2c_master_transmit(dev, tmp, len + 1, 100);
}
