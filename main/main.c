/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "app_api.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
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
#define UART_BAUD_RATE 115200
#define UART_TX_PIN 17
// On ESP32-S2 DevKit boards, GPIO18 is commonly wired to the onboard RGB LED.
// Keep UART RX on a dedicated pin to avoid contention with LED activity.
#define UART_RX_PIN 16
#define UART_RX_BUFFER_SIZE 2048
#define UART_PACKET_BUFFER_SIZE 64
#define UART_READ_TIMEOUT_MS 100
#define APP_WS_ENDPOINT_URI "/ws"
#define APP_WS_SUBPROTOCOL "etchsketch.v1.json"
#define APP_WIFI_CONNECTED_BIT BIT0
#define APP_MCU_CONTROL_PACKET_BUFFER_SIZE 48
#define APP_MCU_RESULT_COMMAND "RESULT"
#define APP_MCU_PROMPT_READY_COMMAND "PROMPT"
#define APP_MCU_ACK_COMMAND "ACK"
#define APP_VIEWER_PAYLOAD_BUFFER_SIZE (SOCKET_PAYLOAD_BUFFER_SIZE + 768U)
#define APP_VIEWER_DELTA_BUFFER_SIZE 1024U
#define APP_VIEWER_TARGET_BROADCAST (-1)
#define APP_VIEWER_DELTA_PERIOD_MS 16U
#define APP_PROMPT_READY_RETRY_MS 250U

enum {
    APP_WS_MAX_OPEN_SOCKETS = 4,
    APP_WS_MAX_CLIENT_FDS = 4,
    APP_WS_SEND_WAIT_TIMEOUT_SEC = 5,
    APP_WS_RECV_WAIT_TIMEOUT_SEC = 5,
    APP_WS_DIAG_LOG_PERIOD_MS = 5000,
};

enum {
    APP_RTOS_UART_PACKET_QUEUE_LENGTH = 64,
    APP_RTOS_FRAMEBUFFER_STATE_QUEUE_LENGTH = 64,
    APP_RTOS_VIEWER_EVENT_QUEUE_LENGTH = 4,
    APP_RTOS_API_REQUEST_QUEUE_LENGTH = 2,
    APP_RTOS_UART_RX_TASK_STACK_SIZE = 4096,
    APP_RTOS_SOCKET_DISPATCH_TASK_STACK_SIZE = 8192,
    APP_RTOS_FRAMEBUFFER_TASK_STACK_SIZE = 12288,
    APP_RTOS_VIEWER_TASK_STACK_SIZE = 4096,
    APP_RTOS_API_TASK_STACK_SIZE = 12288,
};

typedef struct {
    const char *uart_rx_task_name;
    const char *socket_dispatch_task_name;
    const char *framebuffer_task_name;
    const char *viewer_task_name;
    const char *api_task_name;
    uint32_t uart_queue_send_timeout_ms;
    uint32_t framebuffer_queue_send_timeout_ms;
    uint32_t stack_watermark_log_period_ms;
    UBaseType_t uart_rx_task_priority;
    UBaseType_t socket_dispatch_task_priority;
    UBaseType_t framebuffer_task_priority;
    UBaseType_t viewer_task_priority;
    UBaseType_t api_task_priority;
} app_rtos_config_t;

static const app_rtos_config_t APP_RTOS_CONFIG = {
    .uart_rx_task_name = "uart_rx_task",
    .socket_dispatch_task_name = "state_dispatch_task",
    .framebuffer_task_name = "framebuffer_task",
    .viewer_task_name = "viewer_task",
    .api_task_name = "api_task",
    .uart_queue_send_timeout_ms = 0U,
    .framebuffer_queue_send_timeout_ms = 0U,
    .stack_watermark_log_period_ms = 5000U,
    .uart_rx_task_priority = 5,
    .socket_dispatch_task_priority = 6,
    .framebuffer_task_priority = 4,
    .viewer_task_priority = 3,
    .api_task_priority = 3,
};

static const char *TAG = "image_framebuffer";
static image_framebuffer_t s_framebuffer;
static QueueHandle_t s_uart_packet_queue;
static QueueHandle_t s_framebuffer_state_queue;
static EventGroupHandle_t s_wifi_event_group;
static httpd_handle_t s_ws_server;
#if CONFIG_HTTPD_WS_SUPPORT
static bool s_ws_diag_initialized;
static size_t s_ws_last_client_count;
static TickType_t s_ws_last_diag_log_tick;
static TickType_t s_ws_last_failure_log_tick;
#endif

typedef struct {
    char packet_line[UART_PACKET_BUFFER_SIZE];
} uart_packet_msg_t;

typedef enum {
    FRAMEBUFFER_EVENT_APPLY_STATE = 0,
    FRAMEBUFFER_EVENT_SUBMIT = 1,
    FRAMEBUFFER_EVENT_PROMPT_REQUEST = 2,
    FRAMEBUFFER_EVENT_SNAPSHOT_REQUEST = 3,
    FRAMEBUFFER_EVENT_SUBMIT_RESULT = 4,
    FRAMEBUFFER_EVENT_PROMPT_READY = 5,
} framebuffer_event_type_t;

typedef struct {
    framebuffer_event_type_t type;
    image_input_state_t state;
    int target_fd;
    app_ai_submit_result_t result;
    char prompt_word[APP_API_PROMPT_WORD_BUFFER_SIZE];
} framebuffer_state_msg_t;

typedef enum {
    API_REQUEST_SUBMIT = 0,
    API_REQUEST_PROMPT = 1,
} api_request_type_t;

typedef struct {
    api_request_type_t type;
    size_t payload_len;
    char *payload;
    char prompt_word[APP_API_PROMPT_WORD_BUFFER_SIZE];
} api_request_msg_t;

typedef struct {
    int target_fd;
    size_t payload_len;
    char *payload;
} viewer_event_msg_t;

typedef enum {
    APP_PHASE_WAITING_FOR_PROMPT = 0,
    APP_PHASE_DRAWING = 1,
    APP_PHASE_GUESSING = 2,
    APP_PHASE_RESULT = 3,
} app_phase_t;

typedef struct {
    char json[APP_VIEWER_DELTA_BUFFER_SIZE];
    size_t len;
    bool has_events;
    TickType_t first_event_tick;
} app_viewer_delta_buffer_t;

static StaticQueue_t s_uart_packet_queue_struct;
static uint8_t s_uart_packet_queue_storage[APP_RTOS_UART_PACKET_QUEUE_LENGTH * sizeof(uart_packet_msg_t)];

static StaticQueue_t s_framebuffer_state_queue_struct;
static uint8_t s_framebuffer_state_queue_storage[APP_RTOS_FRAMEBUFFER_STATE_QUEUE_LENGTH * sizeof(framebuffer_state_msg_t)];

static QueueHandle_t s_viewer_event_queue;
static StaticQueue_t s_viewer_event_queue_struct;
static uint8_t s_viewer_event_queue_storage[APP_RTOS_VIEWER_EVENT_QUEUE_LENGTH * sizeof(viewer_event_msg_t)];

static QueueHandle_t s_api_request_queue;
static StaticQueue_t s_api_request_queue_struct;
static uint8_t s_api_request_queue_storage[APP_RTOS_API_REQUEST_QUEUE_LENGTH * sizeof(api_request_msg_t)];

static StaticTask_t s_uart_rx_task_tcb;
static StackType_t s_uart_rx_task_stack[APP_RTOS_UART_RX_TASK_STACK_SIZE];
static TaskHandle_t s_uart_rx_task_handle;

static StaticTask_t s_socket_dispatch_task_tcb;
static StackType_t s_socket_dispatch_task_stack[APP_RTOS_SOCKET_DISPATCH_TASK_STACK_SIZE];
static TaskHandle_t s_socket_dispatch_task_handle;

static StaticTask_t s_framebuffer_task_tcb;
static StackType_t s_framebuffer_task_stack[APP_RTOS_FRAMEBUFFER_TASK_STACK_SIZE];
static TaskHandle_t s_framebuffer_task_handle;

static TaskHandle_t s_viewer_task_handle;

static TaskHandle_t s_api_task_handle;

static uint32_t s_uart_line_count;
static uint32_t s_uart_queue_drop_count;
static uint32_t s_framebuffer_queue_drop_count;
static uint32_t s_viewer_queue_drop_count;
static uint32_t s_api_queue_drop_count;
static uint32_t s_uart_parse_ok_count;
static uint32_t s_uart_parse_fail_count;
static uint32_t s_viewer_seq;
// Temporary integration flag until teammate-owned API layer is connected.
static bool s_submit_stub_success_flag = true;

static void app_enqueue_snapshot_request_event(int target_fd);
static void app_process_packet_line_fast_path(const char *packet_line);
static bool app_enqueue_submit_event(void);
static bool app_enqueue_prompt_request_event(void);

static char s_active_prompt_word[APP_API_PROMPT_WORD_BUFFER_SIZE] = "square";
static bool s_has_prompt;
static app_phase_t s_app_phase = APP_PHASE_WAITING_FOR_PROMPT;
static app_ai_submit_result_t s_last_result;
static bool s_has_result;
static bool s_prompt_request_in_flight;

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

static esp_err_t app_send_mcu_submit_result(bool submit_success)
{
    return app_send_mcu_command(APP_MCU_RESULT_COMMAND, submit_success ? 1 : 0);
}

static esp_err_t app_send_mcu_prompt_ready(void)
{
    return app_send_mcu_command(APP_MCU_PROMPT_READY_COMMAND, 1);
}

static esp_err_t app_send_mcu_ack(void)
{
    return app_send_mcu_command(APP_MCU_ACK_COMMAND, 0);
}

static const char *app_phase_to_json(app_phase_t phase)
{
    switch (phase) {
    case APP_PHASE_WAITING_FOR_PROMPT:
        return "waiting_for_prompt";
    case APP_PHASE_DRAWING:
        return "drawing";
    case APP_PHASE_GUESSING:
        return "guessing";
    case APP_PHASE_RESULT:
        return "result";
    default:
        return "waiting_for_prompt";
    }
}

static size_t app_json_escape_string(const char *input, char *out, size_t out_len)
{
    if (input == NULL || out == NULL || out_len == 0U) {
        return 0U;
    }

    size_t out_idx = 0U;
    for (size_t in_idx = 0U; input[in_idx] != '\0'; ++in_idx) {
        const char ch = input[in_idx];
        const char *escape = NULL;
        if (ch == '"' || ch == '\\') {
            escape = (ch == '"') ? "\\\"" : "\\\\";
        }

        if (escape != NULL) {
            if (out_idx + 2U >= out_len) {
                return 0U;
            }
            out[out_idx++] = escape[0];
            out[out_idx++] = escape[1];
            continue;
        }

        if ((unsigned char)ch < 0x20U) {
            if (out_idx + 6U >= out_len) {
                return 0U;
            }
            static const char hex[] = "0123456789abcdef";
            out[out_idx++] = '\\';
            out[out_idx++] = 'u';
            out[out_idx++] = '0';
            out[out_idx++] = '0';
            out[out_idx++] = hex[((unsigned char)ch >> 4U) & 0x0FU];
            out[out_idx++] = hex[(unsigned char)ch & 0x0FU];
            continue;
        }

        if (out_idx + 1U >= out_len) {
            return 0U;
        }
        out[out_idx++] = ch;
    }

    out[out_idx] = '\0';
    return out_idx;
}

static bool app_enqueue_viewer_payload(int target_fd, const char *payload, size_t payload_len)
{
    if (payload == NULL || payload_len == 0U || payload_len >= APP_VIEWER_PAYLOAD_BUFFER_SIZE ||
        s_viewer_event_queue == NULL) {
        return false;
    }

    viewer_event_msg_t msg = {
        .target_fd = target_fd,
        .payload_len = payload_len,
    };
    msg.payload = malloc(payload_len + 1U);
    if (msg.payload == NULL) {
        s_viewer_queue_drop_count++;
        return false;
    }
    memcpy(msg.payload, payload, payload_len);
    msg.payload[payload_len] = '\0';

    const BaseType_t queued = xQueueSend(s_viewer_event_queue, &msg, 0);
    if (queued != pdPASS) {
        free(msg.payload);
        s_viewer_queue_drop_count++;
        return false;
    }
    return true;
}

static bool app_enqueue_legacy_result_payload(const app_ai_submit_result_t *result)
{
    if (result == NULL) {
        return false;
    }

    char json[192];
    char escaped_guess[APP_API_GUESS_BUFFER_SIZE * 6U];
    if (app_json_escape_string(result->guess, escaped_guess, sizeof(escaped_guess)) == 0U) {
        return false;
    }

    const int written = snprintf(json,
                                 sizeof(json),
                                 "{\"type\":\"result\",\"guess\":\"%s\",\"confidence\":%d,\"correct\":%s}",
                                 escaped_guess,
                                 result->confidence,
                                 result->correct ? "true" : "false");
    return (written > 0 && (size_t)written < sizeof(json)) ?
        app_enqueue_viewer_payload(APP_VIEWER_TARGET_BROADCAST, json, (size_t)written) : false;
}

static bool app_enqueue_legacy_prompt_payload(const char *prompt_word)
{
    if (prompt_word == NULL) {
        return false;
    }

    char escaped_prompt[APP_API_PROMPT_WORD_BUFFER_SIZE * 6U];
    if (app_json_escape_string(prompt_word, escaped_prompt, sizeof(escaped_prompt)) == 0U) {
        return false;
    }

    char json[160];
    const int written = snprintf(json,
                                 sizeof(json),
                                 "{\"type\":\"prompt\",\"word\":\"%s\"}",
                                 escaped_prompt);
    return (written > 0 && (size_t)written < sizeof(json)) ?
        app_enqueue_viewer_payload(APP_VIEWER_TARGET_BROADCAST, json, (size_t)written) : false;
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

    if (ws_client_count > sent_count && periodic) {
        ESP_LOGW(TAG,
                 "WS partial send: clients=%u, sent=%u, payload=%u bytes",
                 (unsigned)ws_client_count,
                 (unsigned)sent_count,
                 (unsigned)payload_len);
    }
}
#endif

#if CONFIG_HTTPD_WS_SUPPORT
static bool app_ws_handle_text_command(const char *payload, int target_fd)
{
    if (payload == NULL || payload[0] == '\0') {
        return false;
    }

    if (strncmp(payload, "$S,", 3U) == 0) {
        app_process_packet_line_fast_path(payload);
        return true;
    }

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        return false;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    bool handled = false;
    if (cJSON_IsString(type) && type->valuestring != NULL) {
        if (strcmp(type->valuestring, "resync") == 0) {
            app_enqueue_snapshot_request_event(target_fd);
            handled = true;
        } else if (strcmp(type->valuestring, "prompt_request") == 0) {
            app_enqueue_prompt_request_event();
            handled = true;
        } else if (strcmp(type->valuestring, "api_test") == 0) {
            app_enqueue_submit_event();
            handled = true;
        }
    }

    cJSON_Delete(root);
    return handled;
}

static esp_err_t app_ws_handler(httpd_req_t *req)
{
    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (req->method == HTTP_GET) {
        app_enqueue_snapshot_request_event(httpd_req_to_sockfd(req));
        app_ws_maybe_log_send_diagnostics(app_ws_get_connected_client_count(), 0U, 0U, true);
        return ESP_OK;
    }

    // Accept browser-side test commands while preserving the UART packet parser as the authority.
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
    if (err == ESP_OK && rx_pkt.type == HTTPD_WS_TYPE_TEXT) {
        rx_pkt.payload[rx_pkt.len] = '\0';
        if (!app_ws_handle_text_command((const char *)rx_pkt.payload, httpd_req_to_sockfd(req))) {
            ESP_LOGI(TAG, "Ignoring WebSocket text command: %s", (const char *)rx_pkt.payload);
        }
    }
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
    const UBaseType_t socket_dispatch_stack_hwm =
        (s_socket_dispatch_task_handle != NULL) ? uxTaskGetStackHighWaterMark(s_socket_dispatch_task_handle) : 0U;
    const UBaseType_t framebuffer_stack_hwm =
        (s_framebuffer_task_handle != NULL) ? uxTaskGetStackHighWaterMark(s_framebuffer_task_handle) : 0U;
    const UBaseType_t viewer_stack_hwm =
        (s_viewer_task_handle != NULL) ? uxTaskGetStackHighWaterMark(s_viewer_task_handle) : 0U;
    const UBaseType_t api_stack_hwm =
        (s_api_task_handle != NULL) ? uxTaskGetStackHighWaterMark(s_api_task_handle) : 0U;

    ESP_LOGI(TAG,
             "Stack watermark bytes: %s=%u, %s=%u, %s=%u, %s=%u, %s=%u",
             APP_RTOS_CONFIG.uart_rx_task_name,
             (unsigned)uart_rx_stack_hwm,
             APP_RTOS_CONFIG.socket_dispatch_task_name,
             (unsigned)socket_dispatch_stack_hwm,
             APP_RTOS_CONFIG.framebuffer_task_name,
             (unsigned)framebuffer_stack_hwm,
             APP_RTOS_CONFIG.viewer_task_name,
             (unsigned)viewer_stack_hwm,
             APP_RTOS_CONFIG.api_task_name,
             (unsigned)api_stack_hwm);

    *last_log_tick = now;
}

static bool app_log_period_elapsed(TickType_t *last_log_tick, uint32_t period_ms)
{
    if (last_log_tick == NULL) {
        return true;
    }

    const TickType_t now = xTaskGetTickCount();
    const TickType_t period_ticks = pdMS_TO_TICKS(period_ms);
    if (*last_log_tick != 0 && (now - *last_log_tick) < period_ticks) {
        return false;
    }

    *last_log_tick = now;
    return true;
}

static void app_log_uart_input_if_changed(const image_input_state_t *state)
{
    static image_input_state_t last_state;
    static bool has_last_state;
    static TickType_t last_input_log_tick;

    if (state == NULL) {
        return;
    }

    const bool changed = !has_last_state ||
                         state->x != last_state.x ||
                         state->y != last_state.y ||
                         state->pen_down != last_state.pen_down ||
                         state->erase != last_state.erase ||
                         state->submit != last_state.submit ||
                         state->prompt_request != last_state.prompt_request;
    if (!changed) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    const TickType_t log_period_ticks = pdMS_TO_TICKS(100);
    if (last_input_log_tick != 0 && (now - last_input_log_tick) < log_period_ticks) {
        last_state = *state;
        has_last_state = true;
        return;
    }
    last_input_log_tick = now;

    ESP_LOGI(TAG,
             "UART input: x=%u y=%u penDown=%u erase=%u submit=%u promptReq=%u",
             (unsigned)state->x,
             (unsigned)state->y,
             state->pen_down ? 1U : 0U,
             state->erase ? 1U : 0U,
             state->submit ? 1U : 0U,
             state->prompt_request ? 1U : 0U);

    last_state = *state;
    has_last_state = true;
}

static void app_maybe_log_uart_pipeline_stats(TickType_t *last_log_tick)
{
    if (last_log_tick == NULL) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    const TickType_t log_period_ticks = pdMS_TO_TICKS(2000);
    if ((now - *last_log_tick) < log_period_ticks) {
        return;
    }

    const UBaseType_t uart_queue_depth =
        (s_uart_packet_queue != NULL) ? uxQueueMessagesWaiting(s_uart_packet_queue) : 0U;
    const UBaseType_t framebuffer_queue_depth =
        (s_framebuffer_state_queue != NULL) ? uxQueueMessagesWaiting(s_framebuffer_state_queue) : 0U;
    const UBaseType_t viewer_queue_depth =
        (s_viewer_event_queue != NULL) ? uxQueueMessagesWaiting(s_viewer_event_queue) : 0U;
    const UBaseType_t api_queue_depth =
        (s_api_request_queue != NULL) ? uxQueueMessagesWaiting(s_api_request_queue) : 0U;

    ESP_LOGI(TAG,
             "UART pipeline: lines=%u parsed=%u malformed=%u uartDrops=%u uartDepth=%u fbDrops=%u fbDepth=%u viewerDrops=%u viewerDepth=%u apiDrops=%u apiDepth=%u phase=%s seq=%u",
             (unsigned)s_uart_line_count,
             (unsigned)s_uart_parse_ok_count,
             (unsigned)s_uart_parse_fail_count,
             (unsigned)s_uart_queue_drop_count,
             (unsigned)uart_queue_depth,
             (unsigned)s_framebuffer_queue_drop_count,
             (unsigned)framebuffer_queue_depth,
             (unsigned)s_viewer_queue_drop_count,
             (unsigned)viewer_queue_depth,
             (unsigned)s_api_queue_drop_count,
             (unsigned)api_queue_depth,
             app_phase_to_json(s_app_phase),
             (unsigned)s_viewer_seq);

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

static esp_err_t app_socket_send_frame_to_target(const char *payload, size_t payload_len, int target_fd)
{
    if (payload == NULL || payload_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

#if !CONFIG_HTTPD_WS_SUPPORT
    (void)target_fd;
    return ESP_OK;
#else
    if (s_ws_server == NULL) {
        const esp_err_t start_err = app_ws_server_start();
        if (start_err != ESP_OK) {
            return start_err;
        }
    }

    size_t fds_count = APP_WS_MAX_CLIENT_FDS;
    int client_fds[APP_WS_MAX_CLIENT_FDS] = {0};
    esp_err_t err = ESP_OK;
    if (target_fd == APP_VIEWER_TARGET_BROADCAST) {
        err = httpd_get_client_list(s_ws_server, &fds_count, client_fds);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        client_fds[0] = target_fd;
        fds_count = 1U;
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

        errno = 0;
        err = httpd_ws_send_frame_async(s_ws_server, fd, &tx_pkt);
        if (err == ESP_OK) {
            sent_count++;
        } else {
            const int socket_errno = errno;
            const bool temporary_backpressure = (socket_errno == EAGAIN || socket_errno == EWOULDBLOCK);
            if (app_log_period_elapsed(&s_ws_last_failure_log_tick, 1000U)) {
                ESP_LOGW(TAG,
                         "WS send failed on fd=%d: %s errno=%d (%s)%s",
                         fd,
                         esp_err_to_name(err),
                         socket_errno,
                         strerror(socket_errno),
                         temporary_backpressure ? "; dropping viewer payload" : "; closing session");
            }
            if (!temporary_backpressure) {
                httpd_sess_trigger_close(s_ws_server, fd);
            }
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

static bool app_viewer_delta_append_raw(app_viewer_delta_buffer_t *delta, const char *event_json)
{
    if (delta == NULL || event_json == NULL) {
        return false;
    }

    const size_t event_len = strlen(event_json);
    const size_t comma_len = delta->has_events ? 1U : 0U;
    if (delta->len + comma_len + event_len >= sizeof(delta->json)) {
        return false;
    }

    if (!delta->has_events) {
        delta->first_event_tick = xTaskGetTickCount();
    } else {
        delta->json[delta->len++] = ',';
    }

    memcpy(delta->json + delta->len, event_json, event_len);
    delta->len += event_len;
    delta->json[delta->len] = '\0';
    delta->has_events = true;
    return true;
}

static void app_viewer_delta_reset(app_viewer_delta_buffer_t *delta)
{
    if (delta == NULL) {
        return;
    }

    delta->json[0] = '\0';
    delta->len = 0U;
    delta->has_events = false;
    delta->first_event_tick = 0;
}

static void app_viewer_delta_flush(app_viewer_delta_buffer_t *delta);

static bool app_viewer_delta_append_with_flush(app_viewer_delta_buffer_t *delta, const char *event_json)
{
    if (app_viewer_delta_append_raw(delta, event_json)) {
        return true;
    }

    app_viewer_delta_flush(delta);
    return app_viewer_delta_append_raw(delta, event_json);
}

static void app_viewer_delta_append_clear(app_viewer_delta_buffer_t *delta)
{
    if (!app_viewer_delta_append_with_flush(delta, "{\"kind\":\"clear\"}")) {
        ESP_LOGW(TAG, "Viewer delta clear overflow; dropping clear event");
    }
}

static void app_viewer_delta_append_cursor(app_viewer_delta_buffer_t *delta, const image_input_state_t *state)
{
    if (state == NULL) {
        return;
    }

    char event_json[96];
    const int written = snprintf(event_json,
                                 sizeof(event_json),
                                 "{\"kind\":\"cursor\",\"x\":%u,\"y\":%u,\"penDown\":%u}",
                                 (unsigned)state->x,
                                 (unsigned)state->y,
                                 state->pen_down ? 1U : 0U);
    if (written <= 0 || (size_t)written >= sizeof(event_json) ||
        !app_viewer_delta_append_with_flush(delta, event_json)) {
        ESP_LOGW(TAG, "Viewer delta cursor overflow; dropping cursor event");
    }
}

static void app_viewer_delta_append_line(app_viewer_delta_buffer_t *delta,
                                         uint16_t x0,
                                         uint16_t y0,
                                         uint16_t x1,
                                         uint16_t y1)
{
    char event_json[112];
    const int written = snprintf(event_json,
                                 sizeof(event_json),
                                 "{\"kind\":\"line\",\"x0\":%u,\"y0\":%u,\"x1\":%u,\"y1\":%u}",
                                 (unsigned)x0,
                                 (unsigned)y0,
                                 (unsigned)x1,
                                 (unsigned)y1);
    if (written <= 0 || (size_t)written >= sizeof(event_json) ||
        !app_viewer_delta_append_with_flush(delta, event_json)) {
        ESP_LOGW(TAG, "Viewer delta line overflow; dropping line event");
    }
}

static void app_viewer_delta_flush(app_viewer_delta_buffer_t *delta)
{
    if (delta == NULL || !delta->has_events) {
        return;
    }

    char payload[APP_VIEWER_DELTA_BUFFER_SIZE + 96U];
    const uint32_t base_seq = s_viewer_seq;
    const uint32_t next_seq = base_seq + 1U;
    const int written = snprintf(payload,
                                 sizeof(payload),
                                 "{\"type\":\"delta\",\"baseSeq\":%u,\"seq\":%u,\"events\":[%s]}",
                                 (unsigned)base_seq,
                                 (unsigned)next_seq,
                                 delta->json);
    if (written > 0 && (size_t)written < sizeof(payload)) {
        s_viewer_seq = next_seq;
        app_enqueue_viewer_payload(APP_VIEWER_TARGET_BROADCAST, payload, (size_t)written);
    }

    app_viewer_delta_reset(delta);
}

static void app_viewer_delta_flush_if_due(app_viewer_delta_buffer_t *delta)
{
    if (delta == NULL || !delta->has_events) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if ((now - delta->first_event_tick) >= pdMS_TO_TICKS(APP_VIEWER_DELTA_PERIOD_MS)) {
        app_viewer_delta_flush(delta);
    }
}

static size_t app_build_snapshot_payload(char *out_json, size_t out_json_len)
{
    if (out_json == NULL || out_json_len == 0U) {
        return 0U;
    }

    char frame_base64[IMAGE_FRAMEBUFFER_BASE64_BUFFER_SIZE];
    if (image_framebuffer_build_base64_data(&s_framebuffer, frame_base64, sizeof(frame_base64)) == 0U) {
        return 0U;
    }

    char escaped_prompt[APP_API_PROMPT_WORD_BUFFER_SIZE * 6U];
    const char *prompt_source = s_has_prompt ? s_active_prompt_word : "";
    if (app_json_escape_string(prompt_source, escaped_prompt, sizeof(escaped_prompt)) == 0U && prompt_source[0] != '\0') {
        return 0U;
    }

    const image_framebuffer_status_t *status = image_framebuffer_get_status(&s_framebuffer);
    const bool cursor_valid = (status != NULL) && status->has_cursor;
    size_t offset = 0U;
    int written = snprintf(out_json,
                           out_json_len,
                           "{\"type\":\"snapshot\",\"seq\":%u,\"phase\":\"%s\",\"hasPrompt\":%s,"
                           "\"prompt\":\"%s\",\"cursor\":{\"valid\":%s,\"x\":%u,\"y\":%u,\"penDown\":%s},"
                           "\"frame\":{\"width\":%u,\"height\":%u,\"format\":\"1bpp-msb\",\"data\":\"%s\"},"
                           "\"hasResult\":%s",
                           (unsigned)s_viewer_seq,
                           app_phase_to_json(s_app_phase),
                           s_has_prompt ? "true" : "false",
                           escaped_prompt,
                           cursor_valid ? "true" : "false",
                           (unsigned)(cursor_valid ? status->cursor_x : 0U),
                           (unsigned)(cursor_valid ? status->cursor_y : 0U),
                           (cursor_valid && status->pen_down) ? "true" : "false",
                           (unsigned)IMAGE_FRAMEBUFFER_CANVAS_WIDTH,
                           (unsigned)IMAGE_FRAMEBUFFER_CANVAS_HEIGHT,
                           frame_base64,
                           s_has_result ? "true" : "false");
    if (written <= 0 || (size_t)written >= out_json_len) {
        return 0U;
    }
    offset = (size_t)written;

    if (s_has_result) {
        char escaped_guess[APP_API_GUESS_BUFFER_SIZE * 6U];
        if (app_json_escape_string(s_last_result.guess, escaped_guess, sizeof(escaped_guess)) == 0U) {
            return 0U;
        }
        written = snprintf(out_json + offset,
                           out_json_len - offset,
                           ",\"result\":{\"guess\":\"%s\",\"confidence\":%d,\"correct\":%s}",
                           escaped_guess,
                           s_last_result.confidence,
                           s_last_result.correct ? "true" : "false");
        if (written <= 0 || (size_t)written >= out_json_len - offset) {
            return 0U;
        }
        offset += (size_t)written;
    }

    if (offset + 2U > out_json_len) {
        return 0U;
    }
    out_json[offset++] = '}';
    out_json[offset] = '\0';
    return offset;
}

static void app_enqueue_snapshot(int target_fd)
{
    char payload[APP_VIEWER_PAYLOAD_BUFFER_SIZE];
    const size_t payload_len = app_build_snapshot_payload(payload, sizeof(payload));
    if (payload_len == 0U) {
        ESP_LOGW(TAG, "Failed to build viewer snapshot payload");
        return;
    }
    app_enqueue_viewer_payload(target_fd, payload, payload_len);
}

static bool app_enqueue_framebuffer_msg(const framebuffer_state_msg_t *msg, TickType_t timeout_ticks)
{
    if (msg == NULL || s_framebuffer_state_queue == NULL) {
        return false;
    }

    const BaseType_t queued = xQueueSend(s_framebuffer_state_queue,
                                         msg,
                                         timeout_ticks);
    if (queued != pdPASS) {
        s_framebuffer_queue_drop_count++;
        return false;
    }
    return true;
}

static void app_enqueue_framebuffer_state(const image_input_state_t *state)
{
    if (state == NULL) {
        return;
    }

    framebuffer_state_msg_t msg = {
        .type = FRAMEBUFFER_EVENT_APPLY_STATE,
        .state = *state,
        .target_fd = APP_VIEWER_TARGET_BROADCAST,
    };

    app_enqueue_framebuffer_msg(&msg, pdMS_TO_TICKS(APP_RTOS_CONFIG.framebuffer_queue_send_timeout_ms));
}

static bool app_enqueue_submit_event(void)
{
    framebuffer_state_msg_t msg = {
        .type = FRAMEBUFFER_EVENT_SUBMIT,
        .target_fd = APP_VIEWER_TARGET_BROADCAST,
    };
    return app_enqueue_framebuffer_msg(&msg, pdMS_TO_TICKS(20U));
}

static bool app_enqueue_prompt_request_event(void)
{
    framebuffer_state_msg_t msg = {
        .type = FRAMEBUFFER_EVENT_PROMPT_REQUEST,
        .target_fd = APP_VIEWER_TARGET_BROADCAST,
    };
    return app_enqueue_framebuffer_msg(&msg, pdMS_TO_TICKS(20U));
}

static void app_enqueue_snapshot_request_event(int target_fd)
{
    framebuffer_state_msg_t msg = {
        .type = FRAMEBUFFER_EVENT_SNAPSHOT_REQUEST,
        .target_fd = target_fd,
    };
    app_enqueue_framebuffer_msg(&msg, pdMS_TO_TICKS(20U));
}

static bool app_enqueue_api_request(const api_request_msg_t *msg)
{
    if (msg == NULL || s_api_request_queue == NULL) {
        return false;
    }

    const BaseType_t queued = xQueueSend(s_api_request_queue, msg, pdMS_TO_TICKS(20U));
    if (queued != pdPASS) {
        s_api_queue_drop_count++;
        return false;
    }
    return true;
}

static void app_process_packet_line_fast_path(const char *packet_line)
{
    static TickType_t last_malformed_log_tick;
    static bool s_submit_level_high;
    static bool s_prompt_request_level_high;

    image_input_state_t state;
    if (!image_framebuffer_parse_input_packet(packet_line, &state)) {
        s_uart_parse_fail_count++;
        if (app_log_period_elapsed(&last_malformed_log_tick, 1000U)) {
            ESP_LOGW(TAG, "Dropping malformed UART packet: %s", packet_line);
        }
        return;
    }

    s_uart_parse_ok_count++;

    app_log_uart_input_if_changed(&state);

    const bool submit_rising_edge = state.submit && !s_submit_level_high;
    const bool prompt_request_rising_edge = state.prompt_request && !s_prompt_request_level_high;

    // Authoritative path: framebuffer state owns what the viewer can restore.
    app_enqueue_framebuffer_state(&state);

    // Command path: enqueue explicit submit event on rising edge only.
    if (submit_rising_edge) {
        s_submit_level_high = app_enqueue_submit_event();
    } else if (!state.submit) {
        s_submit_level_high = false;
    }

    if (prompt_request_rising_edge) {
        s_prompt_request_level_high = app_enqueue_prompt_request_event();
    } else if (!state.prompt_request) {
        s_prompt_request_level_high = false;
    }
}

static void app_process_framebuffer_state(const image_input_state_t *state)
{
    if (state == NULL) {
        return;
    }

    if (s_app_phase != APP_PHASE_WAITING_FOR_PROMPT && s_app_phase != APP_PHASE_DRAWING) {
        return;
    }

    image_framebuffer_apply_input(&s_framebuffer, state);
}

static void app_append_viewer_delta_for_state(app_viewer_delta_buffer_t *delta,
                                              const image_framebuffer_status_t *previous_status,
                                              const image_input_state_t *state)
{
    if (delta == NULL || previous_status == NULL || state == NULL) {
        return;
    }

    if (state->erase) {
        app_viewer_delta_append_clear(delta);
    }

    if (state->pen_down) {
        if (previous_status->has_cursor && previous_status->pen_down && !state->erase) {
            app_viewer_delta_append_line(delta,
                                         previous_status->cursor_x,
                                         previous_status->cursor_y,
                                         state->x,
                                         state->y);
        } else {
            app_viewer_delta_append_line(delta, state->x, state->y, state->x, state->y);
        }
    }

    app_viewer_delta_append_cursor(delta, state);
}

static void app_maybe_resend_prompt_ready(const image_input_state_t *state)
{
    static TickType_t last_prompt_retry_tick;
    static bool has_prompt_retry_tick;

    if (state == NULL || !state->prompt_request ||
        s_app_phase != APP_PHASE_DRAWING || !s_has_prompt) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if (has_prompt_retry_tick &&
        (now - last_prompt_retry_tick) < pdMS_TO_TICKS(APP_PROMPT_READY_RETRY_MS)) {
        return;
    }

    last_prompt_retry_tick = now;
    has_prompt_retry_tick = true;
    const esp_err_t notify_err = app_send_mcu_prompt_ready();
    if (notify_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to resend PROMPT ready command to MCU: %s", esp_err_to_name(notify_err));
    } else {
        ESP_LOGI(TAG, "Resent PROMPT ready command to MCU");
    }
}

static void app_process_authoritative_input(const image_input_state_t *state,
                                            app_viewer_delta_buffer_t *delta)
{
    if (state == NULL) {
        return;
    }

    if (s_app_phase != APP_PHASE_WAITING_FOR_PROMPT && s_app_phase != APP_PHASE_DRAWING) {
        return;
    }

    image_framebuffer_status_t previous_status = s_framebuffer.status;
    app_process_framebuffer_state(state);
    app_append_viewer_delta_for_state(delta, &previous_status, state);
    app_maybe_resend_prompt_ready(state);
    if (state->erase) {
        const esp_err_t ack_err = app_send_mcu_ack();
        if (ack_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send ACK command to MCU: %s", esp_err_to_name(ack_err));
        }
    }
    if (delta != NULL && delta->len > ((sizeof(delta->json) * 3U) / 4U)) {
        app_viewer_delta_flush(delta);
    }
}

static bool app_request_prompt_fetch(void)
{
    api_request_msg_t request = {
        .type = API_REQUEST_PROMPT,
    };
    strncpy(request.prompt_word, s_active_prompt_word, sizeof(request.prompt_word) - 1U);
    request.prompt_word[sizeof(request.prompt_word) - 1U] = '\0';

    if (!app_enqueue_api_request(&request)) {
        ESP_LOGW(TAG, "Prompt request queue full");
        return false;
    }

    s_prompt_request_in_flight = true;
    return true;
}

static void app_start_new_prompt_request(void)
{
    if (s_app_phase == APP_PHASE_RESULT) {
        image_framebuffer_init(&s_framebuffer);
        s_prompt_request_in_flight = false;
    } else if (s_app_phase == APP_PHASE_DRAWING && s_has_prompt) {
        const esp_err_t notify_err = app_send_mcu_prompt_ready();
        if (notify_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to resend PROMPT ready command to MCU: %s", esp_err_to_name(notify_err));
        }
        return;
    } else if (s_app_phase != APP_PHASE_WAITING_FOR_PROMPT) {
        return;
    }

    s_app_phase = APP_PHASE_WAITING_FOR_PROMPT;
    s_has_prompt = false;
    s_has_result = false;
    app_enqueue_snapshot(APP_VIEWER_TARGET_BROADCAST);
    if (!s_prompt_request_in_flight) {
        app_request_prompt_fetch();
    }
}

static void app_start_submit_request(void)
{
    if (s_app_phase != APP_PHASE_DRAWING) {
        return;
    }

    api_request_msg_t request = {
        .type = API_REQUEST_SUBMIT,
    };
    request.payload = malloc(SOCKET_PAYLOAD_BUFFER_SIZE);
    if (request.payload == NULL) {
        ESP_LOGE(TAG, "Failed to allocate AI submit payload");
        return;
    }
    request.payload_len = image_framebuffer_build_socket_payload(&s_framebuffer,
                                                                 request.payload,
                                                                 SOCKET_PAYLOAD_BUFFER_SIZE);
    if (request.payload_len == 0U) {
        ESP_LOGE(TAG, "Failed to build AI submit payload");
        free(request.payload);
        return;
    }
    strncpy(request.prompt_word, s_active_prompt_word, sizeof(request.prompt_word) - 1U);
    request.prompt_word[sizeof(request.prompt_word) - 1U] = '\0';

    s_app_phase = APP_PHASE_GUESSING;
    s_has_result = false;
    app_enqueue_snapshot(APP_VIEWER_TARGET_BROADCAST);

    if (!app_enqueue_api_request(&request)) {
        free(request.payload);
        app_ai_submit_result_t result = {
            .guess = "queue-full",
            .confidence = 1,
            .correct = false,
        };
        s_last_result = result;
        s_has_result = true;
        s_app_phase = APP_PHASE_RESULT;
        app_enqueue_snapshot(APP_VIEWER_TARGET_BROADCAST);
        app_enqueue_legacy_result_payload(&result);
        const esp_err_t result_err = app_send_mcu_submit_result(false);
        if (result_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send RESULT command to MCU: %s", esp_err_to_name(result_err));
        }
    }
}

static void app_finish_submit_request(const app_ai_submit_result_t *result)
{
    if (result == NULL || s_app_phase != APP_PHASE_GUESSING) {
        return;
    }

    s_last_result = *result;
    s_has_result = true;
    s_app_phase = APP_PHASE_RESULT;
    app_enqueue_snapshot(APP_VIEWER_TARGET_BROADCAST);
    app_enqueue_legacy_result_payload(result);

    const esp_err_t result_err = app_send_mcu_submit_result(result->correct);
    if (result_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send RESULT command to MCU: %s", esp_err_to_name(result_err));
    }

    ESP_LOGI(TAG,
             "Submit result: guess='%s', confidence=%d, correct=%u",
             result->guess,
             result->confidence,
             result->correct ? 1U : 0U);
}

static void app_finish_prompt_request(const char *prompt_word)
{
    if (prompt_word == NULL || prompt_word[0] == '\0') {
        return;
    }

    strncpy(s_active_prompt_word, prompt_word, sizeof(s_active_prompt_word) - 1U);
    s_active_prompt_word[sizeof(s_active_prompt_word) - 1U] = '\0';
    s_has_prompt = true;
    s_has_result = false;
    s_app_phase = APP_PHASE_DRAWING;
    s_prompt_request_in_flight = false;

    app_enqueue_snapshot(APP_VIEWER_TARGET_BROADCAST);
    app_enqueue_legacy_prompt_payload(s_active_prompt_word);

    const esp_err_t notify_err = app_send_mcu_prompt_ready();
    if (notify_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send PROMPT ready command to MCU: %s", esp_err_to_name(notify_err));
    }
}

static void app_process_framebuffer_event(const framebuffer_state_msg_t *msg,
                                          app_viewer_delta_buffer_t *delta)
{
    if (msg == NULL) {
        return;
    }

    if (msg->type == FRAMEBUFFER_EVENT_APPLY_STATE) {
        app_process_authoritative_input(&msg->state, delta);
        return;
    }

    if (msg->type == FRAMEBUFFER_EVENT_SUBMIT) {
        app_viewer_delta_flush(delta);
        app_start_submit_request();
        return;
    }

    if (msg->type == FRAMEBUFFER_EVENT_PROMPT_REQUEST) {
        app_viewer_delta_flush(delta);
        app_start_new_prompt_request();
        return;
    }

    if (msg->type == FRAMEBUFFER_EVENT_SNAPSHOT_REQUEST) {
        app_viewer_delta_flush(delta);
        app_enqueue_snapshot(msg->target_fd);
        return;
    }

    if (msg->type == FRAMEBUFFER_EVENT_SUBMIT_RESULT) {
        app_finish_submit_request(&msg->result);
        return;
    }

    if (msg->type == FRAMEBUFFER_EVENT_PROMPT_READY) {
        app_finish_prompt_request(msg->prompt_word);
    }
}

static void app_uart_rx_task(void *arg)
{
    (void)arg;

    char uart_packet[UART_PACKET_BUFFER_SIZE];
    uart_packet_msg_t msg;
    TickType_t last_stack_log_tick = xTaskGetTickCount();
    TickType_t last_uart_stats_log_tick = last_stack_log_tick;
    TickType_t last_uart_queue_drop_log_tick = 0;

    while (true) {
        app_maybe_log_stack_watermarks(&last_stack_log_tick);
        app_maybe_log_uart_pipeline_stats(&last_uart_stats_log_tick);

        if (!app_uart_read_packet_line(uart_packet, sizeof(uart_packet))) {
            continue;
        }

        s_uart_line_count++;

        const size_t len = strnlen(uart_packet, sizeof(msg.packet_line) - 1U);
        memcpy(msg.packet_line, uart_packet, len);
        msg.packet_line[len] = '\0';

        const BaseType_t queued = xQueueSend(s_uart_packet_queue,
                                             &msg,
                                             pdMS_TO_TICKS(APP_RTOS_CONFIG.uart_queue_send_timeout_ms));
        if (queued != pdPASS) {
            s_uart_queue_drop_count++;
            if (app_log_period_elapsed(&last_uart_queue_drop_log_tick, 1000U)) {
                ESP_LOGW(TAG, "UART packet queue full, dropping packet");
            }
        }
    }
}

static void app_socket_dispatch_task(void *arg)
{
    (void)arg;

    uart_packet_msg_t msg;
    while (true) {
        const BaseType_t received = xQueueReceive(s_uart_packet_queue, &msg, portMAX_DELAY);
        if (received == pdPASS) {
            app_process_packet_line_fast_path(msg.packet_line);
        }
    }
}

static void app_framebuffer_task(void *arg)
{
    (void)arg;

    app_viewer_delta_buffer_t delta;
    app_viewer_delta_reset(&delta);
    framebuffer_state_msg_t msg;
    while (true) {
        const TickType_t wait_ticks = delta.has_events ? pdMS_TO_TICKS(APP_VIEWER_DELTA_PERIOD_MS) : portMAX_DELAY;
        const BaseType_t received = xQueueReceive(s_framebuffer_state_queue, &msg, wait_ticks);
        if (received == pdPASS) {
            app_process_framebuffer_event(&msg, &delta);
        }
        app_viewer_delta_flush_if_due(&delta);
    }
}

static esp_err_t app_noop_send_frame(const char *payload, size_t payload_len)
{
    (void)payload;
    (void)payload_len;
    return ESP_OK;
}

static void app_api_task(void *arg)
{
    (void)arg;

    api_request_msg_t request;
    while (true) {
        const BaseType_t received = xQueueReceive(s_api_request_queue, &request, portMAX_DELAY);
        if (received != pdPASS) {
            continue;
        }

        if (request.type == API_REQUEST_SUBMIT) {
            app_ai_submit_result_t result = {
                .guess = "unknown",
                .confidence = 1,
                .correct = false,
            };
            bool submit_success = false;
            esp_err_t err = ESP_ERR_INVALID_ARG;
            if (request.payload != NULL && request.payload_len > 0U) {
                err = app_api_submit_drawing(request.payload,
                                             request.payload_len,
                                             request.prompt_word,
                                             &result,
                                             &submit_success,
                                             s_submit_stub_success_flag);
            }
            free(request.payload);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "AI submit failed: %s", esp_err_to_name(err));
                strncpy(result.guess, "api-error", sizeof(result.guess) - 1U);
                result.guess[sizeof(result.guess) - 1U] = '\0';
                result.confidence = 1;
                result.correct = false;
            } else if (!submit_success) {
                ESP_LOGW(TAG, "Submit processing failed; waiting for next explicit prompt request");
            }

            framebuffer_state_msg_t response = {
                .type = FRAMEBUFFER_EVENT_SUBMIT_RESULT,
                .result = result,
                .target_fd = APP_VIEWER_TARGET_BROADCAST,
            };
            app_enqueue_framebuffer_msg(&response, pdMS_TO_TICKS(20U));
            continue;
        }

        if (request.type == API_REQUEST_PROMPT) {
            char prompt_word[APP_API_PROMPT_WORD_BUFFER_SIZE];
            strncpy(prompt_word, request.prompt_word, sizeof(prompt_word) - 1U);
            prompt_word[sizeof(prompt_word) - 1U] = '\0';
            if (prompt_word[0] == '\0') {
                strncpy(prompt_word, "square", sizeof(prompt_word) - 1U);
                prompt_word[sizeof(prompt_word) - 1U] = '\0';
            }

            const esp_err_t err = app_api_fetch_and_publish_prompt(app_noop_send_frame,
                                                                   prompt_word,
                                                                   sizeof(prompt_word));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Prompt request failed: %s", esp_err_to_name(err));
            }

            framebuffer_state_msg_t response = {
                .type = FRAMEBUFFER_EVENT_PROMPT_READY,
                .target_fd = APP_VIEWER_TARGET_BROADCAST,
            };
            strncpy(response.prompt_word, prompt_word, sizeof(response.prompt_word) - 1U);
            response.prompt_word[sizeof(response.prompt_word) - 1U] = '\0';
            app_enqueue_framebuffer_msg(&response, pdMS_TO_TICKS(20U));
        }
    }
}

static void app_viewer_task(void *arg)
{
    (void)arg;

    viewer_event_msg_t msg;
    while (true) {
        const BaseType_t received = xQueueReceive(s_viewer_event_queue, &msg, portMAX_DELAY);
        if (received == pdPASS) {
            const esp_err_t err = app_socket_send_frame_to_target(msg.payload, msg.payload_len, msg.target_fd);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Viewer payload send failed: %s", esp_err_to_name(err));
            }
            free(msg.payload);
        }
    }
}

void app_main(void)
{
    image_framebuffer_init(&s_framebuffer);
    app_api_set_openai_api_key(OPENAI_API_KEY);

    const esp_err_t nvs_init_err = app_nvs_init();
    if (nvs_init_err != ESP_OK) {
        return;
    }

    const esp_err_t wifi_init_err = app_wifi_init();
    if (wifi_init_err != ESP_OK) {
        return;
    }

#if CONFIG_HTTPD_WS_SUPPORT
    const esp_err_t ws_start_err = app_ws_server_start();
    if (ws_start_err != ESP_OK) {
        ESP_LOGW(TAG, "Continuing without active WebSocket server");
    }
#else
    ESP_LOGI(TAG, "WebSocket support disabled; viewer payloads are ignored locally");
#endif

    const esp_err_t uart_init_err = app_uart_init();
    if (uart_init_err != ESP_OK) {
        return;
    }

    s_uart_packet_queue = xQueueCreateStatic(APP_RTOS_UART_PACKET_QUEUE_LENGTH,
                                             sizeof(uart_packet_msg_t),
                                             s_uart_packet_queue_storage,
                                             &s_uart_packet_queue_struct);
    if (s_uart_packet_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create UART packet queue");
        return;
    }

    s_framebuffer_state_queue = xQueueCreateStatic(APP_RTOS_FRAMEBUFFER_STATE_QUEUE_LENGTH,
                                                   sizeof(framebuffer_state_msg_t),
                                                   s_framebuffer_state_queue_storage,
                                                   &s_framebuffer_state_queue_struct);
    if (s_framebuffer_state_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create framebuffer state queue");
        vQueueDelete(s_uart_packet_queue);
        s_uart_packet_queue = NULL;
        return;
    }

    s_viewer_event_queue = xQueueCreateStatic(APP_RTOS_VIEWER_EVENT_QUEUE_LENGTH,
                                              sizeof(viewer_event_msg_t),
                                              s_viewer_event_queue_storage,
                                              &s_viewer_event_queue_struct);
    if (s_viewer_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create viewer event queue");
        vQueueDelete(s_framebuffer_state_queue);
        s_framebuffer_state_queue = NULL;
        vQueueDelete(s_uart_packet_queue);
        s_uart_packet_queue = NULL;
        return;
    }

    s_api_request_queue = xQueueCreateStatic(APP_RTOS_API_REQUEST_QUEUE_LENGTH,
                                             sizeof(api_request_msg_t),
                                             s_api_request_queue_storage,
                                             &s_api_request_queue_struct);
    if (s_api_request_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create API request queue");
        vQueueDelete(s_viewer_event_queue);
        s_viewer_event_queue = NULL;
        vQueueDelete(s_framebuffer_state_queue);
        s_framebuffer_state_queue = NULL;
        vQueueDelete(s_uart_packet_queue);
        s_uart_packet_queue = NULL;
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
        vQueueDelete(s_framebuffer_state_queue);
        s_framebuffer_state_queue = NULL;
        vQueueDelete(s_uart_packet_queue);
        s_uart_packet_queue = NULL;
        return;
    }

    s_socket_dispatch_task_handle = xTaskCreateStatic(app_socket_dispatch_task,
                                                      APP_RTOS_CONFIG.socket_dispatch_task_name,
                                                      APP_RTOS_SOCKET_DISPATCH_TASK_STACK_SIZE,
                                                      NULL,
                                                      APP_RTOS_CONFIG.socket_dispatch_task_priority,
                                                      s_socket_dispatch_task_stack,
                                                      &s_socket_dispatch_task_tcb);
    if (s_socket_dispatch_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create socket dispatch task");
        vTaskDelete(s_uart_rx_task_handle);
        s_uart_rx_task_handle = NULL;
        vQueueDelete(s_framebuffer_state_queue);
        s_framebuffer_state_queue = NULL;
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
        vTaskDelete(s_socket_dispatch_task_handle);
        s_socket_dispatch_task_handle = NULL;
        vTaskDelete(s_uart_rx_task_handle);
        s_uart_rx_task_handle = NULL;
        vQueueDelete(s_framebuffer_state_queue);
        s_framebuffer_state_queue = NULL;
        vQueueDelete(s_uart_packet_queue);
        s_uart_packet_queue = NULL;
        return;
    }

    if (xTaskCreate(app_viewer_task,
                    APP_RTOS_CONFIG.viewer_task_name,
                    APP_RTOS_VIEWER_TASK_STACK_SIZE,
                    NULL,
                    APP_RTOS_CONFIG.viewer_task_priority,
                    &s_viewer_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create viewer task");
        return;
    }

    if (xTaskCreate(app_api_task,
                    APP_RTOS_CONFIG.api_task_name,
                    APP_RTOS_API_TASK_STACK_SIZE,
                    NULL,
                    APP_RTOS_CONFIG.api_task_priority,
                    &s_api_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create API task");
        return;
    }

    ESP_LOGI(TAG,
             "RTOS tasks started: %s, %s, %s, %s and %s",
             APP_RTOS_CONFIG.uart_rx_task_name,
             APP_RTOS_CONFIG.socket_dispatch_task_name,
             APP_RTOS_CONFIG.framebuffer_task_name,
             APP_RTOS_CONFIG.viewer_task_name,
             APP_RTOS_CONFIG.api_task_name);

    app_request_prompt_fetch();
}
