#include "app_api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "secrets.h"

#define APP_HTTP_RESPONSE_BUFFER_SIZE 8192
#define APP_AI_CONTENT_BUFFER_SIZE 768
#define APP_SUBMIT_STUB_GUESS "stub-guess"
#define APP_SUBMIT_STUB_CONFIDENCE 7
#define APP_SUBMIT_STUB_DELAY_MS 2000

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} app_http_response_buffer_t;

static const char *TAG = "app_api";

static bool app_is_api_key_configured(void)
{
    return (OPENAI_API_KEY[0] != '\0') && (strcmp(OPENAI_API_KEY, "YOUR_OPENAI_API_KEY") != 0);
}

static esp_err_t app_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_http_response_buffer_t *response = (app_http_response_buffer_t *)evt->user_data;
    if (response == NULL) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data != NULL && evt->data_len > 0) {
        if (response->length >= response->capacity) {
            return ESP_OK;
        }

        const size_t available = response->capacity - response->length - 1U;
        if (available == 0U) {
            return ESP_OK;
        }

        const size_t copy_len = (evt->data_len < (int)available) ? (size_t)evt->data_len : available;
        memcpy(response->buffer + response->length, evt->data, copy_len);
        response->length += copy_len;
        response->buffer[response->length] = '\0';
    }

    return ESP_OK;
}

static esp_err_t app_http_post_json(const char *url,
                                    const char *body,
                                    char *out_response,
                                    size_t out_response_len)
{
    if (url == NULL || body == NULL || out_response == NULL || out_response_len < 2U) {
        return ESP_ERR_INVALID_ARG;
    }

    out_response[0] = '\0';
    app_http_response_buffer_t response = {
        .buffer = out_response,
        .capacity = out_response_len,
        .length = 0U,
    };

    char auth_header[160];
    const int auth_written = snprintf(auth_header, sizeof(auth_header), "Bearer %s", OPENAI_API_KEY);
    if (auth_written <= 0 || (size_t)auth_written >= sizeof(auth_header)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = app_http_event_handler,
        .user_data = &response,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_POST,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, body, strlen(body));

    const esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

static bool app_extract_chat_content(const char *response_json,
                                     char *out_content,
                                     size_t out_content_len)
{
    if (response_json == NULL || out_content == NULL || out_content_len == 0U) {
        return false;
    }

    cJSON *root = cJSON_Parse(response_json);
    if (root == NULL) {
        return false;
    }

    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    const cJSON *choice0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    const cJSON *message = cJSON_IsObject(choice0) ? cJSON_GetObjectItemCaseSensitive(choice0, "message") : NULL;
    const cJSON *content = cJSON_IsObject(message) ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;

    bool ok = false;
    if (cJSON_IsString(content) && content->valuestring != NULL) {
        const size_t content_len = strnlen(content->valuestring, out_content_len - 1U);
        memcpy(out_content, content->valuestring, content_len);
        out_content[content_len] = '\0';
        ok = true;
    }

    cJSON_Delete(root);
    return ok;
}

static void app_sanitize_token(const char *input, char *output, size_t output_len)
{
    if (output == NULL || output_len == 0U) {
        return;
    }

    output[0] = '\0';
    if (input == NULL) {
        return;
    }

    size_t in_idx = 0U;
    while (input[in_idx] != '\0' && !isalnum((unsigned char)input[in_idx])) {
        in_idx++;
    }

    size_t out_idx = 0U;
    while (input[in_idx] != '\0' && out_idx + 1U < output_len) {
        const char ch = input[in_idx];
        if (!(isalnum((unsigned char)ch) || ch == '-' || ch == '_' || ch == ' ')) {
            break;
        }
        output[out_idx++] = ch;
        in_idx++;
    }
    output[out_idx] = '\0';
}

static bool app_try_extract_prompt_word(const char *content,
                                        char *out_word,
                                        size_t out_word_len)
{
    if (content == NULL || out_word == NULL || out_word_len < 2U) {
        return false;
    }

    cJSON *content_json = cJSON_Parse(content);
    if (content_json != NULL) {
        const cJSON *word = cJSON_GetObjectItemCaseSensitive(content_json, "word");
        if (cJSON_IsString(word) && word->valuestring != NULL) {
            app_sanitize_token(word->valuestring, out_word, out_word_len);
            cJSON_Delete(content_json);
            return out_word[0] != '\0';
        }
        cJSON_Delete(content_json);
    }

    app_sanitize_token(content, out_word, out_word_len);
    return out_word[0] != '\0';
}

esp_err_t app_api_submit_drawing(const char *payload,
                                 size_t payload_len,
                                 app_ai_submit_result_t *out_result,
                                 bool *out_submit_success,
                                 bool submit_stub_success_flag)
{
    if (payload == NULL || payload_len == 0U || out_result == NULL || out_submit_success == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Stub latency to simulate network/API turnaround while integration is pending.
    vTaskDelay(pdMS_TO_TICKS(APP_SUBMIT_STUB_DELAY_MS));

    (void)payload;
    (void)payload_len;

    strncpy(out_result->guess, APP_SUBMIT_STUB_GUESS, sizeof(out_result->guess) - 1U);
    out_result->guess[sizeof(out_result->guess) - 1U] = '\0';
    out_result->confidence = APP_SUBMIT_STUB_CONFIDENCE;
    out_result->correct = submit_stub_success_flag;
    *out_submit_success = submit_stub_success_flag;

    return ESP_OK;
}

esp_err_t app_api_fetch_and_publish_prompt(app_api_send_frame_fn send_frame,
                                           char *io_active_prompt_word,
                                           size_t io_active_prompt_word_len)
{
    if (send_frame == NULL || io_active_prompt_word == NULL || io_active_prompt_word_len < 2U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!app_is_api_key_configured()) {
        ESP_LOGW(TAG, "OPENAI_API_KEY is not configured; using default prompt '%s'", io_active_prompt_word);
    } else {
        cJSON *root = cJSON_CreateObject();
        cJSON *messages = cJSON_CreateArray();
        cJSON *message = cJSON_CreateObject();
        if (root == NULL || messages == NULL || message == NULL) {
            cJSON_Delete(root);
            cJSON_Delete(messages);
            cJSON_Delete(message);
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(root, "model", "gpt-4o-mini");
        cJSON_AddStringToObject(message, "role", "user");
        cJSON_AddStringToObject(message,
                                "content",
                                "Give me one simple Pictionary noun. Respond ONLY with JSON: {\"word\":\"<noun>\"}");
        cJSON_AddItemToArray(messages, message);
        cJSON_AddItemToObject(root, "messages", messages);

        char *request_body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (request_body == NULL) {
            return ESP_ERR_NO_MEM;
        }

        char http_response[APP_HTTP_RESPONSE_BUFFER_SIZE];
        const esp_err_t prompt_err = app_http_post_json("https://api.openai.com/v1/chat/completions",
                                                        request_body,
                                                        http_response,
                                                        sizeof(http_response));
        free(request_body);
        if (prompt_err == ESP_OK) {
            char content[APP_AI_CONTENT_BUFFER_SIZE];
            if (app_extract_chat_content(http_response, content, sizeof(content))) {
                char prompt_word[APP_API_PROMPT_WORD_BUFFER_SIZE];
                if (app_try_extract_prompt_word(content, prompt_word, sizeof(prompt_word))) {
                    strncpy(io_active_prompt_word, prompt_word, io_active_prompt_word_len - 1U);
                    io_active_prompt_word[io_active_prompt_word_len - 1U] = '\0';
                }
            }
        } else {
            ESP_LOGW(TAG, "Prompt fetch failed: %s", esp_err_to_name(prompt_err));
        }
    }

    char prompt_json[128];
    const int prompt_written = snprintf(prompt_json,
                                        sizeof(prompt_json),
                                        "{\"type\":\"prompt\",\"word\":\"%s\"}",
                                        io_active_prompt_word);
    if (prompt_written <= 0 || (size_t)prompt_written >= sizeof(prompt_json)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_err_t ws_err = send_frame(prompt_json, (size_t)prompt_written);
    if (ws_err != ESP_OK) {
        ESP_LOGW(TAG, "Prompt websocket send failed: %s", esp_err_to_name(ws_err));
    }

    ESP_LOGI(TAG, "Active prompt word: %s", io_active_prompt_word);
    return ESP_OK;
}
