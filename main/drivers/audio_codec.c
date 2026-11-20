#include "audio_codec.h"
#include "board_i2c.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "audio_codec";

static i2c_master_dev_handle_t s_es8311 = NULL;
static i2c_master_dev_handle_t s_es7210 = NULL;
static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static bool s_ready = false;

static void es8311_init_regs(void)
{
    if (!s_es8311) {
        return;
    }
    // Standard ES8311 slave mode init for 16kHz, 16-bit I2S, MCLK = 256 * Fs (4.096 MHz)
    static const uint8_t init_seq[][2] = {
        {0x00, 0x1F}, // Soft reset
        {0x00, 0x00}, // Release reset
        {0x00, 0x80}, // Power on CSM
        {0x01, 0x3F}, // Enable MCLK, BCLK, ADC/DAC clocks
        {0x02, 0x00}, // MCLK pre-divider = 1
        {0x03, 0x10}, // FS mode single speed
        {0x04, 0x10}, // DAC OSR
        {0x05, 0x00}, // Clock div = 1
        {0x06, 0x03}, // BCLK div
        {0x07, 0x00}, // LRCK div H
        {0x08, 0xFF}, // LRCK div L
        {0x09, 0x0C}, // SDP IN: I2S 16-bit
        {0x0A, 0x0C}, // SDP OUT: I2S 16-bit
        {0x0D, 0x01}, // Power up analog / VMID
        {0x0E, 0x02}, // Enable analog PGA/ADC
        {0x12, 0x00}, // Enable DAC
        {0x13, 0x10}, // Enable output drive
        {0x1B, 0x0A},
        {0x1C, 0x6A},
        {0x32, 0xBF}, // DAC volume (~0dB)
        {0x37, 0x08}, // DAC ramp / unmute
    };

    for (size_t i = 0; i < sizeof(init_seq) / sizeof(init_seq[0]); i++) {
        esp_err_t err = board_i2c_write_reg(s_es8311, init_seq[i][0], init_seq[i][1]);
        if (i == 0 && err != ESP_OK) {
            ESP_LOGW(TAG, "ES8311 not responding on I2C 0x%02X", BOARD_I2C_ADDR_ES8311);
            return;
        }
        if (i == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    ESP_LOGI(TAG, "ES8311 playback codec initialized");
}

static void es7210_init_regs(void)
{
    if (!s_es7210) {
        return;
    }
    // Standard ES7210 slave mode init for 16kHz 16-bit I2S dual mic (MIC1 + MIC2)
    static const uint8_t init_seq[][2] = {
        {0x00, 0xFF}, // Reset
        {0x00, 0x32}, // Normal + CSM
        {0x09, 0x30}, // Time control 0
        {0x0A, 0x30}, // Time control 1
        {0x23, 0x2A}, // ADC HPF
        {0x22, 0x0A},
        {0x21, 0x2A},
        {0x20, 0x0A},
        {0x08, 0x10}, // Slave mode
        {0x11, 0x60}, // I2S 16-bit standard
        {0x12, 0x00}, // Normal I2S (2ch)
        {0x40, 0x42}, // Analog power
        {0x41, 0x70}, // MIC1/2 bias 2.87V
        {0x42, 0x70}, // MIC3/4 bias
        {0x43, 0x1B}, // MIC1 gain +24dB
        {0x44, 0x1B}, // MIC2 gain +24dB
        {0x45, 0x00}, // MIC3 off
        {0x46, 0x00}, // MIC4 off
        {0x47, 0x08}, // MIC12 power on
        {0x48, 0x08},
        {0x4B, 0x0F}, // Enable ADC1/2
        {0x4C, 0x0F},
        {0x06, 0x00}, // Power up digital
        {0x01, 0x3F}, // Clocks on
    };

    for (size_t i = 0; i < sizeof(init_seq) / sizeof(init_seq[0]); i++) {
        esp_err_t err = board_i2c_write_reg(s_es7210, init_seq[i][0], init_seq[i][1]);
        if (i == 0 && err != ESP_OK) {
            ESP_LOGW(TAG, "ES7210 not responding on I2C 0x%02X", BOARD_I2C_ADDR_ES7210);
            return;
        }
        if (i == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    ESP_LOGI(TAG, "ES7210 microphone ADC initialized");
}

void audio_pa_enable(bool enable)
{
    gpio_set_level(BOARD_PA_CTRL_GPIO, enable ? 1 : 0);
}

esp_err_t audio_codec_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    // Configure NS4150B amplifier enable GPIO (default off to save power)
    gpio_config_t pa_cfg = {
        .pin_bit_mask = (1ULL << BOARD_PA_CTRL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_cfg);
    audio_pa_enable(false);

    // Initialize full-duplex I2S0 channel (shared MCLK/BCLK/WS)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK_GPIO,
            .bclk = BOARD_I2S_BCLK_GPIO,
            .ws   = BOARD_I2S_WS_GPIO,
            .dout = BOARD_I2S_DOUT_GPIO,
            .din  = BOARD_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));

    board_i2c_add_device(BOARD_I2C_ADDR_ES8311, &s_es8311);
    board_i2c_add_device(BOARD_I2C_ADDR_ES7210, &s_es7210);
    es8311_init_regs();
    es7210_init_regs();

    s_ready = true;
    ESP_LOGI(TAG, "Audio subsystem ready (%d Hz, 16-bit I2S)", AUDIO_SAMPLE_RATE_HZ);
    return ESP_OK;
}

esp_err_t audio_play_tone(uint32_t freq_hz, uint32_t duration_ms, uint8_t volume_pct)
{
    if (!s_ready || !s_tx_chan || duration_ms == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (volume_pct > 100) {
        volume_pct = 100;
    }

    int16_t chunk[256 * 2]; // 256 stereo frames = 16ms at 16kHz
    const uint32_t total_frames = (AUDIO_SAMPLE_RATE_HZ * duration_ms) / 1000;
    const float amp = (24000.0f * (float)volume_pct) / 100.0f;
    const float phase_step = (2.0f * 3.14159265f * (float)freq_hz) / (float)AUDIO_SAMPLE_RATE_HZ;
    float phase = 0.0f;

    audio_pa_enable(true);
    vTaskDelay(pdMS_TO_TICKS(5));

    uint32_t remaining = total_frames;
    while (remaining > 0) {
        uint32_t frames = remaining > 256 ? 256 : remaining;
        for (uint32_t i = 0; i < frames; i++) {
            int16_t sample = (freq_hz == 0) ? 0 : (int16_t)(sinf(phase) * amp);
            phase += phase_step;
            if (phase > 6.2831853f) {
                phase -= 6.2831853f;
            }
            chunk[i * 2] = sample;
            chunk[i * 2 + 1] = sample;
        }
        size_t written = 0;
        i2s_channel_write(s_tx_chan, chunk, frames * 2 * sizeof(int16_t), &written, pdMS_TO_TICKS(100));
        remaining -= frames;
    }

    audio_pa_enable(false);
    return ESP_OK;
}

void audio_play_beep_start(void)
{
    audio_play_tone(880, 70, 50);
    audio_play_tone(1175, 90, 55);
}

void audio_play_beep_ok(void)
{
    audio_play_tone(1047, 70, 55);
    audio_play_tone(1319, 70, 55);
    audio_play_tone(1568, 110, 60);
}

void audio_play_beep_err(void)
{
    audio_play_tone(440, 120, 55);
    audio_play_tone(330, 180, 55);
}

void audio_play_alarm_chime(bool (*should_stop_cb)(void))
{
    // Pleasant ascending C-major arpeggio chime
    static const uint32_t notes[] = {523, 659, 784, 1047, 784, 1047};
    static const uint32_t durs[]  = {140, 140, 140, 260,  140, 320};

    for (size_t i = 0; i < sizeof(notes) / sizeof(notes[0]); i++) {
        if (should_stop_cb && should_stop_cb()) {
            break;
        }
        audio_play_tone(notes[i], durs[i], 75);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

static void write_wav_header(uint8_t *hdr, uint32_t pcm_bytes)
{
    uint32_t riff_size = pcm_bytes + 36;
    uint32_t sample_rate = AUDIO_SAMPLE_RATE_HZ;
    uint16_t channels = 1;
    uint16_t bits_per_sample = 16;
    uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    uint16_t block_align = channels * (bits_per_sample / 8);

    memcpy(hdr + 0, "RIFF", 4);
    memcpy(hdr + 4, &riff_size, 4);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    uint16_t audio_fmt = 1; // PCM
    memcpy(hdr + 16, &fmt_size, 4);
    memcpy(hdr + 20, &audio_fmt, 2);
    memcpy(hdr + 22, &channels, 2);
    memcpy(hdr + 24, &sample_rate, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    memcpy(hdr + 32, &block_align, 2);
    memcpy(hdr + 34, &bits_per_sample, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &pcm_bytes, 4);
}

size_t audio_record_wav(uint8_t *wav_buf, size_t wav_buf_size, uint32_t max_ms, bool (*stop_cb)(void))
{
    if (!s_ready || !s_rx_chan || !wav_buf || wav_buf_size <= 44) {
        return 0;
    }

    const size_t max_pcm_samples = (wav_buf_size - 44) / sizeof(int16_t);
    const size_t target_samples = ((size_t)AUDIO_SAMPLE_RATE_HZ * max_ms) / 1000;
    const size_t limit_samples = target_samples < max_pcm_samples ? target_samples : max_pcm_samples;

    int16_t *pcm_out = (int16_t *)(wav_buf + 44);
    int16_t stereo_chunk[256 * 2]; // 16ms stereo frames
    size_t captured = 0;

    bool voice_detected = false;
    uint32_t silence_chunks = 0;

    while (captured < limit_samples) {
        if (stop_cb && stop_cb()) {
            break;
        }
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, stereo_chunk, sizeof(stereo_chunk), &bytes_read, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytes_read == 0) {
            continue;
        }
        size_t frames = bytes_read / (2 * sizeof(int16_t));
        uint64_t energy = 0;
        for (size_t i = 0; i < frames && captured < limit_samples; i++) {
            // Take left mic channel (MIC1)
            int16_t s = stereo_chunk[i * 2];
            pcm_out[captured++] = s;
            energy += (uint64_t)abs((int)s);
        }

        // Simple energy VAD: once speech starts, auto-stop after ~900ms of trailing silence
        uint32_t avg_abs = frames > 0 ? (uint32_t)(energy / frames) : 0;
        if (avg_abs > 500) {
            voice_detected = true;
            silence_chunks = 0;
        } else if (voice_detected && captured > (AUDIO_SAMPLE_RATE_HZ / 2)) {
            silence_chunks++;
            if (silence_chunks >= 55) { // ~880ms trailing silence
                break;
            }
        }
    }

    uint32_t pcm_bytes = (uint32_t)(captured * sizeof(int16_t));
    write_wav_header(wav_buf, pcm_bytes);
    ESP_LOGI(TAG, "Recorded %u ms (%u bytes WAV)", (unsigned)((captured * 1000) / AUDIO_SAMPLE_RATE_HZ), (unsigned)(pcm_bytes + 44));
    return (size_t)pcm_bytes + 44;
}
