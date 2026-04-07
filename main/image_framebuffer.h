#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMAGE_FRAMEBUFFER_CANVAS_WIDTH 128
#define IMAGE_FRAMEBUFFER_CANVAS_HEIGHT 128

typedef struct {
    uint16_t x;
    uint16_t y;
    bool pen_down;
    bool erase;
    bool submit;
    bool prompt_request;
} image_input_state_t;

typedef struct {
    uint16_t cursor_x;
    uint16_t cursor_y;
    bool pen_down;
    bool submit_pending;
    bool has_cursor;
} image_framebuffer_status_t;

typedef struct {
    uint8_t framebuffer[(IMAGE_FRAMEBUFFER_CANVAS_WIDTH * IMAGE_FRAMEBUFFER_CANVAS_HEIGHT + 7U) / 8U];
    image_framebuffer_status_t status;
} image_framebuffer_t;

void image_framebuffer_init(image_framebuffer_t *framebuffer);
void image_framebuffer_clear(image_framebuffer_t *framebuffer);
bool image_framebuffer_parse_input_packet(const char *packet, image_input_state_t *out_state);
void image_framebuffer_apply_input(image_framebuffer_t *framebuffer, const image_input_state_t *state);
const image_framebuffer_status_t *image_framebuffer_get_status(const image_framebuffer_t *framebuffer);

/**
 * Builds a JSON payload for socket transport in this shape:
 * {"type":"frame","width":128,"height":128,"format":"1bpp-msb","data":"<base64>"}
 * Returns the number of bytes written (excluding null terminator), or 0 on failure.
 */
size_t image_framebuffer_build_socket_payload(const image_framebuffer_t *framebuffer, char *out_json, size_t out_json_len);

#ifdef __cplusplus
}
#endif
