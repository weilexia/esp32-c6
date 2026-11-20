#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t board_i2c_init(void);
i2c_master_bus_handle_t board_i2c_get_bus(void);
esp_err_t board_i2c_add_device(uint8_t dev_addr, i2c_master_dev_handle_t *out_handle);
esp_err_t board_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val);
esp_err_t board_i2c_read_regs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len);
esp_err_t board_i2c_write_regs(i2c_master_dev_handle_t dev, uint8_t reg, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
