/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "driver/uart.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "image_framebuffer.h"
#include "nvs_flash.h"
#include "secrets.h"

#define SOCKET_PAYLOAD_BUFFER_SIZE (4U * (((IMAGE_FRAMEBUFFER_CANVAS_WIDTH * IMAGE_FRAMEBUFFER_CANVAS_HEIGHT + 7U) / 8U + 2U) / 3U) + 160U)
#define UART_PORT_NUM UART_NUM_1
#define UART_PORT_LABEL "UART1"
#define UART_BAUD_RATE 9600
#define UART_TX_PIN 17
#define UART_RX_PIN 18
#define UART_RX_BUFFER_SIZE 512
#define UART_PACKET_BUFFER_SIZE 64
#define UART_READ_TIMEOUT_MS 100
#define VIEWER_STROKE_PAYLOAD_BUFFER_SIZE 160
#define APP_WS_ENDPOINT_URI "/ws"
#define APP_WS_SUBPROTOCOL "etchsketch.v1.json"
#define APP_WIFI_CONNECTED_BIT BIT0
#define APP_HTTP_RESPONSE_BUFFER_SIZE 8192
#define APP_AI_CONTENT_BUFFER_SIZE 768
#define APP_PROMPT_WORD_BUFFER_SIZE 64
#define APP_AI_GUESS_BUFFER_SIZE 64
#define APP_MCU_CONTROL_PACKET_BUFFER_SIZE 48

enum {
    APP_WS_MAX_OPEN_SOCKETS = 4,
    APP_WS_MAX_CLIENT_FDS = 4,
    APP_WS_SEND_WAIT_TIMEOUT_SEC = 5,
    APP_WS_RECV_WAIT_TIMEOUT_SEC = 5,
    APP_WS_DIAG_LOG_PERIOD_MS = 5000,
};

enum {
    APP_RTOS_PACKET_QUEUE_LENGTH = 16,
    APP_RTOS_UART_RX_TASK_STACK_SIZE = 4096,
    APP_RTOS_FRAMEBUFFER_TASK_STACK_SIZE = 12288,
};

typedef struct {
    const char *uart_rx_task_name;
    const char *framebuffer_task_name;
    uint32_t uart_queue_send_timeout_ms;
    uint32_t stack_watermark_log_period_ms;
    UBaseType_t uart_rx_task_priority;
    UBaseType_t framebuffer_task_priority;
} app_rtos_config_t;

static const app_rtos_config_t APP_RTOS_CONFIG = {
    .uart_rx_task_name = "uart_rx_task",
    .framebuffer_task_name = "framebuffer_task",
    .uart_queue_send_timeout_ms = 20U,
    .stack_watermark_log_period_ms = 5000U,
    .uart_rx_task_priority = 5,
    .framebuffer_task_priority = 4,
};

static const char *TAG = "image_framebuffer";
static image_framebuffer_t s_framebuffer;
static QueueHandle_t s_uart_packet_queue;
static EventGroupHandle_t s_wifi_event_group;
static httpd_handle_t s_ws_server;
#if CONFIG_HTTPD_WS_SUPPORT
static bool s_ws_diag_initialized;
static size_t s_ws_last_client_count;
static TickType_t s_ws_last_diag_log_tick;
#endif

typedef struct {
    char packet_line[UART_PACKET_BUFFER_SIZE];
} uart_packet_msg_t;

static StaticQueue_t s_uart_packet_queue_struct;
static uint8_t s_uart_packet_queue_storage[APP_RTOS_PACKET_QUEUE_LENGTH * sizeof(uart_packet_msg_t)];

static StaticTask_t s_uart_rx_task_tcb;
static StackType_t s_uart_rx_task_stack[APP_RTOS_UART_RX_TASK_STACK_SIZE];
static TaskHandle_t s_uart_rx_task_handle;

static StaticTask_t s_framebuffer_task_tcb;
static StackType_t s_framebuffer_task_stack[APP_RTOS_FRAMEBUFFER_TASK_STACK_SIZE];
static TaskHandle_t s_framebuffer_task_handle;

static esp_err_t app_socket_send_frame(const char *payload, size_t payload_len);

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} app_http_response_buffer_t;

typedef struct {
    char guess[APP_AI_GUESS_BUFFER_SIZE];
    int confidence;
    bool correct;
} app_ai_submit_result_t;

static char s_active_prompt_word[APP_PROMPT_WORD_BUFFER_SIZE] = "house";

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

static bool app_words_equal_ignore_case(const char *lhs, const char *rhs)
{
    if (lhs == NULL || rhs == NULL) {
        return false;
    }

    size_t li = 0U;
    size_t ri = 0U;
    while (lhs[li] != '\0' && rhs[ri] != '\0') {
        while (lhs[li] == ' ') {
            li++;
        }
        while (rhs[ri] == ' ') {
            ri++;
        }

        const char lc = (char)tolower((unsigned char)lhs[li]);
        const char rc = (char)tolower((unsigned char)rhs[ri]);
        if (lc != rc) {
            return false;
        }
        if (lhs[li] == '\0') {
            break;
        }

        li++;
        ri++;
    }

    while (lhs[li] == ' ') {
        li++;
    }
    while (rhs[ri] == ' ') {
        ri++;
    }

    return lhs[li] == '\0' && rhs[ri] == '\0';
}

static void app_parse_submit_result(const char *content,
                                    const char *target_word,
                                    app_ai_submit_result_t *out_result)
{
    if (out_result == NULL) {
        return;
    }

    out_result->guess[0] = '\0';
    out_result->confidence = 1;
    out_result->correct = false;

    if (content == NULL) {
        strncpy(out_result->guess, "unknown", sizeof(out_result->guess) - 1U);
        out_result->guess[sizeof(out_result->guess) - 1U] = '\0';
        return;
    }

    cJSON *content_json = cJSON_Parse(content);
    if (content_json != NULL) {
        const cJSON *guess = cJSON_GetObjectItemCaseSensitive(content_json, "guess");
        const cJSON *confidence = cJSON_GetObjectItemCaseSensitive(content_json, "confidence");
        const cJSON *correct = cJSON_GetObjectItemCaseSensitive(content_json, "correct");

        if (cJSON_IsString(guess) && guess->valuestring != NULL) {
            app_sanitize_token(guess->valuestring, out_result->guess, sizeof(out_result->guess));
        }

        if (cJSON_IsNumber(confidence)) {
            out_result->confidence = confidence->valueint;
        }

        if (cJSON_IsBool(correct)) {
            out_result->correct = cJSON_IsTrue(correct);
        }

        cJSON_Delete(content_json);
    } else {
        app_sanitize_token(content, out_result->guess, sizeof(out_result->guess));
    }

    if (out_result->guess[0] == '\0') {
        strncpy(out_result->guess, "unknown", sizeof(out_result->guess) - 1U);
        out_result->guess[sizeof(out_result->guess) - 1U] = '\0';
    }

    if (out_result->confidence < 1) {
        out_result->confidence = 1;
    }
    if (out_result->confidence > 10) {
        out_result->confidence = 10;
    }

    if (target_word != NULL && target_word[0] != '\0') {
        out_result->correct = out_result->correct || app_words_equal_ignore_case(out_result->guess, target_word);
    }
}

static esp_err_t app_send_mcu_command(const char *command, int value)
{
    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char packet[APP_MCU_CONTROL_PACKET_BUFFER_SIZE];
    const int written = snprintf(packet, sizeof(packet), "$C,%s,%d\n", command, value);
    if (written <= 0 || (size_t)written >= sizeof(packet)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const int tx_written = uart_write_bytes(UART_PORT_NUM, packet, written);
    if (tx_written != written) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void app_emit_submit_result(const app_ai_submit_result_t *result)
{
    if (result == NULL) {
        return;
    }

    char json[192];
    const int written = snprintf(json,
                                 sizeof(json),
                                 "{\"type\":\"result\",\"guess\":\"%s\",\"confidence\":%d,\"correct\":%s}",
                                 result->guess,
                                 result->confidence,
                                 result->correct ? "true" : "false");
    if (written > 0 && (size_t)written < sizeof(json)) {
        const esp_err_t ws_err = app_socket_send_frame(json, (size_t)written);
        if (ws_err != ESP_OK) {
            ESP_LOGW(TAG, "Result websocket send failed: %s", esp_err_to_name(ws_err));
        }
    }

    int rating = (result->confidence + 1) / 2;
    if (rating < 1) {
        rating = 1;
    }
    if (rating > 5) {
        rating = 5;
    }

    const esp_err_t rate_err = app_send_mcu_command("RATE", rating);
    if (rate_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send RATE command to MCU: %s", esp_err_to_name(rate_err));
    }

    const esp_err_t done_err = app_send_mcu_command("DONE", 1);
    if (done_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send DONE command to MCU: %s", esp_err_to_name(done_err));
    }
}

static esp_err_t app_fetch_and_publish_prompt(void)
{
    if (!app_is_api_key_configured()) {
        ESP_LOGW(TAG, "OPENAI_API_KEY is not configured; using default prompt '%s'", s_active_prompt_word);
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
                char prompt_word[APP_PROMPT_WORD_BUFFER_SIZE];
                if (app_try_extract_prompt_word(content, prompt_word, sizeof(prompt_word))) {
                    strncpy(s_active_prompt_word, prompt_word, sizeof(s_active_prompt_word) - 1U);
                    s_active_prompt_word[sizeof(s_active_prompt_word) - 1U] = '\0';
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
                                        s_active_prompt_word);
    if (prompt_written <= 0 || (size_t)prompt_written >= sizeof(prompt_json)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_err_t ws_err = app_socket_send_frame(prompt_json, (size_t)prompt_written);
    if (ws_err != ESP_OK) {
        ESP_LOGW(TAG, "Prompt websocket send failed: %s", esp_err_to_name(ws_err));
    }

    ESP_LOGI(TAG, "Active prompt word: %s", s_active_prompt_word);
    return ESP_OK;
}

static esp_err_t app_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(err));
            return err;
        }
        err = nvs_flash_init();
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static void app_wifi_event_handler(void *arg,
                                   esp_event_base_t event_base,
                                   int32_t event_id,
                                   void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying");
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, APP_WIFI_CONNECTED_BIT);
        }
    }
}

static esp_err_t app_wifi_init(void)
{
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }

    const wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &app_wifi_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WIFI_EVENT handler register failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &app_wifi_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IP_EVENT handler register failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
    xEventGroupWaitBits(s_wifi_event_group,
                        APP_WIFI_CONNECTED_BIT,
                        pdFALSE,
                        pdTRUE,
                        portMAX_DELAY);
    return ESP_OK;
}

#if CONFIG_HTTPD_WS_SUPPORT
static size_t app_ws_get_connected_client_count(void)
{
    if (s_ws_server == NULL) {
        return 0U;
    }

    size_t fds_count = APP_WS_MAX_CLIENT_FDS;
    int client_fds[APP_WS_MAX_CLIENT_FDS] = {0};
    if (httpd_get_client_list(s_ws_server, &fds_count, client_fds) != ESP_OK) {
        return 0U;
    }

    size_t ws_client_count = 0U;
    for (size_t i = 0; i < fds_count; ++i) {
        if (httpd_ws_get_fd_info(s_ws_server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            ws_client_count++;
        }
    }
    return ws_client_count;
}

static void app_ws_maybe_log_send_diagnostics(size_t ws_client_count,
                                              size_t sent_count,
                                              size_t payload_len,
                                              bool force)
{
    const TickType_t now = xTaskGetTickCount();
    const TickType_t period_ticks = pdMS_TO_TICKS(APP_WS_DIAG_LOG_PERIOD_MS);
    const bool periodic = s_ws_diag_initialized && ((now - s_ws_last_diag_log_tick) >= period_ticks);
    const bool count_changed = s_ws_diag_initialized && (ws_client_count != s_ws_last_client_count);

    if (!s_ws_diag_initialized || force || count_changed || periodic) {
        ESP_LOGI(TAG,
                 "WS diagnostics: clients=%u, sent=%u, payload=%u bytes",
                 (unsigned)ws_client_count,
                 (unsigned)sent_count,
                 (unsigned)payload_len);
        s_ws_last_client_count = ws_client_count;
        s_ws_last_diag_log_tick = now;
        s_ws_diag_initialized = true;
        return;
    }

    if (ws_client_count > sent_count) {
        ESP_LOGW(TAG,
                 "WS partial send: clients=%u, sent=%u, payload=%u bytes",
                 (unsigned)ws_client_count,
                 (unsigned)sent_count,
                 (unsigned)payload_len);
    }
}
#endif

#if CONFIG_HTTPD_WS_SUPPORT
static esp_err_t app_ws_handler(httpd_req_t *req)
{
    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (req->method == HTTP_GET) {
        app_ws_maybe_log_send_diagnostics(app_ws_get_connected_client_count(), 0U, 0U, true);
        return ESP_OK;
    }

    // Drain incoming frames to keep session state healthy even when no command protocol is defined yet.
    httpd_ws_frame_t rx_pkt = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &rx_pkt, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws recv header failed: %s", esp_err_to_name(err));
        return err;
    }

    if (rx_pkt.len == 0U) {
        return ESP_OK;
    }

    rx_pkt.payload = malloc(rx_pkt.len + 1U);
    if (rx_pkt.payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = httpd_ws_recv_frame(req, &rx_pkt, rx_pkt.len);
    free(rx_pkt.payload);
    return err;
}
#endif

static esp_err_t app_ws_server_start(void)
{
#if !CONFIG_HTTPD_WS_SUPPORT
    ESP_LOGW(TAG, "WebSocket support disabled in sdkconfig (CONFIG_HTTPD_WS_SUPPORT=0)");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_ws_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = APP_WS_MAX_OPEN_SOCKETS;
    config.lru_purge_enable = true;
    config.send_wait_timeout = APP_WS_SEND_WAIT_TIMEOUT_SEC;
    config.recv_wait_timeout = APP_WS_RECV_WAIT_TIMEOUT_SEC;

    esp_err_t err = httpd_start(&s_ws_server, &config);
    if (err != ESP_OK) {
        s_ws_server = NULL;
        ESP_LOGW(TAG, "WebSocket server start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t ws_uri = {
        .uri = APP_WS_ENDPOINT_URI,
        .method = HTTP_GET,
        .handler = app_ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = APP_WS_SUBPROTOCOL,
    };

    err = httpd_register_uri_handler(s_ws_server, &ws_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket URI registration failed: %s", esp_err_to_name(err));
        httpd_stop(s_ws_server);
        s_ws_server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "WebSocket server ready on ws://<esp-ip>%s", APP_WS_ENDPOINT_URI);
    return ESP_OK;
#endif
}

static void app_maybe_log_stack_watermarks(TickType_t *last_log_tick)
{
    if (last_log_tick == NULL) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    const TickType_t log_period_ticks = pdMS_TO_TICKS(APP_RTOS_CONFIG.stack_watermark_log_period_ms);
    if ((now - *last_log_tick) < log_period_ticks) {
        return;
    }

    const UBaseType_t uart_rx_stack_hwm =
        (s_uart_rx_task_handle != NULL) ? uxTaskGetStackHighWaterMark(s_uart_rx_task_handle) : 0U;
    const UBaseType_t framebuffer_stack_hwm =
        (s_framebuffer_task_handle != NULL) ? uxTaskGetStackHighWaterMark(s_framebuffer_task_handle) : 0U;

    ESP_LOGI(TAG,
             "Stack watermark bytes: %s=%u, %s=%u",
             APP_RTOS_CONFIG.uart_rx_task_name,
             (unsigned)uart_rx_stack_hwm,
             APP_RTOS_CONFIG.framebuffer_task_name,
             (unsigned)framebuffer_stack_hwm);

    *last_log_tick = now;
}

static esp_err_t app_uart_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(UART_PORT_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(UART_PORT_NUM, UART_RX_BUFFER_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "%s initialized at %d baud (tx=%d, rx=%d)",
             UART_PORT_LABEL,
             UART_BAUD_RATE,
             UART_TX_PIN,
             UART_RX_PIN);
    return ESP_OK;
}

static void app_uart_discard_until_newline(void)
{
    uint8_t byte = 0;
    while (uart_read_bytes(UART_PORT_NUM, &byte, 1, pdMS_TO_TICKS(10)) > 0) {
        if (byte == '\n') {
            return;
        }
    }
}

static bool app_uart_read_packet_line(char *out_packet, size_t out_packet_len)
{
    if (out_packet == NULL || out_packet_len < 2U) {
        return false;
    }

    size_t cursor = 0;
    while (true) {
        uint8_t byte = 0;
        const int read_count = uart_read_bytes(UART_PORT_NUM, &byte, 1, pdMS_TO_TICKS(UART_READ_TIMEOUT_MS));
        if (read_count <= 0) {
            return false;
        }

        if (byte == '\r') {
            continue;
        }

        if (byte == '\n') {
            if (cursor == 0U) {
                continue;
            }
            out_packet[cursor] = '\0';
            return true;
        }

        if (cursor + 1U >= out_packet_len) {
            ESP_LOGW(TAG, "UART packet exceeded %u bytes, dropping", (unsigned)(out_packet_len - 1U));
            app_uart_discard_until_newline();
            return false;
        }

        out_packet[cursor++] = (char)byte;
    }
}

static esp_err_t app_socket_send_frame(const char *payload, size_t payload_len)
{
    if (payload == NULL || payload_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ws_server == NULL) {
        const esp_err_t start_err = app_ws_server_start();
        if (start_err != ESP_OK) {
            return start_err;
        }
    }

#if !CONFIG_HTTPD_WS_SUPPORT
    (void)payload;
    (void)payload_len;
    return ESP_ERR_NOT_SUPPORTED;
#else
    size_t fds_count = APP_WS_MAX_CLIENT_FDS;
    int client_fds[APP_WS_MAX_CLIENT_FDS] = {0};
    esp_err_t err = httpd_get_client_list(s_ws_server, &fds_count, client_fds);
    if (err != ESP_OK) {
        return err;
    }

    size_t ws_client_count = 0U;
    size_t sent_count = 0U;
    for (size_t i = 0; i < fds_count; ++i) {
        const int fd = client_fds[i];
        if (httpd_ws_get_fd_info(s_ws_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            continue;
        }
        ws_client_count++;

        httpd_ws_frame_t tx_pkt = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)payload,
            .len = payload_len,
        };

        err = httpd_ws_send_frame_async(s_ws_server, fd, &tx_pkt);
        if (err == ESP_OK) {
            sent_count++;
        } else {
            ESP_LOGW(TAG,
                     "WS send failed on fd=%d: %s",
                     fd,
                     esp_err_to_name(err));
        }
    }

    app_ws_maybe_log_send_diagnostics(ws_client_count, sent_count, payload_len, false);

    // No connected websocket clients is not an error for producer pipeline.
    if (ws_client_count == 0U) {
        return ESP_OK;
    }
    return ESP_OK;
#endif
}

static esp_err_t app_api_submit_drawing(const char *payload, size_t payload_len)
{
    if (payload == NULL || payload_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    app_ai_submit_result_t result = {
        .guess = "unknown",
        .confidence = 1,
        .correct = false,
    };

    if (!app_is_api_key_configured()) {
        ESP_LOGW(TAG, "OPENAI_API_KEY is not configured; publishing fallback submit result");
        app_emit_submit_result(&result);
        return ESP_OK;
    }

    const size_t instruction_len = payload_len + 768U;
    char *instruction = malloc(instruction_len);
    if (instruction == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const int instruction_written = snprintf(
        instruction,
        instruction_len,
        "You are judging a 128x128 monochrome Etch-a-Sketch drawing. "
        "The target word is '%s'. "
        "The drawing payload is JSON in this message with fields: type,width,height,format,data, where data is base64 1bpp-msb framebuffer. "
        "Drawing payload: %.*s "
        "Return ONLY JSON exactly in this shape: {\"guess\":\"<word>\",\"confidence\":<1-10>,\"correct\":<true|false>}.",
        s_active_prompt_word,
        (int)payload_len,
        payload);
    if (instruction_written <= 0 || (size_t)instruction_written >= instruction_len) {
        free(instruction);
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    cJSON *message = cJSON_CreateObject();
    if (root == NULL || messages == NULL || message == NULL) {
        free(instruction);
        cJSON_Delete(root);
        cJSON_Delete(messages);
        cJSON_Delete(message);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "model", "gpt-4o-mini");
    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(message, "content", instruction);
    cJSON_AddItemToArray(messages, message);
    cJSON_AddItemToObject(root, "messages", messages);
    free(instruction);

    char *request_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (request_body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char http_response[APP_HTTP_RESPONSE_BUFFER_SIZE];
    const esp_err_t http_err = app_http_post_json("https://api.openai.com/v1/chat/completions",
                                                  request_body,
                                                  http_response,
                                                  sizeof(http_response));
    free(request_body);
    if (http_err != ESP_OK) {
        return http_err;
    }

    char content[APP_AI_CONTENT_BUFFER_SIZE];
    if (!app_extract_chat_content(http_response, content, sizeof(content))) {
        return ESP_FAIL;
    }

    app_parse_submit_result(content, s_active_prompt_word, &result);
    app_emit_submit_result(&result);
    ESP_LOGI(TAG,
             "Submit result: guess='%s', confidence=%d, correct=%u",
             result.guess,
             result.confidence,
             result.correct ? 1U : 0U);
    return ESP_OK;
}

static size_t app_build_viewer_stroke_payload(const image_input_state_t *state,
                                              char *out_json,
                                              size_t out_json_len)
{
    if (state == NULL || out_json == NULL || out_json_len == 0U) {
        return 0U;
    }

    const int written = snprintf(out_json,
                                 out_json_len,
                                 "{\"type\":\"stroke\",\"x\":%u,\"y\":%u,\"penDown\":%u,\"erase\":%u}",
                                 (unsigned)state->x,
                                 (unsigned)state->y,
                                 state->pen_down ? 1U : 0U,
                                 state->erase ? 1U : 0U);
    if (written <= 0 || (size_t)written >= out_json_len) {
        return 0U;
    }

    return (size_t)written;
}

static void app_send_viewer_stroke_update(const image_input_state_t *state)
{
    char viewer_payload[VIEWER_STROKE_PAYLOAD_BUFFER_SIZE];
    const size_t payload_len = app_build_viewer_stroke_payload(state, viewer_payload, sizeof(viewer_payload));
    if (payload_len == 0U) {
        ESP_LOGW(TAG, "Failed to build viewer stroke payload");
        return;
    }

    const esp_err_t err = app_socket_send_frame(viewer_payload, payload_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Viewer update send failed: %s", esp_err_to_name(err));
    }
}

static void app_submit_drawing_for_ai(char *socket_payload, size_t socket_payload_len)
{
    const size_t payload_len = image_framebuffer_build_socket_payload(&s_framebuffer, socket_payload, socket_payload_len);
    if (payload_len == 0U) {
        ESP_LOGE(TAG, "Failed to build AI submit payload");
        return;
    }

    const esp_err_t err = app_api_submit_drawing(socket_payload, payload_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AI submit failed: %s", esp_err_to_name(err));
        return;
    }

    // Pull the next round prompt after a successful submit.
    const esp_err_t prompt_err = app_fetch_and_publish_prompt();
    if (prompt_err != ESP_OK) {
        ESP_LOGW(TAG, "Prompt refresh after submit failed: %s", esp_err_to_name(prompt_err));
    }
}

static void app_process_packet_line(const char *packet_line, char *socket_payload, size_t socket_payload_len)
{
    image_input_state_t state;
    if (!image_framebuffer_parse_input_packet(packet_line, &state)) {
        ESP_LOGW(TAG, "Dropping malformed UART packet: %s", packet_line);
        return;
    }

    image_framebuffer_apply_input(&s_framebuffer, &state);

    // Viewer updates should be near real-time and independent of submit.
    app_send_viewer_stroke_update(&state);

    if (!state.submit) {
        return;
    }

    // Submit is reserved for the AI guess pipeline.
    app_submit_drawing_for_ai(socket_payload, socket_payload_len);
}

static void app_uart_rx_task(void *arg)
{
    (void)arg;

    char uart_packet[UART_PACKET_BUFFER_SIZE];
    uart_packet_msg_t msg;
    TickType_t last_stack_log_tick = xTaskGetTickCount();

    while (true) {
        app_maybe_log_stack_watermarks(&last_stack_log_tick);

        if (!app_uart_read_packet_line(uart_packet, sizeof(uart_packet))) {
            continue;
        }

        const size_t len = strnlen(uart_packet, sizeof(msg.packet_line) - 1U);
        memcpy(msg.packet_line, uart_packet, len);
        msg.packet_line[len] = '\0';

        const BaseType_t queued = xQueueSend(s_uart_packet_queue,
                                             &msg,
                                             pdMS_TO_TICKS(APP_RTOS_CONFIG.uart_queue_send_timeout_ms));
        if (queued != pdPASS) {
            ESP_LOGW(TAG, "UART packet queue full, dropping packet");
        }
    }
}

static void app_framebuffer_task(void *arg)
{
    (void)arg;

    char *socket_payload = malloc(SOCKET_PAYLOAD_BUFFER_SIZE);
    if (socket_payload == NULL) {
        ESP_LOGE(TAG, "Failed to allocate socket payload buffer");
        vTaskDelete(NULL);
    }

    uart_packet_msg_t msg;
    while (true) {
        const BaseType_t received = xQueueReceive(s_uart_packet_queue, &msg, portMAX_DELAY);
        if (received == pdPASS) {
            app_process_packet_line(msg.packet_line, socket_payload, SOCKET_PAYLOAD_BUFFER_SIZE);
        }
    }
}

void app_main(void)
{
    image_framebuffer_init(&s_framebuffer);

    const esp_err_t nvs_init_err = app_nvs_init();
    if (nvs_init_err != ESP_OK) {
        return;
    }

    const esp_err_t wifi_init_err = app_wifi_init();
    if (wifi_init_err != ESP_OK) {
        return;
    }

    const esp_err_t ws_start_err = app_ws_server_start();
    if (ws_start_err != ESP_OK) {
        ESP_LOGW(TAG, "Continuing without active WebSocket server");
    }

    const esp_err_t uart_init_err = app_uart_init();
    if (uart_init_err != ESP_OK) {
        return;
    }

    const esp_err_t prompt_init_err = app_fetch_and_publish_prompt();
    if (prompt_init_err != ESP_OK) {
        ESP_LOGW(TAG, "Initial prompt fetch/publish failed: %s", esp_err_to_name(prompt_init_err));
    }

    s_uart_packet_queue = xQueueCreateStatic(APP_RTOS_PACKET_QUEUE_LENGTH,
                                             sizeof(uart_packet_msg_t),
                                             s_uart_packet_queue_storage,
                                             &s_uart_packet_queue_struct);
    if (s_uart_packet_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create UART packet queue");
        return;
    }

    s_uart_rx_task_handle = xTaskCreateStatic(app_uart_rx_task,
                                              APP_RTOS_CONFIG.uart_rx_task_name,
                                              APP_RTOS_UART_RX_TASK_STACK_SIZE,
                                              NULL,
                                              APP_RTOS_CONFIG.uart_rx_task_priority,
                                              s_uart_rx_task_stack,
                                              &s_uart_rx_task_tcb);
    if (s_uart_rx_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create UART RX task");
        vQueueDelete(s_uart_packet_queue);
        s_uart_packet_queue = NULL;
        return;
    }

    s_framebuffer_task_handle = xTaskCreateStatic(app_framebuffer_task,
                                                  APP_RTOS_CONFIG.framebuffer_task_name,
                                                  APP_RTOS_FRAMEBUFFER_TASK_STACK_SIZE,
                                                  NULL,
                                                  APP_RTOS_CONFIG.framebuffer_task_priority,
                                                  s_framebuffer_task_stack,
                                                  &s_framebuffer_task_tcb);
    if (s_framebuffer_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create framebuffer task");
        vTaskDelete(s_uart_rx_task_handle);
        s_uart_rx_task_handle = NULL;
        vQueueDelete(s_uart_packet_queue);
        s_uart_packet_queue = NULL;
        return;
    }

    ESP_LOGI(TAG,
             "RTOS tasks started: %s and %s",
             APP_RTOS_CONFIG.uart_rx_task_name,
             APP_RTOS_CONFIG.framebuffer_task_name);
}
