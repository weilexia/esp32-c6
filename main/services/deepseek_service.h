#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool balance_valid;
    bool is_available;
    char currency[8];         // "CNY" or "USD"
    float total_balance;
    float granted_balance;
    float topped_up_balance;
    float day_start_balance;
    float today_cost;
    float daily_budget;

    uint64_t prompt_tokens;
    uint64_t completion_tokens;
    uint64_t total_tokens;
    bool tokens_estimated;    // true when estimated from balance delta, false when pushed via webhook

    uint32_t calls;
    uint32_t errors;

    char last_sync[16];       // "HH:MM"
    char status_text[24];     // "ONLINE", "NO KEY", "SYNCING", "HTTP 401", etc.
} deepseek_usage_t;

esp_err_t deepseek_service_start(void);
void deepseek_service_get_usage(deepseek_usage_t *out);
void deepseek_service_request_refresh(void);

// Push full usage snapshot (from local proxy / CC Switch / webhook)
void deepseek_service_set_usage(uint32_t calls, uint32_t errors,
                                uint64_t prompt_tokens, uint64_t completion_tokens,
                                uint64_t total_tokens, float cost_rmb, bool has_cost);

// Incrementally add a single API call's token & cost consumption
void deepseek_service_add_call(uint64_t prompt_tokens, uint64_t completion_tokens,
                               float cost_rmb, bool is_error);

// Reset today's accumulated counters
void deepseek_service_reset_today(void);

// Format token count cleanly: e.g., "842", "12.4K", "1.28M"
void deepseek_format_tokens(uint64_t tokens, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
