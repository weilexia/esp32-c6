#pragma once

// Hardware pin map for Waveshare ESP32-C6-Touch-LCD-1.83 (Rev1.2)
// Reference: reference/ESP32-C6-Touch-LCD-1.83.md

// === Shared SPI Bus (LCD + Micro SD) ===
#define BOARD_SPI_HOST          SPI2_HOST
#define BOARD_SPI_SCK_GPIO      1
#define BOARD_SPI_MOSI_GPIO     2
#define BOARD_SPI_MISO_GPIO     16

// === LCD (ST7789P, 240x284, RAM 240x320) ===
#define BOARD_LCD_WIDTH         240
#define BOARD_LCD_HEIGHT        284
#define BOARD_LCD_Y_OFFSET      20
#define BOARD_LCD_DC_GPIO       3
#define BOARD_LCD_RST_GPIO      4   // Shared with Touch RST
#define BOARD_LCD_CS_GPIO       5
#define BOARD_LCD_BL_GPIO       6

// === Micro SD Card (SPI) ===
#define BOARD_SD_CS_GPIO        17

// === Shared I2C Bus (Touch, PMIC, IMU, RTC, ES8311, ES7210) ===
#define BOARD_I2C_PORT          I2C_NUM_0
#define BOARD_I2C_SDA_GPIO      7
#define BOARD_I2C_SCL_GPIO      8
#define BOARD_I2C_FREQ_HZ       400000

#define BOARD_I2C_ADDR_CST816D  0x15
#define BOARD_I2C_ADDR_ES8311   0x18
#define BOARD_I2C_ADDR_AXP2101  0x34
#define BOARD_I2C_ADDR_ES7210   0x40
#define BOARD_I2C_ADDR_PCF85063 0x51
#define BOARD_I2C_ADDR_QMI8658  0x6B

// === Touch (CST816D) ===
#define BOARD_TOUCH_RST_GPIO    4   // Shared with LCD RST (managed by LCD init)
#define BOARD_TOUCH_INT_GPIO    11

// === Audio I2S (ES8311 + ES7210) & PA (NS4150B) ===
#define BOARD_I2S_MCLK_GPIO     19
#define BOARD_I2S_BCLK_GPIO     20
#define BOARD_I2S_DIN_GPIO      21  // I2S_ASDOUT (ESP32 RX from ES7210)
#define BOARD_I2S_WS_GPIO       22  // I2S_LRCK
#define BOARD_I2S_DOUT_GPIO     23  // I2S_DSDIN  (ESP32 TX to ES8311)
#define BOARD_PA_CTRL_GPIO      0

// === Sensors & RTC Interrupts ===
#define BOARD_IMU_INT1_GPIO     10
#define BOARD_RTC_INT_GPIO      15

// === Buttons ===
#define BOARD_BTN_BOOT_GPIO     9
#define BOARD_BTN_PWR_GPIO      18
