/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_api.h"
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
#define VIEWER_STROKE_PAYLOAD_BUFFER_SIZE 160
#define APP_WS_ENDPOINT_URI "/ws"
#define APP_WS_SUBPROTOCOL "etchsketch.v1.json"
#define APP_WIFI_CONNECTED_BIT BIT0
#define APP_MCU_CONTROL_PACKET_BUFFER_SIZE 48
#define APP_MCU_RESULT_COMMAND "RESULT"
#define APP_MCU_PROMPT_READY_COMMAND "PROMPT"

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
    APP_RTOS_UART_RX_TASK_STACK_SIZE = 4096,
    APP_RTOS_SOCKET_DISPATCH_TASK_STACK_SIZE = 8192,
    APP_RTOS_FRAMEBUFFER_TASK_STACK_SIZE = 12288,
};

typedef struct {
    const char *uart_rx_task_name;
    const char *socket_dispatch_task_name;
    const char *framebuffer_task_name;
    uint32_t uart_queue_send_timeout_ms;
    uint32_t framebuffer_queue_send_timeout_ms;
    uint32_t stack_watermark_log_period_ms;
    UBaseType_t uart_rx_task_priority;
    UBaseType_t socket_dispatch_task_priority;
    UBaseType_t framebuffer_task_priority;
} app_rtos_config_t;

static const app_rtos_config_t APP_RTOS_CONFIG = {
    .uart_rx_task_name = "uart_rx_task",
    .socket_dispatch_task_name = "socket_dispatch_task",
    .framebuffer_task_name = "framebuffer_task",
    .uart_queue_send_timeout_ms = 0U,
    .framebuffer_queue_send_timeout_ms = 0U,
    .stack_watermark_log_period_ms = 5000U,
    .uart_rx_task_priority = 5,
    .socket_dispatch_task_priority = 6,
    .framebuffer_task_priority = 4,
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
#endif

typedef struct {
    char packet_line[UART_PACKET_BUFFER_SIZE];
} uart_packet_msg_t;

typedef enum {
    FRAMEBUFFER_EVENT_APPLY_STATE = 0,
    FRAMEBUFFER_EVENT_SUBMIT = 1,
    FRAMEBUFFER_EVENT_PROMPT_REQUEST = 2,
} framebuffer_event_type_t;

typedef struct {
    framebuffer_event_type_t type;
    image_input_state_t state;
} framebuffer_state_msg_t;

static StaticQueue_t s_uart_packet_queue_struct;
static uint8_t s_uart_packet_queue_storage[APP_RTOS_UART_PACKET_QUEUE_LENGTH * sizeof(uart_packet_msg_t)];

static StaticQueue_t s_framebuffer_state_queue_struct;
static uint8_t s_framebuffer_state_queue_storage[APP_RTOS_FRAMEBUFFER_STATE_QUEUE_LENGTH * sizeof(framebuffer_state_msg_t)];

static StaticTask_t s_uart_rx_task_tcb;
static StackType_t s_uart_rx_task_stack[APP_RTOS_UART_RX_TASK_STACK_SIZE];
static TaskHandle_t s_uart_rx_task_handle;

static StaticTask_t s_socket_dispatch_task_tcb;
static StackType_t s_socket_dispatch_task_stack[APP_RTOS_SOCKET_DISPATCH_TASK_STACK_SIZE];
static TaskHandle_t s_socket_dispatch_task_handle;

static StaticTask_t s_framebuffer_task_tcb;
static StackType_t s_framebuffer_task_stack[APP_RTOS_FRAMEBUFFER_TASK_STACK_SIZE];
static TaskHandle_t s_framebuffer_task_handle;

static uint32_t s_uart_line_count;
static uint32_t s_uart_queue_drop_count;
static uint32_t s_framebuffer_queue_drop_count;
static uint32_t s_uart_parse_ok_count;
static uint32_t s_uart_parse_fail_count;
// Temporary integration flag until teammate-owned API layer is connected.
static bool s_submit_stub_success_flag = true;

static esp_err_t app_socket_send_frame(const char *payload, size_t payload_len);

static char s_active_prompt_word[APP_API_PROMPT_WORD_BUFFER_SIZE] = "house";

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

    const esp_err_t result_err = app_send_mcu_submit_result(result->correct);
    if (result_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send RESULT command to MCU: %s", esp_err_to_name(result_err));
    }
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
    const UBaseType_t socket_dispatch_stack_hwm =
        (s_socket_dispatch_task_handle != NULL) ? uxTaskGetStackHighWaterMark(s_socket_dispatch_task_handle) : 0U;
    const UBaseType_t framebuffer_stack_hwm =
        (s_framebuffer_task_handle != NULL) ? uxTaskGetStackHighWaterMark(s_framebuffer_task_handle) : 0U;

    ESP_LOGI(TAG,
             "Stack watermark bytes: %s=%u, %s=%u, %s=%u",
             APP_RTOS_CONFIG.uart_rx_task_name,
             (unsigned)uart_rx_stack_hwm,
             APP_RTOS_CONFIG.socket_dispatch_task_name,
             (unsigned)socket_dispatch_stack_hwm,
             APP_RTOS_CONFIG.framebuffer_task_name,
             (unsigned)framebuffer_stack_hwm);

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

    ESP_LOGI(TAG,
             "UART pipeline: lines=%u parsed=%u malformed=%u uartDrops=%u uartDepth=%u fbDrops=%u fbDepth=%u",
             (unsigned)s_uart_line_count,
             (unsigned)s_uart_parse_ok_count,
             (unsigned)s_uart_parse_fail_count,
             (unsigned)s_uart_queue_drop_count,
             (unsigned)uart_queue_depth,
             (unsigned)s_framebuffer_queue_drop_count,
             (unsigned)framebuffer_queue_depth);

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

    app_ai_submit_result_t result = {
        .guess = "unknown",
        .confidence = 1,
        .correct = false,
    };
    bool submit_success = false;

    const esp_err_t err = app_api_submit_drawing(socket_payload,
                                                 payload_len,
                                                 &result,
                                                 &submit_success,
                                                 s_submit_stub_success_flag);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AI submit failed: %s", esp_err_to_name(err));
        return;
    }

    app_emit_submit_result(&result);
    ESP_LOGI(TAG,
             "Submit result: guess='%s', confidence=%d, correct=%u (stubSuccess=%u)",
             result.guess,
             result.confidence,
             result.correct ? 1U : 0U,
             submit_success ? 1U : 0U);

    if (!submit_success) {
        ESP_LOGW(TAG, "Submit processing failed; waiting for next explicit prompt request");
    }
}

static void app_enqueue_framebuffer_state(const image_input_state_t *state)
{
    if (state == NULL) {
        return;
    }

    framebuffer_state_msg_t msg = {
        .type = FRAMEBUFFER_EVENT_APPLY_STATE,
        .state = *state,
    };

    if (s_framebuffer_state_queue == NULL) {
        return;
    }

    const BaseType_t queued = xQueueSend(s_framebuffer_state_queue,
                                         &msg,
                                         pdMS_TO_TICKS(APP_RTOS_CONFIG.framebuffer_queue_send_timeout_ms));
    if (queued != pdPASS) {
        s_framebuffer_queue_drop_count++;
    }
}

static void app_enqueue_submit_event(void)
{
    if (s_framebuffer_state_queue == NULL) {
        return;
    }

    framebuffer_state_msg_t msg = {
        .type = FRAMEBUFFER_EVENT_SUBMIT,
    };

    const BaseType_t queued = xQueueSend(s_framebuffer_state_queue,
                                         &msg,
                                         pdMS_TO_TICKS(APP_RTOS_CONFIG.framebuffer_queue_send_timeout_ms));
    if (queued != pdPASS) {
        s_framebuffer_queue_drop_count++;
    }
}

static void app_enqueue_prompt_request_event(void)
{
    if (s_framebuffer_state_queue == NULL) {
        return;
    }

    framebuffer_state_msg_t msg = {
        .type = FRAMEBUFFER_EVENT_PROMPT_REQUEST,
    };

    const BaseType_t queued = xQueueSend(s_framebuffer_state_queue,
                                         &msg,
                                         pdMS_TO_TICKS(APP_RTOS_CONFIG.framebuffer_queue_send_timeout_ms));
    if (queued != pdPASS) {
        s_framebuffer_queue_drop_count++;
    }
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

    // Fast path: publish raw pen movement to the browser immediately.
    app_send_viewer_stroke_update(&state);

    const bool submit_rising_edge = state.submit && !s_submit_level_high;
    s_submit_level_high = state.submit;

    const bool prompt_request_rising_edge = state.prompt_request && !s_prompt_request_level_high;
    s_prompt_request_level_high = state.prompt_request;

    // Slow path: build authoritative framebuffer state in a lower-priority worker.
    app_enqueue_framebuffer_state(&state);

    // Command path: enqueue explicit submit event on rising edge only.
    if (submit_rising_edge) {
        app_enqueue_submit_event();
    }

    if (prompt_request_rising_edge) {
        app_enqueue_prompt_request_event();
    }
}

static void app_process_framebuffer_state(const image_input_state_t *state)
{
    if (state == NULL) {
        return;
    }

    image_framebuffer_apply_input(&s_framebuffer, state);
}

static void app_process_framebuffer_event(const framebuffer_state_msg_t *msg,
                                          char *socket_payload,
                                          size_t socket_payload_len)
{
    if (msg == NULL) {
        return;
    }

    if (msg->type == FRAMEBUFFER_EVENT_APPLY_STATE) {
        app_process_framebuffer_state(&msg->state);
        return;
    }

    if (msg->type == FRAMEBUFFER_EVENT_SUBMIT) {
        // Submit is reserved for the AI guess pipeline.
        app_submit_drawing_for_ai(socket_payload, socket_payload_len);
        return;
    }

    if (msg->type == FRAMEBUFFER_EVENT_PROMPT_REQUEST) {
        const esp_err_t prompt_err = app_api_fetch_and_publish_prompt(app_socket_send_frame,
                                                                       s_active_prompt_word,
                                                                       sizeof(s_active_prompt_word));
        if (prompt_err != ESP_OK) {
            ESP_LOGW(TAG, "Prompt request failed: %s", esp_err_to_name(prompt_err));
            return;
        }

        const esp_err_t notify_err = app_send_mcu_prompt_ready();
        if (notify_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send PROMPT ready command to MCU: %s", esp_err_to_name(notify_err));
        }
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

    char *socket_payload = malloc(SOCKET_PAYLOAD_BUFFER_SIZE);
    if (socket_payload == NULL) {
        ESP_LOGE(TAG, "Failed to allocate socket payload buffer");
        vTaskDelete(NULL);
    }

    framebuffer_state_msg_t msg;
    while (true) {
        const BaseType_t received = xQueueReceive(s_framebuffer_state_queue, &msg, portMAX_DELAY);
        if (received == pdPASS) {
            app_process_framebuffer_event(&msg, socket_payload, SOCKET_PAYLOAD_BUFFER_SIZE);
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

    ESP_LOGI(TAG,
             "RTOS tasks started: %s, %s and %s",
             APP_RTOS_CONFIG.uart_rx_task_name,
             APP_RTOS_CONFIG.socket_dispatch_task_name,
             APP_RTOS_CONFIG.framebuffer_task_name);

    const esp_err_t prompt_init_err = app_api_fetch_and_publish_prompt(app_socket_send_frame,
                                                                       s_active_prompt_word,
                                                                       sizeof(s_active_prompt_word));
    if (prompt_init_err != ESP_OK) {
        ESP_LOGW(TAG, "Initial prompt fetch/publish failed: %s", esp_err_to_name(prompt_init_err));
    } else {
        const esp_err_t notify_err = app_send_mcu_prompt_ready();
        if (notify_err != ESP_OK) {
            ESP_LOGW(TAG, "Initial PROMPT ready command failed: %s", esp_err_to_name(notify_err));
        }
    }
}
