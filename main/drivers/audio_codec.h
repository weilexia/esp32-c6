#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_SAMPLE_RATE_HZ 16000

esp_err_t audio_codec_init(void);
void audio_pa_enable(bool enable);
void audio_set_mute(bool muted);
bool audio_is_muted(void);

// Playback helpers (ES8311 + NS4150B)
esp_err_t audio_play_tone(uint32_t freq_hz, uint32_t duration_ms, uint8_t volume_pct);
void audio_play_beep_start(void);
void audio_play_beep_ok(void);
void audio_play_beep_err(void);
void audio_play_alarm_chime(bool (*should_stop_cb)(void));

// Play 16-bit mono WAV (supports 16kHz native and 8kHz 2x upsampling), interruptible every 16ms
esp_err_t audio_play_wav(const uint8_t *wav_data, size_t wav_len, bool (*should_stop_cb)(void));

// Zero-RAM-allocation streaming PCM playback (for large WAV alerts when heap is tight)
void audio_stream_begin(void);
void audio_stream_write_mono16(const int16_t *samples, size_t sample_count, uint32_t sample_rate);
void audio_stream_end(void);

// Voice recording helper (ES7210 dual mic -> 16kHz 16-bit mono WAV in caller buffer)
// Returns total WAV bytes written into wav_buf (including 44-byte RIFF header), or 0 on error.
size_t audio_record_wav(uint8_t *wav_buf, size_t wav_buf_size, uint32_t max_ms, bool (*stop_cb)(void));

#ifdef __cplusplus
}
#endif
