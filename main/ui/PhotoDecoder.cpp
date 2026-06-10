#include "PhotoDecoder.hpp"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "photo_decode";

/*
 * stb_image is used in memory-only mode. All allocations prefer PSRAM because
 * decoding can temporarily require more memory than the final RGB565 image.
 */

/* Allocate stb_image memory from PSRAM first, then internal RAM if needed. */
static void *photo_stbi_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

/* Resize stb_image memory while preserving the PSRAM-first allocation policy. */
static void * __attribute__((unused)) photo_stbi_realloc(void *ptr, size_t size)
{
    void *resized = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resized) {
        resized = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    }
    return resized;
}

/* Free memory allocated by the stb_image allocation hooks. */
static void photo_stbi_free(void *ptr)
{
    heap_caps_free(ptr);
}

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MALLOC(sz) photo_stbi_malloc(sz)
#define STBI_REALLOC(p, newsz) photo_stbi_realloc(p, newsz)
#define STBI_REALLOC_SIZED(p, oldsz, newsz) photo_stbi_realloc(p, newsz)
#define STBI_FREE(p) photo_stbi_free(p)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "libs/gltf/stb_image/stb_image.h"
#pragma GCC diagnostic pop

/* Allocate the final RGB565 popup pixel buffer. */
static uint8_t *alloc_photo_pixels(size_t size)
{
    uint8_t *pixels = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) {
        pixels = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return pixels;
}

/* Decode a JPEG thumbnail into an LVGL-compatible RGB565 image descriptor. */
bool PhotoDecoder::decodeJpegToRgb565(const uint8_t *jpeg, size_t jpeg_size,
                                      lv_image_dsc_t *image, uint8_t **pixels_out,
                                      size_t *pixels_size_out)
{
    if (!jpeg || jpeg_size == 0 || !image || !pixels_out) {
        return false;
    }

    *pixels_out = NULL;
    if (pixels_size_out) {
        *pixels_size_out = 0;
    }
    memset(image, 0, sizeof(*image));

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *rgb = stbi_load_from_memory(jpeg, (int)jpeg_size, &width, &height, &channels, 3);
    if (!rgb || width <= 0 || height <= 0) {
        ESP_LOGW(TAG, "JPEG decode failed: %s", stbi_failure_reason());
        if (rgb) {
            stbi_image_free(rgb);
        }
        return false;
    }

    if (width > 320 || height > 240) {
        ESP_LOGW(TAG, "JPEG too large for popup: %dx%d", width, height);
        stbi_image_free(rgb);
        return false;
    }

    size_t pixel_count = (size_t)width * (size_t)height;
    size_t stride = (size_t)width * 2;
    size_t pixels_size = stride * (size_t)height;
    if (height != 0 && pixels_size / (size_t)height != stride) {
        stbi_image_free(rgb);
        return false;
    }

    uint8_t *pixels = alloc_photo_pixels(pixels_size);
    if (!pixels) {
        ESP_LOGW(TAG, "No memory for decoded photo (%u bytes)", (unsigned)pixels_size);
        stbi_image_free(rgb);
        return false;
    }

    /*
     * Convert from RGB888 to RGB565 in one pass. The output buffer is passed
     * directly to LVGL, so its lifetime must outlive the visible image.
     */
    uint16_t *dst = (uint16_t *)pixels;
    for (size_t i = 0; i < pixel_count; ++i) {
        uint8_t r = rgb[i * 3 + 0];
        uint8_t g = rgb[i * 3 + 1];
        uint8_t b = rgb[i * 3 + 2];
        dst[i] = (uint16_t)(((uint16_t)(r & 0xf8) << 8) |
                            ((uint16_t)(g & 0xfc) << 3) |
                            ((uint16_t)b >> 3));
    }

    stbi_image_free(rgb);

    image->header.magic = LV_IMAGE_HEADER_MAGIC;
    image->header.cf = LV_COLOR_FORMAT_RGB565;
    image->header.w = (uint32_t)width;
    image->header.h = (uint32_t)height;
    image->header.stride = (uint32_t)stride;
    image->data_size = (uint32_t)pixels_size;
    image->data = pixels;

    *pixels_out = pixels;
    if (pixels_size_out) {
        *pixels_size_out = pixels_size;
    }
    return true;
}
