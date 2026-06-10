#pragma once

#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_client.h"

#include "RadarTypes.hpp"

/*
 * HTTP/TLS helper shared by aircraft, photo, settings, and runway fetches.
 *
 * The ESP32-P4 plus hosted WiFi module is sensitive to concurrent TLS work:
 * certificate validation, AES buffers, SDIO packet buffers, and LVGL all
 * compete for internal DMA-capable memory. This class serialises HTTPS fetches
 * with one mutex and prefers PSRAM for response buffers wherever possible.
 */
class RadarHttpClient {
public:
    /* Create the TLS serialisation mutex. */
    void init();

    /* Exposed for diagnostics and services that need to coordinate with HTTP. */
    SemaphoreHandle_t mutex() const { return http_mutex; }

    /* Route cJSON heap use to PSRAM first. */
    static void initJsonAllocator();

    /* Allocate an HTTP response buffer, preferring PSRAM. */
    static char *allocResponse(size_t capacity);

    /* Resize a response buffer without unnecessarily consuming internal RAM. */
    static char *resizeResponse(char *response, size_t capacity);

    /* Cheap structural check used to reject truncated JSON responses. */
    static bool jsonLooksComplete(const char *json, int json_len);

    /* ESP HTTP client callback that appends streamed response data. */
    static esp_err_t eventHandler(esp_http_client_event_t *evt);

    /* Read a required numeric JSON field. */
    static bool jsonGetNumber(cJSON *object, const char *name, double *value);

    /* Read a numeric JSON field that may be encoded as a string. */
    static bool jsonGetDouble(cJSON *object, const char *name, double *value);

    /* Copy a JSON string field after trimming surrounding whitespace. */
    static void copyTrimmedField(char *dst, size_t dst_size, cJSON *object, const char *name);

    /* Normalise an ICAO airport code to its uppercase four-character form. */
    static bool normalizeIcaoCode(char *dst, size_t dst_size, const char *src);

    /* Return whether haystack contains needle, ignoring case. */
    static bool stringContainsCi(const char *haystack, const char *needle);

    /* Create the long-lived Airplanes.live client. Caller owns the handle. */
    esp_http_client_handle_t createAircraftClient();

    /*
     * Fetch aircraft JSON using an existing client.
     *
     * response_out receives a heap buffer owned by the caller on success.
     */
    esp_err_t fetchAircraftJson(esp_http_client_handle_t client, const char *url,
                                char **response_out, int *response_len_out);

    /*
     * Fetch a bounded generic response.
     *
     * Used for photo metadata, JPEG thumbnails, AirportDB, and settings lookup
     * APIs. The caller owns response_out on success.
     */
    esp_err_t fetchBuffer(const char *url, const char *accept, const char *origin,
                          size_t initial_capacity, size_t max_bytes,
                          char **response_out, int *response_len_out,
                          int mutex_timeout_ms = HTTP_BUFFER_TLS_MUTEX_TIMEOUT_MS,
                          int http_timeout_ms = PHOTO_HTTP_TIMEOUT_MS);

private:
    SemaphoreHandle_t http_mutex = nullptr;

    /* Allocate cJSON memory from PSRAM when possible. */
    static void *jsonMalloc(size_t size);

    /* Release memory allocated by jsonMalloc. */
    static void jsonFree(void *ptr);

    /* Inflate gzip response data into the response buffer. */
    static esp_err_t appendGzipResponse(z_stream *stream, const char *input, int input_len,
                                        char **json, size_t *capacity, bool *finished);

    /* Ensure the response buffer can hold the requested byte count. */
    static esp_err_t reserveResponse(http_fetch_context_t *context, size_t required);

    /* Append uncompressed response bytes to the fetch context. */
    static esp_err_t appendPlainResponse(http_fetch_context_t *context, const char *data, int data_len);

    /* Append response bytes, dispatching to gzip handling when needed. */
    static esp_err_t appendFetchResponse(http_fetch_context_t *context, const char *data, int data_len);

    /* Browser-like headers required by some public photo APIs. */
    static void setBrowserHeaders(esp_http_client_handle_t client, const char *accept, const char *origin);
};
