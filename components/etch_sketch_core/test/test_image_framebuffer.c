#include "unity.h"

#include <stdlib.h>
#include <string.h>

#include "app_api.h"
#include "image_framebuffer.h"
#include "mbedtls/base64.h"

static bool s_submit_http_fake_called;
static bool s_submit_http_fake_url_valid;
static bool s_submit_http_fake_saw_png_image;
static bool s_prompt_send_fake_called;
static char s_prompt_send_fake_payload[128];

static bool request_body_contains_png_data_url(const char *body)
{
    const char *prefix = "\"image_url\":\"data:image/png;base64,";
    const char *encoded = (body != NULL) ? strstr(body, prefix) : NULL;
    if (encoded == NULL) {
        return false;
    }
    encoded += strlen(prefix);

    const char *encoded_end = strchr(encoded, '"');
    if (encoded_end == NULL || encoded_end == encoded) {
        return false;
    }

    const size_t encoded_len = (size_t)(encoded_end - encoded);
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

    const uint8_t png_signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    const bool is_png = (decode_ret == 0) &&
                        (decoded_len >= sizeof(png_signature)) &&
                        (memcmp(decoded, png_signature, sizeof(png_signature)) == 0);
    free(decoded);
    return is_png;
}

void setUp(void)
{
    s_submit_http_fake_called = false;
    s_submit_http_fake_url_valid = false;
    s_submit_http_fake_saw_png_image = false;
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
        "{\"output_text\":\"{\\\"reasoning\\\":\\\"mock accepted\\\",\\\"guess\\\":\\\"square\\\",\\\"confidence\\\":9,\\\"correct\\\":true}\"}";

    s_submit_http_fake_called = true;
    s_submit_http_fake_url_valid = (url != NULL) && (strcmp(url, "https://api.openai.com/v1/responses") == 0);
    s_submit_http_fake_saw_png_image =
        (body != NULL) &&
        (strstr(body, "\"type\":\"input_image\"") != NULL) &&
        request_body_contains_png_data_url(body);

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
    TEST_ASSERT_TRUE(s_submit_http_fake_saw_png_image);
    TEST_ASSERT_TRUE(submit_success);
    TEST_ASSERT_EQUAL_STRING("square", result.guess);
    TEST_ASSERT_EQUAL(9, result.confidence);
    TEST_ASSERT_TRUE(result.correct);
}
