#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

class RadarApp;

/*
 * Fetches and installs aircraft thumbnails for the popup.
 *
 * Photo work is kept off the LVGL task because it involves two HTTPS requests
 * and JPEG decoding. The class uses a request_id to discard late results when
 * the user closes the popup or taps a different aircraft.
 */
class AircraftPhotoService {
public:
    /* Attach the owning application used for popup state and HTTP access. */
    void bind(RadarApp *app) { owner = app; }

    /* Extract the first thumbnail URL from a Planespotters metadata response. */
    static bool parseThumbnailUrl(const char *json, int json_len, char *url, size_t url_size);

    /* Keep only hexadecimal characters and uppercase them for the API path. */
    static bool normalizeIcaoHex(char *dst, size_t dst_size, const char *src);

    /* Check whether a worker result still belongs to the visible popup. */
    bool requestIsCurrent(uint32_t request_id) const;

    /* Safely update the popup status label from the worker task. */
    void updateStatusFromTask(uint32_t request_id, const char *status);

    /*
     * Install decoded RGB565 pixels into the popup.
     *
     * On success the popup takes ownership of pixels. If the request is stale,
     * this method frees pixels so the worker can simply return.
     */
    void installFromTask(uint32_t request_id, const lv_image_dsc_t *decoded_image,
                         uint8_t *pixels, size_t pixels_size);

    /* Worker task body for metadata fetch, JPEG fetch, decode, and install. */
    void fetchTask(void *arg);

    /* Start a photo request for the supplied ICAO hex address. */
    void startFetch(const char *icao);

private:
    /*
     * Retry transient low-memory HTTPS failures.
     *
     * TLS cleanup on this board can leave the internal DMA heap low for a short
     * period, especially just after an aircraft fetch. Retrying avoids showing
     * PHOTO MEM LOW for a momentary condition.
     */
    esp_err_t fetchBufferWithRetry(const char *url, const char *accept,
                                   size_t initial_capacity, size_t max_bytes,
                                   char **response_out, int *response_len_out);

    RadarApp *owner = nullptr;
};
