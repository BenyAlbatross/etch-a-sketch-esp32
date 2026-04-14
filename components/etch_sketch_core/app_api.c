#include "app_api.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "image_framebuffer.h"
#include "mbedtls/base64.h"

#define APP_OPENAI_RESPONSES_URL "https://api.openai.com/v1/responses"
#define APP_OPENAI_MODEL "gpt-5.4-nano"
#define APP_HTTP_RESPONSE_BUFFER_SIZE 8192
#define APP_AI_CONTENT_BUFFER_SIZE 768
#define APP_HTTP_TIMEOUT_MS 30000
#define APP_HTTP_PERFORM_MAX_ATTEMPTS 3
#define APP_HTTP_ALLOW_INSECURE_TEST_FALLBACK 1
#define APP_HTTP_AUTH_HEADER_PREFIX "Bearer "
#define APP_HTTP_ERROR_BODY_PREVIEW_BYTES 160U
#define APP_PNG_DATA_URL_PREFIX "data:image/png;base64,"
#define APP_PROMPT_WORD_COUNT 8U
#define APP_SUBMIT_CONFIDENCE_PASS_THRESHOLD 6
#define APP_API_LOCAL_DEBUG_MODE 0
#define APP_API_OPENAI_PROMPT_ENABLED 0
#define APP_API_OPENAI_SUBMIT_ENABLED 1
#define APP_PNG_ROW_BYTES (IMAGE_FRAMEBUFFER_CANVAS_WIDTH / 8U)
#define APP_PNG_SCANLINE_BYTES (APP_PNG_ROW_BYTES + 1U)
#define APP_PNG_RAW_IMAGE_BYTES (IMAGE_FRAMEBUFFER_CANVAS_HEIGHT * APP_PNG_SCANLINE_BYTES)
#define APP_PNG_ZLIB_HEADER_BYTES 2U
#define APP_PNG_DEFLATE_BLOCK_HEADER_BYTES 5U
#define APP_PNG_ADLER32_BYTES 4U
#define APP_PNG_IDAT_DATA_BYTES (APP_PNG_ZLIB_HEADER_BYTES + APP_PNG_DEFLATE_BLOCK_HEADER_BYTES + APP_PNG_RAW_IMAGE_BYTES + APP_PNG_ADLER32_BYTES)
#define APP_PNG_SIGNATURE_BYTES 8U
#define APP_PNG_CHUNK_OVERHEAD_BYTES 12U
#define APP_PNG_IHDR_DATA_BYTES 13U
#define APP_PNG_MAX_BYTES (APP_PNG_SIGNATURE_BYTES + \
                           APP_PNG_CHUNK_OVERHEAD_BYTES + APP_PNG_IHDR_DATA_BYTES + \
                           APP_PNG_CHUNK_OVERHEAD_BYTES + APP_PNG_IDAT_DATA_BYTES + \
                           APP_PNG_CHUNK_OVERHEAD_BYTES)
#define APP_ADLER32_MOD 65521U

#if (IMAGE_FRAMEBUFFER_CANVAS_WIDTH % 8U) != 0
#error "PNG encoder requires framebuffer width to be divisible by 8"
#endif

#if APP_PNG_RAW_IMAGE_BYTES > 65535U
#error "PNG encoder uses one uncompressed DEFLATE block and requires raw image bytes <= 65535"
#endif

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} app_http_response_buffer_t;

static const char *TAG = "app_api";
static const char *s_openai_api_key = "";
static bool s_local_debug_mode = (APP_API_LOCAL_DEBUG_MODE != 0);
static size_t s_prompt_word_index;
static const char *APP_PROMPT_WORDS[APP_PROMPT_WORD_COUNT] = {
    "square",
    "triangle",
    "circle",
    "rectangle",
    "house",
    "tree",
    "fish",
    "star",
};

static esp_err_t app_http_post_json(const char *url,
                                    const char *body,
                                    char *out_response,
                                    size_t out_response_len);

static app_api_http_post_fn s_http_post_json = app_http_post_json;

void app_api_set_openai_api_key(const char *api_key)
{
    s_openai_api_key = (api_key != NULL) ? api_key : "";
}

void app_api_set_http_post_for_test(app_api_http_post_fn http_post)
{
    s_http_post_json = (http_post != NULL) ? http_post : app_http_post_json;
}

void app_api_set_local_debug_mode_for_test(bool enabled)
{
#if APP_API_OPENAI_SUBMIT_ENABLED
    s_local_debug_mode = enabled;
#else
    (void)enabled;
    s_local_debug_mode = true;
#endif
    s_prompt_word_index = 0U;
}

static bool app_api_use_local_debug_mode(void)
{
#if APP_API_OPENAI_SUBMIT_ENABLED
    return s_local_debug_mode;
#else
    return true;
#endif
}

static void app_log_submit_heap_snapshot(const char *label)
{
    ESP_LOGI(TAG,
             "%s heap: free=%zu largest=%zu",
             label,
             heap_caps_get_free_size(MALLOC_CAP_8BIT),
             heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static bool app_is_api_key_configured(void)
{
    return (s_openai_api_key[0] != '\0') && (strcmp(s_openai_api_key, "YOUR_OPENAI_API_KEY") != 0);
}

static esp_err_t app_build_authorization_header(char **out_header)
{
    if (out_header == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_header = NULL;
    const size_t prefix_len = sizeof(APP_HTTP_AUTH_HEADER_PREFIX) - 1U;
    const size_t key_len = strlen(s_openai_api_key);
    if (key_len > SIZE_MAX - prefix_len - 1U) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *header = (char *)malloc(prefix_len + key_len + 1U);
    if (header == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(header, APP_HTTP_AUTH_HEADER_PREFIX, prefix_len);
    memcpy(header + prefix_len, s_openai_api_key, key_len + 1U);
    *out_header = header;
    return ESP_OK;
}

#if APP_API_OPENAI_PROMPT_ENABLED
static bool app_is_allowed_prompt_word(const char *word)
{
    if (word == NULL) {
        return false;
    }

    for (size_t i = 0U; i < APP_PROMPT_WORD_COUNT; ++i) {
        if (strcmp(word, APP_PROMPT_WORDS[i]) == 0) {
            return true;
        }
    }
    return false;
}
#endif

static void app_set_submit_result(app_ai_submit_result_t *out_result,
                                  const char *guess,
                                  int confidence,
                                  bool correct)
{
    if (out_result == NULL) {
        return;
    }

    if (guess == NULL) {
        guess = "unknown";
    }
    strncpy(out_result->guess, guess, sizeof(out_result->guess) - 1U);
    out_result->guess[sizeof(out_result->guess) - 1U] = '\0';
    out_result->confidence = confidence;
    out_result->correct = correct;
}

static void app_select_next_local_prompt_word(char *out_word, size_t out_word_len)
{
    if (out_word == NULL || out_word_len == 0U) {
        return;
    }

    const char *word = APP_PROMPT_WORDS[s_prompt_word_index];
    s_prompt_word_index = (s_prompt_word_index + 1U) % APP_PROMPT_WORD_COUNT;
    strncpy(out_word, word, out_word_len - 1U);
    out_word[out_word_len - 1U] = '\0';
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

static esp_err_t app_http_perform_with_retry(esp_http_client_handle_t client, const char *phase)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= APP_HTTP_PERFORM_MAX_ATTEMPTS; ++attempt) {
        err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            return ESP_OK;
        }

        if (err != ESP_ERR_HTTP_EAGAIN || attempt == APP_HTTP_PERFORM_MAX_ATTEMPTS) {
            ESP_LOGE(TAG, "%s HTTP POST failed: %s", phase, esp_err_to_name(err));
            return err;
        }

        ESP_LOGW(TAG,
                 "%s HTTP POST timed out waiting for data (attempt %d/%d), retrying",
                 phase,
                 attempt,
                 APP_HTTP_PERFORM_MAX_ATTEMPTS);
        vTaskDelay(pdMS_TO_TICKS(250U));
    }

    return err;
}

static void app_log_http_error_response(const char *phase, int status_code, const char *response_body)
{
    char preview[APP_HTTP_ERROR_BODY_PREVIEW_BYTES + 1U];
    size_t preview_len = 0U;

    if (response_body != NULL) {
        preview_len = strnlen(response_body, APP_HTTP_ERROR_BODY_PREVIEW_BYTES);
        for (size_t i = 0U; i < preview_len; ++i) {
            const unsigned char ch = (unsigned char)response_body[i];
            preview[i] = (ch >= 0x20U && ch <= 0x7eU) ? (char)ch : ' ';
        }
    }
    preview[preview_len] = '\0';

    if (preview_len > 0U) {
        ESP_LOGE(TAG,
                 "%s HTTP POST rejected: status=%d body='%s%s'",
                 phase,
                 status_code,
                 preview,
                 (response_body[preview_len] != '\0') ? "..." : "");
    } else {
        ESP_LOGE(TAG, "%s HTTP POST rejected: status=%d", phase, status_code);
    }
}

static esp_err_t app_http_post_json_once(const char *url,
                                         const char *body,
                                         char *out_response,
                                         size_t out_response_len,
                                         bool use_cert_bundle,
                                         const char *phase)
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

    char *auth_header = NULL;
    esp_err_t err = app_build_authorization_header(&auth_header);
    if (err != ESP_OK) {
        return err;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = app_http_event_handler,
        .user_data = &response,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = use_cert_bundle ? esp_crt_bundle_attach : NULL,
        .timeout_ms = APP_HTTP_TIMEOUT_MS,
        .method = HTTP_METHOD_POST,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(auth_header);
        return ESP_ERR_NO_MEM;
    }

    err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s HTTP Content-Type header setup failed: %s", phase, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(auth_header);
        return err;
    }

    err = esp_http_client_set_header(client, "Authorization", auth_header);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s HTTP Authorization header setup failed: %s", phase, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(auth_header);
        return err;
    }

    err = esp_http_client_set_post_field(client, body, strlen(body));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s HTTP POST body setup failed: %s", phase, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(auth_header);
        return err;
    }

    err = app_http_perform_with_retry(client, phase);
    if (err == ESP_OK) {
        const int status_code = esp_http_client_get_status_code(client);
        if (status_code < 200 || status_code >= 300) {
            app_log_http_error_response(phase, status_code, out_response);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }

    esp_http_client_cleanup(client);
    free(auth_header);
    return err;
}

static esp_err_t app_http_post_json(const char *url,
                                    const char *body,
                                    char *out_response,
                                    size_t out_response_len)
{
    const esp_err_t secure_err = app_http_post_json_once(url,
                                                         body,
                                                         out_response,
                                                         out_response_len,
                                                         true,
                                                         "Secure TLS");
    if (secure_err == ESP_OK) {
        return ESP_OK;
    }

#if APP_HTTP_ALLOW_INSECURE_TEST_FALLBACK
    if (secure_err == ESP_ERR_HTTP_CONNECT) {
        ESP_LOGW(TAG, "Retrying HTTPS POST without certificate bundle for API bring-up only");
        return app_http_post_json_once(url,
                                       body,
                                       out_response,
                                       out_response_len,
                                       false,
                                       "Insecure test TLS fallback");
    }
#endif

    return secure_err;
}

#if APP_API_OPENAI_PROMPT_ENABLED
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
            return out_word[0] != '\0' && app_is_allowed_prompt_word(out_word);
        }
        cJSON_Delete(content_json);
    }

    app_sanitize_token(content, out_word, out_word_len);
    return out_word[0] != '\0' && app_is_allowed_prompt_word(out_word);
}
#endif

static bool app_copy_string(char *out, size_t out_len, const char *value)
{
    if (out == NULL || out_len == 0U || value == NULL) {
        return false;
    }

    const size_t len = strnlen(value, out_len - 1U);
    memcpy(out, value, len);
    out[len] = '\0';
    return true;
}

static bool app_extract_responses_output_text(const char *response_json,
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

    bool ok = false;
    const cJSON *output_text = cJSON_GetObjectItemCaseSensitive(root, "output_text");
    if (cJSON_IsString(output_text) && output_text->valuestring != NULL) {
        ok = app_copy_string(out_content, out_content_len, output_text->valuestring);
    }

    const cJSON *output = cJSON_GetObjectItemCaseSensitive(root, "output");
    if (!ok && cJSON_IsArray(output)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, output) {
            const cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
            if (!cJSON_IsArray(content)) {
                continue;
            }

            const cJSON *part = NULL;
            cJSON_ArrayForEach(part, content) {
                const cJSON *type = cJSON_GetObjectItemCaseSensitive(part, "type");
                const cJSON *text = cJSON_GetObjectItemCaseSensitive(part, "text");
                if (cJSON_IsString(type) && type->valuestring != NULL &&
                    strcmp(type->valuestring, "output_text") == 0 &&
                    cJSON_IsString(text) && text->valuestring != NULL) {
                    ok = app_copy_string(out_content, out_content_len, text->valuestring);
                    break;
                }
            }
            if (ok) {
                break;
            }
        }
    }

    cJSON_Delete(root);
    return ok;
}

static int app_parse_confidence_value(const cJSON *value)
{
    if (cJSON_IsNumber(value)) {
        return (int)value->valuedouble;
    }

    if (cJSON_IsString(value) && value->valuestring != NULL) {
        int parsed = 0;
        if (sscanf(value->valuestring, "%d", &parsed) == 1) {
            return parsed;
        }
    }

    return -1;
}

bool app_api_parse_submit_response_text(const char *response_text,
                                        app_ai_submit_result_t *out_result)
{
    if (response_text == NULL || out_result == NULL) {
        return false;
    }

    cJSON *root = cJSON_Parse(response_text);
    if (root == NULL) {
        return false;
    }

    const cJSON *guess = cJSON_GetObjectItemCaseSensitive(root, "guess");
    const cJSON *confidence = cJSON_GetObjectItemCaseSensitive(root, "confidence");
    const cJSON *correct = cJSON_GetObjectItemCaseSensitive(root, "correct");
    const int parsed_confidence = app_parse_confidence_value(confidence);

    bool ok = false;
    if (cJSON_IsString(guess) && guess->valuestring != NULL &&
        parsed_confidence >= 0 && cJSON_IsBool(correct)) {
        const int clamped_confidence = (parsed_confidence < 1) ? 1 :
            (parsed_confidence > 10) ? 10 : parsed_confidence;
        app_set_submit_result(out_result,
                              guess->valuestring,
                              clamped_confidence,
                              cJSON_IsTrue(correct));
        ok = true;
    }

    cJSON_Delete(root);
    return ok;
}

static esp_err_t app_extract_framebuffer_base64(const char *payload,
                                                size_t payload_len,
                                                char **out_base64,
                                                size_t *out_base64_len)
{
    if (payload == NULL || payload_len == 0U || out_base64 == NULL || out_base64_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_base64 = NULL;
    *out_base64_len = 0U;

    char *payload_copy = (char *)malloc(payload_len + 1U);
    if (payload_copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(payload_copy, payload, payload_len);
    payload_copy[payload_len] = '\0';

    cJSON *root = cJSON_Parse(payload_copy);
    free(payload_copy);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *width = cJSON_GetObjectItemCaseSensitive(root, "width");
    const cJSON *height = cJSON_GetObjectItemCaseSensitive(root, "height");
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(root, "format");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");

    if (!cJSON_IsString(type) || strcmp(type->valuestring, "frame") != 0 ||
        !cJSON_IsNumber(width) || (int)width->valuedouble != IMAGE_FRAMEBUFFER_CANVAS_WIDTH ||
        !cJSON_IsNumber(height) || (int)height->valuedouble != IMAGE_FRAMEBUFFER_CANVAS_HEIGHT ||
        !cJSON_IsString(format) || strcmp(format->valuestring, "1bpp-msb") != 0 ||
        !cJSON_IsString(data) || data->valuestring == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const size_t data_len = strlen(data->valuestring);
    char *base64 = (char *)malloc(data_len + 1U);
    if (base64 == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    memcpy(base64, data->valuestring, data_len + 1U);

    cJSON_Delete(root);
    *out_base64 = base64;
    *out_base64_len = data_len;
    return ESP_OK;
}

static esp_err_t app_decode_framebuffer_bytes(const char *payload,
                                              size_t payload_len,
                                              uint8_t **out_framebuffer)
{
    if (out_framebuffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_framebuffer = NULL;

    char *base64 = NULL;
    size_t base64_len = 0U;
    esp_err_t err = app_extract_framebuffer_base64(payload, payload_len, &base64, &base64_len);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t *framebuffer = (uint8_t *)malloc(IMAGE_FRAMEBUFFER_BYTES);
    if (framebuffer == NULL) {
        free(base64);
        return ESP_ERR_NO_MEM;
    }

    size_t raw_len = 0U;
    const int ret = mbedtls_base64_decode(framebuffer,
                                          IMAGE_FRAMEBUFFER_BYTES,
                                          &raw_len,
                                          (const unsigned char *)base64,
                                          base64_len);
    free(base64);
    if (ret != 0 || raw_len != IMAGE_FRAMEBUFFER_BYTES) {
        free(framebuffer);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_framebuffer = framebuffer;
    return ESP_OK;
}

static void app_write_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void app_write_le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static uint32_t app_crc32_update(uint32_t crc, const uint8_t *data, size_t data_len)
{
    for (size_t i = 0U; i < data_len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)(0U - (crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return crc;
}

static uint32_t app_png_chunk_crc(const char type[4], const uint8_t *data, size_t data_len)
{
    uint32_t crc = 0xffffffffU;
    crc = app_crc32_update(crc, (const uint8_t *)type, 4U);
    if (data != NULL && data_len > 0U) {
        crc = app_crc32_update(crc, data, data_len);
    }
    return crc ^ 0xffffffffU;
}

static uint32_t app_adler32(const uint8_t *data, size_t data_len)
{
    uint32_t a = 1U;
    uint32_t b = 0U;
    for (size_t i = 0U; i < data_len; ++i) {
        a = (a + data[i]) % APP_ADLER32_MOD;
        b = (b + a) % APP_ADLER32_MOD;
    }
    return (b << 16U) | a;
}

static void app_build_png_raw_scanlines(const uint8_t *framebuffer, uint8_t *raw)
{
    size_t raw_offset = 0U;
    for (uint32_t y = 0U; y < IMAGE_FRAMEBUFFER_CANVAS_HEIGHT; ++y) {
        const size_t framebuffer_offset = (size_t)y * APP_PNG_ROW_BYTES;
        raw[raw_offset++] = 0U;
        for (size_t x = 0U; x < APP_PNG_ROW_BYTES; ++x) {
            raw[raw_offset++] = (uint8_t)~framebuffer[framebuffer_offset + x];
        }
    }
}

static esp_err_t app_png_append_chunk(uint8_t *png,
                                      size_t png_capacity,
                                      size_t *png_len,
                                      const char type[4],
                                      const uint8_t *data,
                                      size_t data_len)
{
    if (png == NULL || png_len == NULL || type == NULL || data_len > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*png_len > png_capacity || png_capacity - *png_len < APP_PNG_CHUNK_OVERHEAD_BYTES + data_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *chunk = png + *png_len;
    app_write_be32(chunk, (uint32_t)data_len);
    memcpy(chunk + 4U, type, 4U);
    if (data != NULL && data_len > 0U) {
        memcpy(chunk + 8U, data, data_len);
    }
    app_write_be32(chunk + 8U + data_len, app_png_chunk_crc(type, data, data_len));
    *png_len += APP_PNG_CHUNK_OVERHEAD_BYTES + data_len;
    return ESP_OK;
}

static esp_err_t app_png_append_uncompressed_idat(uint8_t *png,
                                                  size_t png_capacity,
                                                  size_t *png_len,
                                                  const uint8_t *raw,
                                                  size_t raw_len)
{
    if (png == NULL || png_len == NULL || raw == NULL || raw_len > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*png_len > png_capacity || png_capacity - *png_len < APP_PNG_CHUNK_OVERHEAD_BYTES + APP_PNG_IDAT_DATA_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *chunk = png + *png_len;
    uint8_t *idat = chunk + 8U;
    size_t idat_offset = 0U;
    const uint16_t block_len = (uint16_t)raw_len;
    const uint16_t block_nlen = (uint16_t)~block_len;

    app_write_be32(chunk, APP_PNG_IDAT_DATA_BYTES);
    memcpy(chunk + 4U, "IDAT", 4U);

    idat[idat_offset++] = 0x78U;
    idat[idat_offset++] = 0x01U;
    idat[idat_offset++] = 0x01U;
    app_write_le16(idat + idat_offset, block_len);
    idat_offset += 2U;
    app_write_le16(idat + idat_offset, block_nlen);
    idat_offset += 2U;
    memcpy(idat + idat_offset, raw, raw_len);
    idat_offset += raw_len;
    app_write_be32(idat + idat_offset, app_adler32(raw, raw_len));
    idat_offset += APP_PNG_ADLER32_BYTES;

    if (idat_offset != APP_PNG_IDAT_DATA_BYTES) {
        return ESP_FAIL;
    }

    app_write_be32(chunk + 8U + idat_offset, app_png_chunk_crc("IDAT", idat, idat_offset));
    *png_len += APP_PNG_CHUNK_OVERHEAD_BYTES + idat_offset;
    return ESP_OK;
}

static esp_err_t app_encode_framebuffer_png_1bpp(const uint8_t *framebuffer,
                                                 uint8_t *out_png,
                                                 size_t out_png_capacity,
                                                 size_t *out_png_len)
{
    if (framebuffer == NULL || out_png == NULL || out_png_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[APP_PNG_RAW_IMAGE_BYTES];
    uint8_t ihdr[APP_PNG_IHDR_DATA_BYTES] = {0};
    static const uint8_t png_signature[APP_PNG_SIGNATURE_BYTES] = {
        0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n',
    };

    if (out_png_capacity < APP_PNG_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    app_build_png_raw_scanlines(framebuffer, raw);
    app_write_be32(ihdr, IMAGE_FRAMEBUFFER_CANVAS_WIDTH);
    app_write_be32(ihdr + 4U, IMAGE_FRAMEBUFFER_CANVAS_HEIGHT);
    ihdr[8] = 1U;
    ihdr[9] = 0U;
    ihdr[10] = 0U;
    ihdr[11] = 0U;
    ihdr[12] = 0U;

    size_t png_len = 0U;
    memcpy(out_png, png_signature, sizeof(png_signature));
    png_len += sizeof(png_signature);

    esp_err_t err = app_png_append_chunk(out_png, out_png_capacity, &png_len, "IHDR", ihdr, sizeof(ihdr));
    if (err != ESP_OK) {
        return err;
    }
    err = app_png_append_uncompressed_idat(out_png, out_png_capacity, &png_len, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }
    err = app_png_append_chunk(out_png, out_png_capacity, &png_len, "IEND", NULL, 0U);
    if (err != ESP_OK) {
        return err;
    }

    *out_png_len = png_len;
    return ESP_OK;
}

static esp_err_t app_build_png_data_url_from_framebuffer(const uint8_t *framebuffer,
                                                         char **out_data_url,
                                                         size_t *out_data_url_len)
{
    if (framebuffer == NULL || out_data_url == NULL || out_data_url_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_data_url = NULL;
    *out_data_url_len = 0U;

    uint8_t png[APP_PNG_MAX_BYTES];
    size_t png_len = 0U;
    esp_err_t err = app_encode_framebuffer_png_1bpp(framebuffer, png, sizeof(png), &png_len);
    if (err != ESP_OK) {
        return err;
    }

    const size_t prefix_len = sizeof(APP_PNG_DATA_URL_PREFIX) - 1U;
    const size_t base64_len = 4U * ((png_len + 2U) / 3U);
    char *data_url = (char *)malloc(prefix_len + base64_len + 1U);
    if (data_url == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(data_url, APP_PNG_DATA_URL_PREFIX, prefix_len);
    size_t written = 0U;
    const int ret = mbedtls_base64_encode((unsigned char *)(data_url + prefix_len),
                                          base64_len + 1U,
                                          &written,
                                          png,
                                          png_len);
    if (ret != 0) {
        free(data_url);
        return ESP_FAIL;
    }

    data_url[prefix_len + written] = '\0';
    *out_data_url = data_url;
    *out_data_url_len = prefix_len + written;
    return ESP_OK;
}

esp_err_t app_api_build_frame_png_data_url(const char *payload,
                                           size_t payload_len,
                                           char **out_data_url,
                                           size_t *out_data_url_len)
{
    if (out_data_url == NULL || out_data_url_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_data_url = NULL;
    *out_data_url_len = 0U;

    uint8_t *framebuffer = NULL;
    esp_err_t err = app_decode_framebuffer_bytes(payload, payload_len, &framebuffer);
    if (err != ESP_OK) {
        return err;
    }

    err = app_build_png_data_url_from_framebuffer(framebuffer, out_data_url, out_data_url_len);
    free(framebuffer);
    return err;
}

static cJSON *app_create_string_array(const char *const *values, size_t value_count)
{
    cJSON *array = cJSON_CreateArray();
    if (array == NULL) {
        return NULL;
    }

    for (size_t i = 0U; i < value_count; ++i) {
        cJSON *value = cJSON_CreateString(values[i]);
        if (value == NULL) {
            cJSON_Delete(array);
            return NULL;
        }
        cJSON_AddItemToArray(array, value);
    }

    return array;
}

#if APP_API_OPENAI_PROMPT_ENABLED
static cJSON *app_create_prompt_schema(void)
{
    cJSON *schema = cJSON_CreateObject();
    cJSON *properties = cJSON_CreateObject();
    cJSON *word = cJSON_CreateObject();
    cJSON *required = NULL;
    cJSON *enum_values = NULL;
    if (schema == NULL || properties == NULL || word == NULL) {
        cJSON_Delete(schema);
        cJSON_Delete(properties);
        cJSON_Delete(word);
        return NULL;
    }

    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddStringToObject(word, "type", "string");
    enum_values = app_create_string_array(APP_PROMPT_WORDS, APP_PROMPT_WORD_COUNT);
    if (enum_values == NULL) {
        cJSON_Delete(schema);
        cJSON_Delete(properties);
        cJSON_Delete(word);
        return NULL;
    }
    cJSON_AddItemToObject(word, "enum", enum_values);
    cJSON_AddItemToObject(properties, "word", word);
    cJSON_AddItemToObject(schema, "properties", properties);
    required = app_create_string_array((const char *const[]){"word"}, 1U);
    if (required == NULL) {
        cJSON_Delete(schema);
        return NULL;
    }
    cJSON_AddItemToObject(schema, "required", required);
    cJSON_AddBoolToObject(schema, "additionalProperties", false);
    return schema;
}
#endif

static cJSON *app_create_submit_schema(void)
{
    cJSON *schema = cJSON_CreateObject();
    cJSON *properties = cJSON_CreateObject();
    cJSON *guess = cJSON_CreateObject();
    cJSON *confidence = cJSON_CreateObject();
    cJSON *correct = cJSON_CreateObject();
    cJSON *required = NULL;
    if (schema == NULL || properties == NULL || guess == NULL ||
        confidence == NULL || correct == NULL) {
        cJSON_Delete(schema);
        cJSON_Delete(properties);
        cJSON_Delete(guess);
        cJSON_Delete(confidence);
        cJSON_Delete(correct);
        return NULL;
    }

    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddStringToObject(guess, "type", "string");
    cJSON_AddStringToObject(confidence, "type", "integer");
    cJSON_AddNumberToObject(confidence, "minimum", 1);
    cJSON_AddNumberToObject(confidence, "maximum", 10);
    cJSON_AddStringToObject(correct, "type", "boolean");

    cJSON_AddItemToObject(properties, "guess", guess);
    cJSON_AddItemToObject(properties, "confidence", confidence);
    cJSON_AddItemToObject(properties, "correct", correct);
    cJSON_AddItemToObject(schema, "properties", properties);

    required = app_create_string_array((const char *const[]){"guess", "confidence", "correct"}, 3U);
    if (required == NULL) {
        cJSON_Delete(schema);
        return NULL;
    }
    cJSON_AddItemToObject(schema, "required", required);
    cJSON_AddBoolToObject(schema, "additionalProperties", false);
    return schema;
}

static bool app_add_text_format(cJSON *root, const char *name, cJSON *schema)
{
    if (root == NULL || name == NULL || schema == NULL) {
        cJSON_Delete(schema);
        return false;
    }

    cJSON *text = cJSON_CreateObject();
    cJSON *format = cJSON_CreateObject();
    if (text == NULL || format == NULL) {
        cJSON_Delete(text);
        cJSON_Delete(format);
        cJSON_Delete(schema);
        return false;
    }

    cJSON_AddStringToObject(format, "type", "json_schema");
    cJSON_AddStringToObject(format, "name", name);
    cJSON_AddItemToObject(format, "schema", schema);
    cJSON_AddBoolToObject(format, "strict", true);
    cJSON_AddItemToObject(text, "format", format);
    cJSON_AddItemToObject(root, "text", text);
    return true;
}

#if APP_API_OPENAI_PROMPT_ENABLED
static char *app_build_prompt_request_body(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *input = cJSON_CreateArray();
    cJSON *message = cJSON_CreateObject();
    if (root == NULL || input == NULL || message == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(input);
        cJSON_Delete(message);
        return NULL;
    }

    cJSON_AddStringToObject(root, "model", APP_OPENAI_MODEL);
    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(message,
                            "content",
                            "Select exactly one word from the configured Pictionary list. Return only the structured JSON.");
    cJSON_AddItemToArray(input, message);
    cJSON_AddItemToObject(root, "input", input);
    if (!app_add_text_format(root, "etch_prompt_word", app_create_prompt_schema())) {
        cJSON_Delete(root);
        return NULL;
    }

    char *request_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return request_body;
}
#endif

static char *app_build_submit_request_body(const char *active_prompt_word, const char *data_url)
{
    if (active_prompt_word == NULL || data_url == NULL) {
        return NULL;
    }

    char instruction[768];
    const int instruction_written = snprintf(instruction,
                                             sizeof(instruction),
                                             "Grade this Etch-a-Sketch drawing against the target word '%s'. "
                                             "Return whether the drawing is recognizable as the target. "
                                             "Use confidence 1 for no match and 10 for excellent match; set correct true at confidence %d or higher. "
                                             "Return only the structured JSON.",
                                             active_prompt_word,
                                             APP_SUBMIT_CONFIDENCE_PASS_THRESHOLD);
    if (instruction_written <= 0 || (size_t)instruction_written >= sizeof(instruction)) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *input = cJSON_CreateArray();
    cJSON *message = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *input_text = cJSON_CreateObject();
    cJSON *input_image = cJSON_CreateObject();
    if (root == NULL || input == NULL || message == NULL || content == NULL ||
        input_text == NULL || input_image == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(input);
        cJSON_Delete(message);
        cJSON_Delete(content);
        cJSON_Delete(input_text);
        cJSON_Delete(input_image);
        return NULL;
    }

    cJSON_AddStringToObject(root, "model", APP_OPENAI_MODEL);
    cJSON_AddNumberToObject(root, "max_output_tokens", 300);
    cJSON_AddStringToObject(message, "role", "user");

    cJSON_AddStringToObject(input_text, "type", "input_text");
    cJSON_AddStringToObject(input_text, "text", instruction);
    cJSON_AddItemToArray(content, input_text);

    cJSON_AddStringToObject(input_image, "type", "input_image");
    cJSON *image_url = cJSON_CreateStringReference(data_url);
    if (image_url == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(input);
        cJSON_Delete(message);
        cJSON_Delete(content);
        cJSON_Delete(input_image);
        return NULL;
    }
    cJSON_AddItemToObject(input_image, "image_url", image_url);
    cJSON_AddStringToObject(input_image, "detail", "low");
    cJSON_AddItemToArray(content, input_image);

    cJSON_AddItemToObject(message, "content", content);
    cJSON_AddItemToArray(input, message);
    cJSON_AddItemToObject(root, "input", input);
    if (!app_add_text_format(root, "etch_submit_result", app_create_submit_schema())) {
        cJSON_Delete(root);
        return NULL;
    }

    char *request_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return request_body;
}

esp_err_t app_api_submit_drawing(const char *payload,
                                 size_t payload_len,
                                 const char *active_prompt_word,
                                 app_ai_submit_result_t *out_result,
                                 bool *out_submit_success,
                                 bool local_debug_submit_correct)
{
    if (payload == NULL || payload_len == 0U || active_prompt_word == NULL ||
        out_result == NULL || out_submit_success == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_submit_success = false;
    app_set_submit_result(out_result, "unknown", 1, false);

    if (app_api_use_local_debug_mode()) {
        const bool correct = local_debug_submit_correct;
        const char *guess = (active_prompt_word[0] != '\0') ? active_prompt_word : "local-debug";
        app_set_submit_result(out_result, guess, correct ? 10 : 1, correct);
        *out_submit_success = true;
        ESP_LOGI(TAG,
                 "Local debug submit result: guess='%s', confidence=%d, correct=%u",
                 out_result->guess,
                 out_result->confidence,
                 out_result->correct ? 1U : 0U);
        return ESP_OK;
    }

#if !APP_API_OPENAI_SUBMIT_ENABLED
    ESP_LOGW(TAG, "OpenAI submit path disabled by APP_API_OPENAI_SUBMIT_ENABLED");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!app_is_api_key_configured()) {
        ESP_LOGW(TAG, "OpenAI API key not configured, unable to submit drawing");
        return ESP_ERR_INVALID_STATE;
    }

    char *data_url = NULL;
    size_t data_url_len = 0U;
    app_log_submit_heap_snapshot("Before PNG data URL build");
    esp_err_t err = app_api_build_frame_png_data_url(payload, payload_len, &data_url, &data_url_len);
    app_log_submit_heap_snapshot("After PNG data URL build");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build PNG data URL: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Built 1-bit PNG data URL for submit (%zu bytes)", data_url_len);

    char *request_body = app_build_submit_request_body(active_prompt_word, data_url);
    free(data_url);
    if (request_body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char *http_response = (char *)malloc(APP_HTTP_RESPONSE_BUFFER_SIZE);
    if (http_response == NULL) {
        free(request_body);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Sending drawing submit request via OpenAI Responses API");
    err = s_http_post_json(APP_OPENAI_RESPONSES_URL,
                           request_body,
                           http_response,
                           APP_HTTP_RESPONSE_BUFFER_SIZE);
    free(request_body);
    if (err != ESP_OK) {
        free(http_response);
        ESP_LOGE(TAG, "OpenAI Responses submit failed: %s", esp_err_to_name(err));
        app_set_submit_result(out_result, "api-error", 1, false);
        return err;
    }

    char content[APP_AI_CONTENT_BUFFER_SIZE];
    if (!app_extract_responses_output_text(http_response, content, sizeof(content)) ||
        !app_api_parse_submit_response_text(content, out_result)) {
        free(http_response);
        ESP_LOGW(TAG, "Failed to parse OpenAI Responses submit output");
        app_set_submit_result(out_result, "parse-error", 1, false);
        return ESP_ERR_INVALID_RESPONSE;
    }
    free(http_response);

    *out_submit_success = true;
    ESP_LOGI(TAG,
             "Submit result: guess='%s', confidence=%d, correct=%u",
             out_result->guess,
             out_result->confidence,
             out_result->correct ? 1U : 0U);
    return ESP_OK;
#endif
}

esp_err_t app_api_fetch_and_publish_prompt(app_api_send_frame_fn send_frame,
                                           char *io_active_prompt_word,
                                           size_t io_active_prompt_word_len)
{
    if (send_frame == NULL || io_active_prompt_word == NULL || io_active_prompt_word_len < 2U) {
        return ESP_ERR_INVALID_ARG;
    }

    app_select_next_local_prompt_word(io_active_prompt_word, io_active_prompt_word_len);
    ESP_LOGI(TAG, "Using local prompt word list");

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
