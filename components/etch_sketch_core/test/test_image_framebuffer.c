#include "unity.h"

#include <stdlib.h>
#include <string.h>

#include "app_api.h"
#include "cJSON.h"
#include "image_framebuffer.h"
#include "mbedtls/base64.h"

static bool s_submit_http_fake_called;
static bool s_submit_http_fake_url_valid;
static bool s_submit_http_fake_request_valid;
static bool s_prompt_send_fake_called;
static char s_prompt_send_fake_payload[128];

#define TEST_PNG_DATA_URL_PREFIX "data:image/png;base64,"
#define TEST_PNG_ROW_BYTES (IMAGE_FRAMEBUFFER_CANVAS_WIDTH / 8U)
#define TEST_PNG_SCANLINE_BYTES (TEST_PNG_ROW_BYTES + 1U)
#define TEST_PNG_RAW_IMAGE_BYTES (IMAGE_FRAMEBUFFER_CANVAS_HEIGHT * TEST_PNG_SCANLINE_BYTES)
#define TEST_PNG_IDAT_DATA_BYTES (2U + 5U + TEST_PNG_RAW_IMAGE_BYTES + 4U)
#define TEST_PNG_EXPECTED_BYTES (8U + 12U + 13U + 12U + TEST_PNG_IDAT_DATA_BYTES + 12U)

static uint32_t test_read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) |
           ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) |
           (uint32_t)data[3];
}

static bool decode_base64_segment(const char *encoded,
                                  size_t encoded_len,
                                  uint8_t **out_decoded,
                                  size_t *out_decoded_len)
{
    if (encoded == NULL || encoded_len == 0U || out_decoded == NULL || out_decoded_len == NULL) {
        return false;
    }

    const size_t decoded_capacity = ((encoded_len * 3U) / 4U) + 4U;
    uint8_t *decoded = (uint8_t *)malloc(decoded_capacity);
    if (decoded == NULL) {
        return false;
    }

    size_t decoded_len = 0U;
    const int decode_ret = mbedtls_base64_decode(decoded,
                                                 decoded_capacity,
                                                 &decoded_len,
                                                 (const unsigned char *)encoded,
                                                 encoded_len);
    if (decode_ret != 0) {
        free(decoded);
        return false;
    }

    *out_decoded = decoded;
    *out_decoded_len = decoded_len;
    return true;
}

static bool decode_png_data_url(const char *data_url, uint8_t **out_png, size_t *out_png_len)
{
    const size_t prefix_len = strlen(TEST_PNG_DATA_URL_PREFIX);
    if (data_url == NULL || strncmp(data_url, TEST_PNG_DATA_URL_PREFIX, prefix_len) != 0) {
        return false;
    }

    const char *encoded = data_url + prefix_len;
    return decode_base64_segment(encoded, strlen(encoded), out_png, out_png_len);
}

static bool data_url_decodes_to_png(const char *data_url)
{
    uint8_t *decoded = NULL;
    size_t decoded_len = 0U;
    if (!decode_png_data_url(data_url, &decoded, &decoded_len)) {
        return false;
    }

    const uint8_t png_signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    const bool is_png = (decoded_len >= sizeof(png_signature)) &&
                        (memcmp(decoded, png_signature, sizeof(png_signature)) == 0);
    free(decoded);
    return is_png;
}

static bool json_string_field_equals(const cJSON *object, const char *name, const char *expected)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring != NULL &&
           strcmp(item->valuestring, expected) == 0;
}

static bool json_array_contains_string(const cJSON *array, const char *expected)
{
    if (!cJSON_IsArray(array)) {
        return false;
    }

    const int count = cJSON_GetArraySize(array);
    for (int i = 0; i < count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(array, i);
        if (cJSON_IsString(item) && item->valuestring != NULL &&
            strcmp(item->valuestring, expected) == 0) {
            return true;
        }
    }
    return false;
}

static bool submit_schema_is_valid(const cJSON *schema)
{
    if (!cJSON_IsObject(schema) || !json_string_field_equals(schema, "type", "object")) {
        return false;
    }

    const cJSON *additional = cJSON_GetObjectItemCaseSensitive(schema, "additionalProperties");
    const cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    const cJSON *required = cJSON_GetObjectItemCaseSensitive(schema, "required");
    if (!cJSON_IsBool(additional) || cJSON_IsTrue(additional) ||
        !cJSON_IsObject(properties) || !cJSON_IsArray(required) ||
        cJSON_GetArraySize(required) != 3) {
        return false;
    }

    if (cJSON_GetObjectItemCaseSensitive(properties, "reasoning") != NULL ||
        !json_array_contains_string(required, "guess") ||
        !json_array_contains_string(required, "confidence") ||
        !json_array_contains_string(required, "correct")) {
        return false;
    }

    const cJSON *guess = cJSON_GetObjectItemCaseSensitive(properties, "guess");
    const cJSON *confidence = cJSON_GetObjectItemCaseSensitive(properties, "confidence");
    const cJSON *correct = cJSON_GetObjectItemCaseSensitive(properties, "correct");
    const cJSON *minimum = cJSON_GetObjectItemCaseSensitive(confidence, "minimum");
    const cJSON *maximum = cJSON_GetObjectItemCaseSensitive(confidence, "maximum");
    return json_string_field_equals(guess, "type", "string") &&
           json_string_field_equals(confidence, "type", "integer") &&
           cJSON_IsNumber(minimum) && (int)minimum->valuedouble == 1 &&
           cJSON_IsNumber(maximum) && (int)maximum->valuedouble == 10 &&
           json_string_field_equals(correct, "type", "boolean");
}

static bool submit_text_format_is_valid(const cJSON *root)
{
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(text, "format");
    const cJSON *strict = cJSON_GetObjectItemCaseSensitive(format, "strict");
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(format, "schema");

    return cJSON_IsObject(text) &&
           cJSON_IsObject(format) &&
           json_string_field_equals(format, "type", "json_schema") &&
           json_string_field_equals(format, "name", "etch_submit_result") &&
           cJSON_IsTrue(strict) &&
           submit_schema_is_valid(schema);
}

static bool submit_content_is_valid(const cJSON *content, const char *expected_prompt)
{
    if (!cJSON_IsArray(content) || cJSON_GetArraySize(content) != 2) {
        return false;
    }

    bool saw_input_text = false;
    bool saw_input_image = false;
    const int count = cJSON_GetArraySize(content);
    for (int i = 0; i < count; ++i) {
        const cJSON *part = cJSON_GetArrayItem(content, i);
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(part, "type");
        if (!cJSON_IsString(type) || type->valuestring == NULL) {
            return false;
        }

        if (strcmp(type->valuestring, "input_text") == 0) {
            const cJSON *text = cJSON_GetObjectItemCaseSensitive(part, "text");
            saw_input_text = cJSON_IsString(text) &&
                             text->valuestring != NULL &&
                             strstr(text->valuestring, expected_prompt) != NULL;
        } else if (strcmp(type->valuestring, "input_image") == 0) {
            const cJSON *image_url = cJSON_GetObjectItemCaseSensitive(part, "image_url");
            saw_input_image = cJSON_IsString(image_url) &&
                              image_url->valuestring != NULL &&
                              json_string_field_equals(part, "detail", "low") &&
                              data_url_decodes_to_png(image_url->valuestring);
        } else {
            return false;
        }
    }

    return saw_input_text && saw_input_image;
}

static bool request_body_matches_submit_contract(const char *body, const char *expected_prompt)
{
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return false;
    }

    const cJSON *max_output_tokens = cJSON_GetObjectItemCaseSensitive(root, "max_output_tokens");
    const cJSON *input = cJSON_GetObjectItemCaseSensitive(root, "input");
    const cJSON *message = cJSON_GetArrayItem(input, 0);
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");

    const bool ok = json_string_field_equals(root, "model", "gpt-5.4-nano") &&
                    cJSON_IsNumber(max_output_tokens) &&
                    (int)max_output_tokens->valuedouble == 300 &&
                    cJSON_IsArray(input) &&
                    cJSON_GetArraySize(input) == 1 &&
                    cJSON_IsObject(message) &&
                    json_string_field_equals(message, "role", "user") &&
                    submit_content_is_valid(content, expected_prompt) &&
                    submit_text_format_is_valid(root);

    cJSON_Delete(root);
    return ok;
}

static bool find_png_chunk(const uint8_t *png,
                           size_t png_len,
                           const char type[4],
                           const uint8_t **out_data,
                           size_t *out_data_len)
{
    const uint8_t png_signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    if (png == NULL || png_len < sizeof(png_signature) || memcmp(png, png_signature, sizeof(png_signature)) != 0) {
        return false;
    }

    size_t offset = sizeof(png_signature);
    while (offset + 12U <= png_len) {
        const uint32_t chunk_len = test_read_be32(png + offset);
        const uint8_t *chunk_type = png + offset + 4U;
        const uint8_t *chunk_data = png + offset + 8U;
        if ((size_t)chunk_len > png_len - offset - 12U) {
            return false;
        }
        if (memcmp(chunk_type, type, 4U) == 0) {
            if (out_data != NULL) {
                *out_data = chunk_data;
            }
            if (out_data_len != NULL) {
                *out_data_len = (size_t)chunk_len;
            }
            return true;
        }
        offset += 12U + (size_t)chunk_len;
    }

    return false;
}

void setUp(void)
{
    s_submit_http_fake_called = false;
    s_submit_http_fake_url_valid = false;
    s_submit_http_fake_request_valid = false;
    s_prompt_send_fake_called = false;
    s_prompt_send_fake_payload[0] = '\0';
    app_api_set_openai_api_key("");
    app_api_set_http_post_for_test(NULL);
    app_api_set_local_debug_mode_for_test(true);
}

void tearDown(void)
{
    app_api_set_openai_api_key("");
    app_api_set_http_post_for_test(NULL);
    app_api_set_local_debug_mode_for_test(true);
}

static esp_err_t submit_http_fake(const char *url,
                                  const char *body,
                                  char *out_response,
                                  size_t out_response_len)
{
    const char *response =
        "{\"output_text\":\"{\\\"guess\\\":\\\"square\\\",\\\"confidence\\\":9,\\\"correct\\\":true}\"}";

    s_submit_http_fake_called = true;
    s_submit_http_fake_url_valid = (url != NULL) && (strcmp(url, "https://api.openai.com/v1/responses") == 0);
    s_submit_http_fake_request_valid = request_body_matches_submit_contract(body, "square");

    if (out_response == NULL || out_response_len <= strlen(response)) {
        return ESP_ERR_INVALID_SIZE;
    }

    strcpy(out_response, response);
    return ESP_OK;
}

static esp_err_t prompt_send_fake(const char *payload, size_t payload_len)
{
    s_prompt_send_fake_called = true;
    if (payload == NULL || payload_len >= sizeof(s_prompt_send_fake_payload)) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(s_prompt_send_fake_payload, payload, payload_len);
    s_prompt_send_fake_payload[payload_len] = '\0';
    return ESP_OK;
}

static bool framebuffer_pixel_is_set(const image_framebuffer_t *framebuffer, uint16_t x, uint16_t y)
{
    const size_t bit_index = ((size_t)y * IMAGE_FRAMEBUFFER_CANVAS_WIDTH) + x;
    const size_t byte_index = bit_index >> 3U;
    const uint8_t bit_mask = (uint8_t)(0x80U >> (bit_index & 0x07U));
    return (framebuffer->framebuffer[byte_index] & bit_mask) != 0U;
}

TEST_CASE("pen lift breaks framebuffer line continuity", "[image_framebuffer]")
{
    image_framebuffer_t framebuffer;
    image_framebuffer_init(&framebuffer);

    image_framebuffer_apply_input(&framebuffer, &(image_input_state_t){.x = 1, .y = 1, .pen_down = true});
    image_framebuffer_apply_input(&framebuffer, &(image_input_state_t){.x = 8, .y = 1, .pen_down = false});
    image_framebuffer_apply_input(&framebuffer, &(image_input_state_t){.x = 8, .y = 8, .pen_down = true});

    TEST_ASSERT_TRUE(framebuffer_pixel_is_set(&framebuffer, 1, 1));
    TEST_ASSERT_TRUE(framebuffer_pixel_is_set(&framebuffer, 8, 8));
    TEST_ASSERT_FALSE(framebuffer_pixel_is_set(&framebuffer, 8, 2));
}

TEST_CASE("erase clears previous framebuffer pixels", "[image_framebuffer]")
{
    image_framebuffer_t framebuffer;
    image_framebuffer_init(&framebuffer);

    image_framebuffer_apply_input(&framebuffer, &(image_input_state_t){.x = 4, .y = 4, .pen_down = true});
    TEST_ASSERT_TRUE(framebuffer_pixel_is_set(&framebuffer, 4, 4));

    image_framebuffer_apply_input(&framebuffer, &(image_input_state_t){.x = 5, .y = 5, .pen_down = true, .erase = true});
    TEST_ASSERT_FALSE(framebuffer_pixel_is_set(&framebuffer, 4, 4));
    TEST_ASSERT_TRUE(framebuffer_pixel_is_set(&framebuffer, 5, 5));
    TEST_ASSERT_FALSE(framebuffer_pixel_is_set(&framebuffer, 4, 5));
}

TEST_CASE("framebuffer base64 export fits documented buffer size", "[image_framebuffer]")
{
    image_framebuffer_t framebuffer;
    image_framebuffer_init(&framebuffer);

    char encoded[IMAGE_FRAMEBUFFER_BASE64_BUFFER_SIZE];
    const size_t encoded_len = image_framebuffer_build_base64_data(&framebuffer, encoded, sizeof(encoded));

    TEST_ASSERT_GREATER_THAN(0U, encoded_len);
    TEST_ASSERT_EQUAL('\0', encoded[encoded_len]);
}

TEST_CASE("png data url contains required png chunks", "[app_api]")
{
    image_framebuffer_t framebuffer;
    image_framebuffer_init(&framebuffer);

    char payload[4U * ((IMAGE_FRAMEBUFFER_BYTES + 2U) / 3U) + 160U];
    const size_t payload_len = image_framebuffer_build_socket_payload(&framebuffer, payload, sizeof(payload));
    TEST_ASSERT_GREATER_THAN(0U, payload_len);

    char *data_url = NULL;
    size_t data_url_len = 0U;
    TEST_ASSERT_EQUAL(ESP_OK, app_api_build_frame_png_data_url(payload, payload_len, &data_url, &data_url_len));
    TEST_ASSERT_NOT_NULL(data_url);
    TEST_ASSERT_GREATER_THAN(strlen(TEST_PNG_DATA_URL_PREFIX), data_url_len);

    uint8_t *png = NULL;
    size_t png_len = 0U;
    TEST_ASSERT_TRUE(decode_png_data_url(data_url, &png, &png_len));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)TEST_PNG_EXPECTED_BYTES, (uint32_t)png_len);

    const uint8_t *chunk = NULL;
    size_t chunk_len = 0U;
    TEST_ASSERT_TRUE(find_png_chunk(png, png_len, "IHDR", &chunk, &chunk_len));
    TEST_ASSERT_EQUAL_UINT32(13U, (uint32_t)chunk_len);
    TEST_ASSERT_TRUE(find_png_chunk(png, png_len, "IDAT", &chunk, &chunk_len));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)TEST_PNG_IDAT_DATA_BYTES, (uint32_t)chunk_len);
    TEST_ASSERT_TRUE(find_png_chunk(png, png_len, "IEND", &chunk, &chunk_len));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)chunk_len);

    free(png);
    free(data_url);
}

TEST_CASE("png data url stores ink pixels as black bits", "[app_api]")
{
    image_framebuffer_t framebuffer;
    image_framebuffer_init(&framebuffer);
    image_framebuffer_apply_input(&framebuffer, &(image_input_state_t){.x = 0, .y = 0, .pen_down = true});

    char payload[4U * ((IMAGE_FRAMEBUFFER_BYTES + 2U) / 3U) + 160U];
    const size_t payload_len = image_framebuffer_build_socket_payload(&framebuffer, payload, sizeof(payload));
    TEST_ASSERT_GREATER_THAN(0U, payload_len);

    char *data_url = NULL;
    size_t data_url_len = 0U;
    TEST_ASSERT_EQUAL(ESP_OK, app_api_build_frame_png_data_url(payload, payload_len, &data_url, &data_url_len));
    TEST_ASSERT_NOT_NULL(data_url);
    TEST_ASSERT_GREATER_THAN(strlen(TEST_PNG_DATA_URL_PREFIX), data_url_len);

    uint8_t *png = NULL;
    size_t png_len = 0U;
    TEST_ASSERT_TRUE(decode_png_data_url(data_url, &png, &png_len));

    const uint8_t *idat = NULL;
    size_t idat_len = 0U;
    TEST_ASSERT_TRUE(find_png_chunk(png, png_len, "IDAT", &idat, &idat_len));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)TEST_PNG_IDAT_DATA_BYTES, (uint32_t)idat_len);
    TEST_ASSERT_EQUAL(0x78, idat[0]);
    TEST_ASSERT_EQUAL(0x01, idat[1]);
    TEST_ASSERT_EQUAL(0x01, idat[2]);

    const uint8_t *raw = idat + 7U;
    TEST_ASSERT_EQUAL(0, raw[0]);
    TEST_ASSERT_EQUAL(0x7f, raw[1]);
    for (size_t i = 2U; i <= TEST_PNG_ROW_BYTES; ++i) {
        TEST_ASSERT_EQUAL(0xff, raw[i]);
    }
    TEST_ASSERT_EQUAL(0, raw[TEST_PNG_SCANLINE_BYTES]);
    TEST_ASSERT_EQUAL(0xff, raw[TEST_PNG_SCANLINE_BYTES + 1U]);

    free(png);
    free(data_url);
}

TEST_CASE("input parser accepts prompt request field with newline", "[image_framebuffer]")
{
    image_input_state_t state = {0};

    TEST_ASSERT_TRUE(image_framebuffer_parse_input_packet("$S,64,65,1,0,0,1\n", &state));
    TEST_ASSERT_EQUAL_UINT16(64, state.x);
    TEST_ASSERT_EQUAL_UINT16(65, state.y);
    TEST_ASSERT_TRUE(state.pen_down);
    TEST_ASSERT_FALSE(state.erase);
    TEST_ASSERT_FALSE(state.submit);
    TEST_ASSERT_TRUE(state.prompt_request);
}

TEST_CASE("input parser rejects invalid prompt request field", "[image_framebuffer]")
{
    image_input_state_t state = {0};

    TEST_ASSERT_FALSE(image_framebuffer_parse_input_packet("$S,64,65,1,0,0,2\n", &state));
}

TEST_CASE("local prompt debug mode cycles prompt words without HTTP", "[app_api]")
{
    char prompt_word[APP_API_PROMPT_WORD_BUFFER_SIZE] = "";

    TEST_ASSERT_EQUAL(ESP_OK,
                      app_api_fetch_and_publish_prompt(prompt_send_fake,
                                                       prompt_word,
                                                       sizeof(prompt_word)));
    TEST_ASSERT_TRUE(s_prompt_send_fake_called);
    TEST_ASSERT_EQUAL_STRING("square", prompt_word);
    TEST_ASSERT_NOT_NULL(strstr(s_prompt_send_fake_payload, "\"word\":\"square\""));

    s_prompt_send_fake_called = false;
    s_prompt_send_fake_payload[0] = '\0';

    TEST_ASSERT_EQUAL(ESP_OK,
                      app_api_fetch_and_publish_prompt(prompt_send_fake,
                                                       prompt_word,
                                                       sizeof(prompt_word)));
    TEST_ASSERT_TRUE(s_prompt_send_fake_called);
    TEST_ASSERT_EQUAL_STRING("triangle", prompt_word);
    TEST_ASSERT_NOT_NULL(strstr(s_prompt_send_fake_payload, "\"word\":\"triangle\""));
}

TEST_CASE("prompt fetch stays local when submit API is enabled", "[app_api]")
{
    char prompt_word[APP_API_PROMPT_WORD_BUFFER_SIZE] = "";

    app_api_set_openai_api_key("unit-test-key");
    app_api_set_http_post_for_test(submit_http_fake);
    app_api_set_local_debug_mode_for_test(false);

    TEST_ASSERT_EQUAL(ESP_OK,
                      app_api_fetch_and_publish_prompt(prompt_send_fake,
                                                       prompt_word,
                                                       sizeof(prompt_word)));
    TEST_ASSERT_TRUE(s_prompt_send_fake_called);
    TEST_ASSERT_FALSE(s_submit_http_fake_called);
    TEST_ASSERT_EQUAL_STRING("square", prompt_word);
    TEST_ASSERT_NOT_NULL(strstr(s_prompt_send_fake_payload, "\"word\":\"square\""));
}

TEST_CASE("local submit debug mode returns immediate configured result", "[app_api]")
{
    image_framebuffer_t framebuffer;
    image_framebuffer_init(&framebuffer);
    image_framebuffer_apply_input(&framebuffer, &(image_input_state_t){.x = 0, .y = 0, .pen_down = true});

    char payload[4U * ((IMAGE_FRAMEBUFFER_BYTES + 2U) / 3U) + 160U];
    const size_t payload_len = image_framebuffer_build_socket_payload(&framebuffer, payload, sizeof(payload));
    TEST_ASSERT_GREATER_THAN(0U, payload_len);

    app_ai_submit_result_t result = {0};
    bool submit_success = false;
    app_api_set_http_post_for_test(submit_http_fake);

    TEST_ASSERT_EQUAL(ESP_OK,
                      app_api_submit_drawing(payload,
                                             payload_len,
                                             "triangle",
                                             &result,
                                             &submit_success,
                                             true));
    TEST_ASSERT_FALSE(s_submit_http_fake_called);
    TEST_ASSERT_TRUE(submit_success);
    TEST_ASSERT_EQUAL_STRING("triangle", result.guess);
    TEST_ASSERT_EQUAL(10, result.confidence);
    TEST_ASSERT_TRUE(result.correct);
}

TEST_CASE("submit response parser accepts reduced JSON schema", "[app_api]")
{
    app_ai_submit_result_t result = {0};

    TEST_ASSERT_TRUE(app_api_parse_submit_response_text("{\"guess\":\"square\",\"confidence\":9,\"correct\":true}", &result));
    TEST_ASSERT_EQUAL_STRING("square", result.guess);
    TEST_ASSERT_EQUAL(9, result.confidence);
    TEST_ASSERT_TRUE(result.correct);
}

TEST_CASE("submit sends PNG data URL to API and parses response", "[app_api]")
{
    image_framebuffer_t framebuffer;
    image_framebuffer_init(&framebuffer);
    image_framebuffer_apply_input(&framebuffer, &(image_input_state_t){.x = 0, .y = 0, .pen_down = true});

    char payload[4U * ((IMAGE_FRAMEBUFFER_BYTES + 2U) / 3U) + 160U];
    const size_t payload_len = image_framebuffer_build_socket_payload(&framebuffer, payload, sizeof(payload));
    TEST_ASSERT_GREATER_THAN(0U, payload_len);

    app_ai_submit_result_t result = {0};
    bool submit_success = false;
    app_api_set_openai_api_key("unit-test-key");
    app_api_set_http_post_for_test(submit_http_fake);
    app_api_set_local_debug_mode_for_test(false);

    TEST_ASSERT_EQUAL(ESP_OK,
                      app_api_submit_drawing(payload,
                                             payload_len,
                                             "square",
                                             &result,
                                             &submit_success,
                                             false));
    TEST_ASSERT_TRUE(s_submit_http_fake_called);
    TEST_ASSERT_TRUE(s_submit_http_fake_url_valid);
    TEST_ASSERT_TRUE(s_submit_http_fake_request_valid);
    TEST_ASSERT_TRUE(submit_success);
    TEST_ASSERT_EQUAL_STRING("square", result.guess);
    TEST_ASSERT_EQUAL(9, result.confidence);
    TEST_ASSERT_TRUE(result.correct);
}
