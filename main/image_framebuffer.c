#include "image_framebuffer.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/base64.h"

#define IMAGE_FRAMEBUFFER_BYTES ((IMAGE_FRAMEBUFFER_CANVAS_WIDTH * IMAGE_FRAMEBUFFER_CANVAS_HEIGHT + 7U) / 8U)
#define IMAGE_FRAMEBUFFER_SOCKET_JSON_PREFIX "{\"type\":\"frame\",\"width\":128,\"height\":128,\"format\":\"1bpp-msb\",\"data\":\""
#define IMAGE_FRAMEBUFFER_SOCKET_JSON_SUFFIX "\"}"
#define IMAGE_FRAMEBUFFER_INPUT_PACKET_BUFFER_LEN 96

static bool image_framebuffer_parse_int_token(const char *token, int *out_value)
{
    if (token == NULL || out_value == NULL || token[0] == '\0') {
        return false;
    }

    char *end_ptr = NULL;
    const long parsed = strtol(token, &end_ptr, 10);
    if (end_ptr == token || *end_ptr != '\0') {
        return false;
    }

    *out_value = (int)parsed;
    return true;
}

static inline size_t image_framebuffer_index(uint16_t x, uint16_t y)
{
    return ((size_t)y * (size_t)IMAGE_FRAMEBUFFER_CANVAS_WIDTH) + (size_t)x;
}

static inline void image_framebuffer_set_pixel(image_framebuffer_t *framebuffer, uint16_t x, uint16_t y)
{
    if (x >= IMAGE_FRAMEBUFFER_CANVAS_WIDTH || y >= IMAGE_FRAMEBUFFER_CANVAS_HEIGHT) {
        return;
    }

    const size_t bit_index = image_framebuffer_index(x, y);
    const size_t byte_index = bit_index >> 3U;
    const uint8_t bit_mask = (uint8_t)(0x80U >> (bit_index & 0x07U));
    framebuffer->framebuffer[byte_index] |= bit_mask;
}

static void image_framebuffer_draw_line(image_framebuffer_t *framebuffer, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    int32_t dx = (int32_t)x1 - (int32_t)x0;
    int32_t dy = (int32_t)y1 - (int32_t)y0;

    int32_t sx = (dx >= 0) ? 1 : -1;
    int32_t sy = (dy >= 0) ? 1 : -1;
    dx = (dx >= 0) ? dx : -dx;
    dy = (dy >= 0) ? dy : -dy;

    int32_t err = dx - dy;
    int32_t x = (int32_t)x0;
    int32_t y = (int32_t)y0;

    while (true) {
        image_framebuffer_set_pixel(framebuffer, (uint16_t)x, (uint16_t)y);
        if (x == (int32_t)x1 && y == (int32_t)y1) {
            break;
        }

        const int32_t twice_err = err << 1;
        if (twice_err > -dy) {
            err -= dy;
            x += sx;
        }
        if (twice_err < dx) {
            err += dx;
            y += sy;
        }
    }
}

void image_framebuffer_init(image_framebuffer_t *framebuffer)
{
    if (framebuffer == NULL) {
        return;
    }

    memset(framebuffer->framebuffer, 0, sizeof(framebuffer->framebuffer));
    framebuffer->status.cursor_x = 0;
    framebuffer->status.cursor_y = 0;
    framebuffer->status.pen_down = false;
    framebuffer->status.submit_pending = false;
    framebuffer->status.has_cursor = false;
}

void image_framebuffer_clear(image_framebuffer_t *framebuffer)
{
    if (framebuffer == NULL) {
        return;
    }

    memset(framebuffer->framebuffer, 0, sizeof(framebuffer->framebuffer));
    framebuffer->status.submit_pending = false;
}

bool image_framebuffer_parse_input_packet(const char *packet, image_input_state_t *out_state)
{
    if (packet == NULL || out_state == NULL) {
        return false;
    }

    if (strncmp(packet, "$S,", 3U) != 0) {
        return false;
    }

    char packet_copy[IMAGE_FRAMEBUFFER_INPUT_PACKET_BUFFER_LEN];
    const size_t packet_len = strnlen(packet, sizeof(packet_copy));
    if (packet_len == 0U || packet_len >= sizeof(packet_copy)) {
        return false;
    }
    memcpy(packet_copy, packet, packet_len);
    packet_copy[packet_len] = '\0';

    int fields[6] = {0, 0, 0, 0, 0, 0};
    size_t field_count = 0U;

    char *save_ptr = NULL;
    char *token = strtok_r(packet_copy + 3, ",", &save_ptr);
    while (token != NULL && field_count < 6U) {
        if (!image_framebuffer_parse_int_token(token, &fields[field_count])) {
            return false;
        }
        field_count++;
        token = strtok_r(NULL, ",", &save_ptr);
    }

    if (token != NULL) {
        // More than 6 comma-separated values is invalid.
        return false;
    }

    // Backward compatible parsing:
    // $S,x,y,pen
    // $S,x,y,pen,erase
    // $S,x,y,pen,erase,submit
    // $S,x,y,pen,erase,submit,promptRequest
    if (field_count < 3U) {
        return false;
    }

    const int x = fields[0];
    const int y = fields[1];
    const int pen_down = fields[2];
    const int erase = (field_count >= 4U) ? fields[3] : 0;
    const int submit = (field_count >= 5U) ? fields[4] : 0;
    const int prompt_request = (field_count >= 6U) ? fields[5] : 0;

    if (x < 0 || x >= IMAGE_FRAMEBUFFER_CANVAS_WIDTH || y < 0 || y >= IMAGE_FRAMEBUFFER_CANVAS_HEIGHT) {
        return false;
    }
    if ((pen_down != 0 && pen_down != 1) ||
        (erase != 0 && erase != 1) ||
        (submit != 0 && submit != 1) ||
        (prompt_request != 0 && prompt_request != 1)) {
        return false;
    }

    out_state->x = (uint16_t)x;
    out_state->y = (uint16_t)y;
    out_state->pen_down = (pen_down == 1);
    out_state->erase = (erase == 1);
    out_state->submit = (submit == 1);
    out_state->prompt_request = (prompt_request == 1);
    return true;
}

void image_framebuffer_apply_input(image_framebuffer_t *framebuffer, const image_input_state_t *state)
{
    if (framebuffer == NULL || state == NULL) {
        return;
    }

    if (state->erase) {
        image_framebuffer_clear(framebuffer);
    }

    if (state->pen_down) {
        if (framebuffer->status.has_cursor) {
            image_framebuffer_draw_line(framebuffer,
                                    framebuffer->status.cursor_x,
                                    framebuffer->status.cursor_y,
                                    state->x,
                                    state->y);
        } else {
            image_framebuffer_set_pixel(framebuffer, state->x, state->y);
        }
    }

    framebuffer->status.cursor_x = state->x;
    framebuffer->status.cursor_y = state->y;
    framebuffer->status.pen_down = state->pen_down;
    framebuffer->status.submit_pending = state->submit;
    framebuffer->status.has_cursor = true;
}

const image_framebuffer_status_t *image_framebuffer_get_status(const image_framebuffer_t *framebuffer)
{
    if (framebuffer == NULL) {
        return NULL;
    }
    return &framebuffer->status;
}

size_t image_framebuffer_build_socket_payload(const image_framebuffer_t *framebuffer, char *out_json, size_t out_json_len)
{
    if (framebuffer == NULL || out_json == NULL || out_json_len == 0U) {
        return 0U;
    }

    const size_t prefix_len = sizeof(IMAGE_FRAMEBUFFER_SOCKET_JSON_PREFIX) - 1U;
    const size_t suffix_len = sizeof(IMAGE_FRAMEBUFFER_SOCKET_JSON_SUFFIX) - 1U;
    const size_t base64_max = 4U * ((IMAGE_FRAMEBUFFER_BYTES + 2U) / 3U);
    const size_t needed = prefix_len + base64_max + suffix_len + 1U;
    if (out_json_len < needed) {
        return 0U;
    }

    memcpy(out_json, IMAGE_FRAMEBUFFER_SOCKET_JSON_PREFIX, prefix_len);

    size_t base64_len = 0U;
    const int ret = mbedtls_base64_encode((unsigned char *)(out_json + prefix_len),
                                          out_json_len - prefix_len,
                                          &base64_len,
                                          framebuffer->framebuffer,
                                          IMAGE_FRAMEBUFFER_BYTES);
    if (ret != 0) {
        return 0U;
    }

    memcpy(out_json + prefix_len + base64_len, IMAGE_FRAMEBUFFER_SOCKET_JSON_SUFFIX, suffix_len);
    out_json[prefix_len + base64_len + suffix_len] = '\0';
    return prefix_len + base64_len + suffix_len;
}
