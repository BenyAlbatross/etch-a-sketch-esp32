#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_API_PROMPT_WORD_BUFFER_SIZE 64
#define APP_API_GUESS_BUFFER_SIZE 64

typedef struct {
    char guess[APP_API_GUESS_BUFFER_SIZE];
    int confidence;
    bool correct;
} app_ai_submit_result_t;

typedef esp_err_t (*app_api_send_frame_fn)(const char *payload, size_t payload_len);

esp_err_t app_api_submit_drawing(const char *payload,
                                 size_t payload_len,
                                 app_ai_submit_result_t *out_result,
                                 bool *out_submit_success,
                                 bool submit_stub_success_flag);

esp_err_t app_api_fetch_and_publish_prompt(app_api_send_frame_fn send_frame,
                                           char *io_active_prompt_word,
                                           size_t io_active_prompt_word_len);

#ifdef __cplusplus
}
#endif
