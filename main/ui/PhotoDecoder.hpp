#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

/*
 * JPEG thumbnail decoder for aircraft photos.
 *
 * Planespotters thumbnails arrive as JPEG. LVGL on this build displays the
 * popup image from an RGB565 buffer, so the decoder converts and allocates a
 * display-ready image descriptor. The caller owns the returned pixel buffer.
 */
class PhotoDecoder {
public:
    /*
     * Decode JPEG bytes into an LVGL RGB565 image.
     *
     * pixels_out receives heap memory on success and must be freed with
     * heap_caps_free once the LVGL image no longer uses it.
     */
    static bool decodeJpegToRgb565(const uint8_t *jpeg, size_t jpeg_size,
                                   lv_image_dsc_t *image, uint8_t **pixels_out,
                                   size_t *pixels_size_out);
};
