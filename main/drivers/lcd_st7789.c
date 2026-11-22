#include "lcd_st7789.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "lcd_st7789";
static esp_lcd_panel_handle_t s_panel = NULL;
static SemaphoreHandle_t s_trans_done_sem = NULL;
static uint8_t s_backlight_pct = 85;

static bool IRAM_ATTR on_lcd_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                              esp_lcd_panel_io_event_data_t *edata,
                                              void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    BaseType_t hp_woken = pdFALSE;
    if (s_trans_done_sem) {
        xSemaphoreGiveFromISR(s_trans_done_sem, &hp_woken);
    }
    return hp_woken == pdTRUE;
}

#define LCD_SPI_CLK_HZ        (40 * 1000 * 1000)
#define LCD_BL_LEDC_TIMER     LEDC_TIMER_0
#define LCD_BL_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define LCD_BL_LEDC_CHANNEL   LEDC_CHANNEL_0

static void board_shared_lcd_touch_reset(void)
{
    // GPIO4 is physically shared by ST7789P RST and CST816D RST.
    // Perform one unified hardware reset here; both drivers use rst_gpio_num = -1.
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << BOARD_LCD_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(BOARD_LCD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_LCD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static esp_err_t lcd_backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LCD_BL_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LCD_BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        return err;
    }

    ledc_channel_config_t ch_cfg = {
        .gpio_num = BOARD_LCD_BL_GPIO,
        .speed_mode = LCD_BL_LEDC_MODE,
        .channel = LCD_BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&ch_cfg);
}

esp_err_t lcd_st7789_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_backlight_pct = percent;
    uint32_t duty = (1023 * percent) / 100;
    ledc_set_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL, duty);
    return ledc_update_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL);
}

uint8_t lcd_st7789_get_backlight(void)
{
    return s_backlight_pct;
}

esp_err_t lcd_st7789_init(void)
{
    if (s_panel != NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing shared SPI2 bus & ST7789P 240x284 LCD");

    // Configure shared SPI2 bus (also ready for Micro SD MISO on GPIO16)
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = BOARD_SPI_SCK_GPIO,
        .mosi_io_num = BOARD_SPI_MOSI_GPIO,
        .miso_io_num = BOARD_SPI_MISO_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_WIDTH * 40 * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(BOARD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    board_shared_lcd_touch_reset();

    if (!s_trans_done_sem) {
        s_trans_done_sem = xSemaphoreCreateBinary();
    }

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC_GPIO,
        .cs_gpio_num = BOARD_LCD_CS_GPIO,
        .pclk_hz = LCD_SPI_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_lcd_color_trans_done,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_SPI_HOST, &io_cfg, &io_handle);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1, // Already reset via board_shared_lcd_touch_reset()
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, BOARD_LCD_Y_OFFSET));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    lcd_backlight_init();
    lcd_st7789_set_backlight(85);

    ESP_LOGI(TAG, "ST7789P LCD ready");
    return ESP_OK;
}

esp_lcd_panel_handle_t lcd_st7789_get_panel(void)
{
    return s_panel;
}

esp_err_t lcd_st7789_draw_bitmap(int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    if (!s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    // Clear any stale semaphore token before queuing DMA transfer
    if (s_trans_done_sem) {
        xSemaphoreTake(s_trans_done_sem, 0);
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, color_data);
    // Wait until SPI DMA finishes reading color_data so caller's shared s_tile_buf is never overwritten mid-transfer
    if (err == ESP_OK && s_trans_done_sem) {
        xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(100));
    }
    return err;
}
