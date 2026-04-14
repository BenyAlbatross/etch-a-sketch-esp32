#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CANVAS_WIDTH 128U
#define CANVAS_HEIGHT 128U
#define FRAMEBUFFER_BYTES ((CANVAS_WIDTH * CANVAS_HEIGHT) / 8U)
#define PNG_ROW_BYTES (CANVAS_WIDTH / 8U)
#define PNG_SCANLINE_BYTES (PNG_ROW_BYTES + 1U)
#define PNG_RAW_IMAGE_BYTES (CANVAS_HEIGHT * PNG_SCANLINE_BYTES)
#define PNG_ZLIB_HEADER_BYTES 2U
#define PNG_DEFLATE_BLOCK_HEADER_BYTES 5U
#define PNG_ADLER32_BYTES 4U
#define PNG_IDAT_DATA_BYTES (PNG_ZLIB_HEADER_BYTES + PNG_DEFLATE_BLOCK_HEADER_BYTES + PNG_RAW_IMAGE_BYTES + PNG_ADLER32_BYTES)
#define PNG_SIGNATURE_BYTES 8U
#define PNG_CHUNK_OVERHEAD_BYTES 12U
#define PNG_IHDR_DATA_BYTES 13U
#define PNG_MAX_BYTES (PNG_SIGNATURE_BYTES + \
                       PNG_CHUNK_OVERHEAD_BYTES + PNG_IHDR_DATA_BYTES + \
                       PNG_CHUNK_OVERHEAD_BYTES + PNG_IDAT_DATA_BYTES + \
                       PNG_CHUNK_OVERHEAD_BYTES)
#define ADLER32_MOD 65521U

#if (CANVAS_WIDTH % 8U) != 0
#error "PNG encoder requires a canvas width divisible by 8"
#endif

#if PNG_RAW_IMAGE_BYTES > 65535U
#error "This host test uses one uncompressed DEFLATE block"
#endif

static void write_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void write_le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t data_len)
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

static uint32_t png_chunk_crc(const char type[4], const uint8_t *data, size_t data_len)
{
    uint32_t crc = 0xffffffffU;
    crc = crc32_update(crc, (const uint8_t *)type, 4U);
    if (data != NULL && data_len > 0U) {
        crc = crc32_update(crc, data, data_len);
    }
    return crc ^ 0xffffffffU;
}

static uint32_t adler32(const uint8_t *data, size_t data_len)
{
    uint32_t a = 1U;
    uint32_t b = 0U;
    for (size_t i = 0U; i < data_len; ++i) {
        a = (a + data[i]) % ADLER32_MOD;
        b = (b + a) % ADLER32_MOD;
    }
    return (b << 16U) | a;
}

static void set_framebuffer_pixel(uint8_t *framebuffer, uint32_t x, uint32_t y)
{
    if (x >= CANVAS_WIDTH || y >= CANVAS_HEIGHT) {
        return;
    }

    const size_t bit_index = ((size_t)y * CANVAS_WIDTH) + x;
    const size_t byte_index = bit_index >> 3U;
    const uint8_t bit_mask = (uint8_t)(0x80U >> (bit_index & 0x07U));
    framebuffer[byte_index] |= bit_mask;
}

static void draw_circle(uint8_t *framebuffer)
{
    const int center_x = (int)CANVAS_WIDTH / 2;
    const int center_y = (int)CANVAS_HEIGHT / 2;
    const int outer_radius = 42;
    const int inner_radius = 38;
    const int outer_radius_squared = outer_radius * outer_radius;
    const int inner_radius_squared = inner_radius * inner_radius;

    for (uint32_t y = 0U; y < CANVAS_HEIGHT; ++y) {
        for (uint32_t x = 0U; x < CANVAS_WIDTH; ++x) {
            const int dx = (int)x - center_x;
            const int dy = (int)y - center_y;
            const int distance_squared = (dx * dx) + (dy * dy);
            if (distance_squared <= outer_radius_squared &&
                distance_squared >= inner_radius_squared) {
                set_framebuffer_pixel(framebuffer, x, y);
            }
        }
    }
}

static void build_png_raw_scanlines(const uint8_t *framebuffer, uint8_t *raw)
{
    size_t raw_offset = 0U;
    for (uint32_t y = 0U; y < CANVAS_HEIGHT; ++y) {
        const size_t framebuffer_offset = (size_t)y * PNG_ROW_BYTES;
        raw[raw_offset++] = 0U;
        for (size_t x = 0U; x < PNG_ROW_BYTES; ++x) {
            raw[raw_offset++] = (uint8_t)~framebuffer[framebuffer_offset + x];
        }
    }
}

static int append_chunk(uint8_t *png,
                        size_t png_capacity,
                        size_t *png_len,
                        const char type[4],
                        const uint8_t *data,
                        size_t data_len)
{
    if (*png_len > png_capacity || png_capacity - *png_len < PNG_CHUNK_OVERHEAD_BYTES + data_len) {
        return -1;
    }

    uint8_t *chunk = png + *png_len;
    write_be32(chunk, (uint32_t)data_len);
    memcpy(chunk + 4U, type, 4U);
    if (data != NULL && data_len > 0U) {
        memcpy(chunk + 8U, data, data_len);
    }
    write_be32(chunk + 8U + data_len, png_chunk_crc(type, data, data_len));
    *png_len += PNG_CHUNK_OVERHEAD_BYTES + data_len;
    return 0;
}

static int append_uncompressed_idat(uint8_t *png,
                                    size_t png_capacity,
                                    size_t *png_len,
                                    const uint8_t *raw,
                                    size_t raw_len)
{
    if (raw_len > 65535U ||
        *png_len > png_capacity ||
        png_capacity - *png_len < PNG_CHUNK_OVERHEAD_BYTES + PNG_IDAT_DATA_BYTES) {
        return -1;
    }

    uint8_t *chunk = png + *png_len;
    uint8_t *idat = chunk + 8U;
    size_t idat_offset = 0U;
    const uint16_t block_len = (uint16_t)raw_len;
    const uint16_t block_nlen = (uint16_t)~block_len;

    write_be32(chunk, PNG_IDAT_DATA_BYTES);
    memcpy(chunk + 4U, "IDAT", 4U);

    idat[idat_offset++] = 0x78U;
    idat[idat_offset++] = 0x01U;
    idat[idat_offset++] = 0x01U;
    write_le16(idat + idat_offset, block_len);
    idat_offset += 2U;
    write_le16(idat + idat_offset, block_nlen);
    idat_offset += 2U;
    memcpy(idat + idat_offset, raw, raw_len);
    idat_offset += raw_len;
    write_be32(idat + idat_offset, adler32(raw, raw_len));
    idat_offset += PNG_ADLER32_BYTES;

    if (idat_offset != PNG_IDAT_DATA_BYTES) {
        return -1;
    }

    write_be32(chunk + 8U + idat_offset, png_chunk_crc("IDAT", idat, idat_offset));
    *png_len += PNG_CHUNK_OVERHEAD_BYTES + idat_offset;
    return 0;
}

static int encode_png_1bpp(const uint8_t *framebuffer,
                           uint8_t *out_png,
                           size_t out_png_capacity,
                           size_t *out_png_len)
{
    uint8_t raw[PNG_RAW_IMAGE_BYTES];
    uint8_t ihdr[PNG_IHDR_DATA_BYTES] = {0};
    static const uint8_t png_signature[PNG_SIGNATURE_BYTES] = {
        0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n',
    };

    if (out_png_capacity < PNG_MAX_BYTES) {
        return -1;
    }

    build_png_raw_scanlines(framebuffer, raw);
    write_be32(ihdr, CANVAS_WIDTH);
    write_be32(ihdr + 4U, CANVAS_HEIGHT);
    ihdr[8] = 1U;
    ihdr[9] = 0U;
    ihdr[10] = 0U;
    ihdr[11] = 0U;
    ihdr[12] = 0U;

    size_t png_len = 0U;
    memcpy(out_png, png_signature, sizeof(png_signature));
    png_len += sizeof(png_signature);

    if (append_chunk(out_png, out_png_capacity, &png_len, "IHDR", ihdr, sizeof(ihdr)) != 0) {
        return -1;
    }
    if (append_uncompressed_idat(out_png, out_png_capacity, &png_len, raw, sizeof(raw)) != 0) {
        return -1;
    }
    if (append_chunk(out_png, out_png_capacity, &png_len, "IEND", NULL, 0U) != 0) {
        return -1;
    }

    *out_png_len = png_len;
    return 0;
}

int main(int argc, char **argv)
{
    const char *output_path = (argc >= 2) ? argv[1] : "circle.png";
    uint8_t framebuffer[FRAMEBUFFER_BYTES] = {0};
    uint8_t png[PNG_MAX_BYTES];
    size_t png_len = 0U;

    draw_circle(framebuffer);

    if (encode_png_1bpp(framebuffer, png, sizeof(png), &png_len) != 0) {
        fprintf(stderr, "failed to encode PNG\n");
        return 1;
    }

    FILE *file = fopen(output_path, "wb");
    if (file == NULL) {
        perror(output_path);
        return 1;
    }

    const size_t written = fwrite(png, 1U, png_len, file);
    if (fclose(file) != 0 || written != png_len) {
        fprintf(stderr, "failed to write complete PNG\n");
        return 1;
    }

    printf("wrote %s (%zu bytes)\n", output_path, png_len);
    return 0;
}
