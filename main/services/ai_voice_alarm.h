#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AI_VOICE_IDLE = 0,
    AI_VOICE_LISTENING,
    AI_VOICE_THINKING,
    AI_VOICE_DONE,
    AI_VOICE_ERROR,
} ai_voice_state_t;

typedef struct {
    ai_voice_state_t state;
    char heard_text[96];
    char reply_text[64];
} ai_voice_status_t;

esp_err_t ai_voice_alarm_init(void);

// Start recording from ES7210 mic -> Cloud STT -> Cloud LLM / Local NLP -> AlarmManager
esp_err_t ai_voice_alarm_start_listening(void);

// Directly parse natural-language Chinese/English alarm command (e.g. from USB console or STT)
bool ai_voice_alarm_process_text(const char *utterance);

void ai_voice_alarm_get_status(ai_voice_status_t *out);

#ifdef __cplusplus
}
#endif
