/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "image_framebuffer.h"

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

    // Integration point: replace this with HTTPS/API call to vision model.
    ESP_LOGI(TAG, "AI submit hook pending integration, payload_len=%u", (unsigned)payload_len);
    ESP_LOGI(TAG, "AI payload prefix: %.120s", payload);
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

    const esp_err_t ws_start_err = app_ws_server_start();
    if (ws_start_err != ESP_OK) {
        ESP_LOGW(TAG, "Continuing without active WebSocket server");
    }

    const esp_err_t uart_init_err = app_uart_init();
    if (uart_init_err != ESP_OK) {
        return;
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
