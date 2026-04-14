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
typedef esp_err_t (*app_api_http_post_fn)(const char *url,
                                          const char *body,
                                          char *out_response,
                                          size_t out_response_len);

void app_api_set_openai_api_key(const char *api_key);
void app_api_set_http_post_for_test(app_api_http_post_fn http_post);
void app_api_set_local_debug_mode_for_test(bool enabled);

esp_err_t app_api_submit_drawing(const char *payload,
                                 size_t payload_len,
                                 const char *active_prompt_word,
                                 app_ai_submit_result_t *out_result,
                                 bool *out_submit_success,
                                 bool local_debug_submit_correct);

esp_err_t app_api_fetch_and_publish_prompt(app_api_send_frame_fn send_frame,
                                           char *io_active_prompt_word,
                                           size_t io_active_prompt_word_len);

esp_err_t app_api_build_frame_png_data_url(const char *payload,
                                           size_t payload_len,
                                           char **out_data_url,
                                           size_t *out_data_url_len);

bool app_api_parse_submit_response_text(const char *response_text,
                                        app_ai_submit_result_t *out_result);

#ifdef __cplusplus
}
#endif
