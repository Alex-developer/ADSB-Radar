#include "RadarHttpClient.hpp"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "radar_http";

namespace {

/*
 * Internal DMA heap thresholds used before starting TLS.
 *
 * The hosted WiFi stack and mbedTLS both need DMA-capable internal memory. The
 * thresholds are intentionally lower for short buffer fetches than for aircraft
 * JSON because photo metadata and thumbnails are optional and small, while the
 * aircraft feed has to be more conservative to avoid starving SDIO traffic.
 */
constexpr size_t HTTP_INTERNAL_FALLBACK_MAX_BYTES = 4096;
constexpr size_t HTTP_MIN_AIRCRAFT_DMA_FREE_BYTES = 32 * 1024;
constexpr size_t HTTP_MIN_AIRCRAFT_DMA_LARGEST_BLOCK_BYTES = 20 * 1024;
constexpr size_t HTTP_MIN_BUFFER_DMA_FREE_BYTES = 16 * 1024;
constexpr size_t HTTP_MIN_BUFFER_DMA_LARGEST_BLOCK_BYTES = 8 * 1024;

/* Return whether a URL will require TLS setup. */
bool is_https_url(const char *url)
{
    return url && strncasecmp(url, "https://", 8) == 0;
}

/* Allow tiny responses to fall back to internal RAM when PSRAM is unavailable. */
bool allow_internal_response_fallback(size_t capacity)
{
    return capacity <= HTTP_INTERNAL_FALLBACK_MAX_BYTES;
}

/* Check internal DMA heap headroom before starting a TLS operation. */
bool have_tls_heap_headroom(const char *operation, size_t min_free, size_t min_largest)
{
    size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    size_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    if (free_dma < min_free || largest_dma < min_largest) {
        ESP_LOGW(TAG,
                 "Skipping %s HTTPS fetch: low internal DMA heap free=%u largest=%u min=%u/%u",
                 operation ? operation : "unknown",
                 (unsigned)free_dma, (unsigned)largest_dma,
                 (unsigned)min_free, (unsigned)min_largest);
        return false;
    }

    return true;
}

/* Check the stricter heap threshold used before aircraft feed fetches. */
bool have_aircraft_tls_heap_headroom()
{
    return have_tls_heap_headroom("aircraft",
                                  HTTP_MIN_AIRCRAFT_DMA_FREE_BYTES,
                                  HTTP_MIN_AIRCRAFT_DMA_LARGEST_BLOCK_BYTES);
}

/* Check the smaller heap threshold used before optional bounded buffer fetches. */
bool have_buffer_tls_heap_headroom()
{
    return have_tls_heap_headroom("buffer",
                                  HTTP_MIN_BUFFER_DMA_FREE_BYTES,
                                  HTTP_MIN_BUFFER_DMA_LARGEST_BLOCK_BYTES);
}

/* Copy a URL for logging while hiding query-string API tokens. */
void redact_url_for_log(const char *url, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!url) {
        return;
    }

    const char *query = strchr(url, '?');
    size_t prefix_len = query ? (size_t)(query - url) : strlen(url);
    if (prefix_len >= dst_size) {
        prefix_len = dst_size - 1;
    }
    memcpy(dst, url, prefix_len);
    dst[prefix_len] = '\0';
    if (query && prefix_len + 12 < dst_size) {
        strncat(dst, "?<redacted>", dst_size - strlen(dst) - 1);
    }
}

} // namespace

/* Create the mutex used to serialise TLS operations. */
void RadarHttpClient::init()
{
    if (!http_mutex) {
        http_mutex = xSemaphoreCreateMutex();
    }
}

/* Allocate cJSON memory from PSRAM first. */
void *RadarHttpClient::jsonMalloc(size_t size)
{
    /*
     * cJSON can allocate large parse trees for aircraft and settings data.
     * Prefer PSRAM so internal RAM stays available for WiFi, TLS, and LVGL.
     */
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

/* Free cJSON memory allocated by jsonMalloc. */
void RadarHttpClient::jsonFree(void *ptr)
{
    heap_caps_free(ptr);
}

/* Install cJSON hooks so parse trees prefer PSRAM. */
void RadarHttpClient::initJsonAllocator()
{
    cJSON_Hooks hooks = {};
    hooks.malloc_fn = jsonMalloc;
    hooks.free_fn = jsonFree;
    cJSON_InitHooks(&hooks);
}

/* Allocate an HTTP response buffer with PSRAM-first policy. */
char *RadarHttpClient::allocResponse(size_t capacity)
{
    char *response = (char *)heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response && allow_internal_response_fallback(capacity)) {
        /*
         * Large response buffers must not consume internal RAM. A small fallback
         * is allowed for tiny metadata responses so optional features degrade
         * gracefully when PSRAM is fragmented.
         */
        response = (char *)heap_caps_malloc(capacity, MALLOC_CAP_8BIT);
    }
    return response;
}

/* Resize an HTTP response buffer without using large internal RAM blocks. */
char *RadarHttpClient::resizeResponse(char *response, size_t capacity)
{
    char *resized = (char *)heap_caps_realloc(response, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resized && allow_internal_response_fallback(capacity)) {
        resized = (char *)heap_caps_realloc(response, capacity, MALLOC_CAP_8BIT);
    }
    return resized;
}

/* Return whether one string contains another, ignoring case. */
bool RadarHttpClient::stringContainsCi(const char *haystack, const char *needle)
{
    if (!haystack || !needle || needle[0] == '\0') {
        return false;
    }

    size_t needle_len = strlen(needle);
    for (const char *pos = haystack; *pos; ++pos) {
        size_t i = 0;
        while (i < needle_len &&
               pos[i] &&
               tolower((unsigned char)pos[i]) == tolower((unsigned char)needle[i])) {
            ++i;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

/* Perform a cheap structural check that JSON was not truncated. */
bool RadarHttpClient::jsonLooksComplete(const char *json, int json_len)
{
    int depth = 0;
    bool started = false;
    bool ended = false;
    bool in_string = false;
    bool escaped = false;

    for (int i = 0; i < json_len; ++i) {
        char ch = json[i];

        if (!started) {
            if (isspace((unsigned char)ch)) {
                continue;
            }
            if (ch != '{' && ch != '[') {
                return false;
            }
            started = true;
        } else if (ended) {
            if (!isspace((unsigned char)ch)) {
                return false;
            }
            continue;
        }

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
        } else if (ch == '{' || ch == '[') {
            ++depth;
        } else if (ch == '}' || ch == ']') {
            --depth;
            if (depth < 0) {
                return false;
            }
            if (depth == 0) {
                ended = true;
            }
        }
    }

    return started && ended && depth == 0 && !in_string && !escaped;
}

/* Inflate gzip response data into a growing response buffer. */
esp_err_t RadarHttpClient::appendGzipResponse(z_stream *stream, const char *input, int input_len,
                                              char **json, size_t *capacity, bool *finished)
{
    stream->next_in = (Bytef *)input;
    stream->avail_in = (uInt)input_len;

    while (stream->avail_in > 0 && !*finished) {
        size_t used = (size_t)stream->total_out;
        if (used + 1 >= *capacity) {
            size_t next_capacity = *capacity * 2;
            if (next_capacity > HTTP_RESPONSE_MAX_BYTES) {
                next_capacity = HTTP_RESPONSE_MAX_BYTES;
            }
            if (next_capacity <= *capacity) {
                return ESP_ERR_INVALID_SIZE;
            }

            char *resized = resizeResponse(*json, next_capacity);
            if (!resized) {
                return ESP_ERR_NO_MEM;
            }
            *json = resized;
            *capacity = next_capacity;
        }

        stream->next_out = (Bytef *)(*json + stream->total_out);
        stream->avail_out = (uInt)(*capacity - 1 - (size_t)stream->total_out);

        uLong previous_in = stream->total_in;
        uLong previous_out = stream->total_out;
        int zret = inflate(stream, Z_NO_FLUSH);
        if (zret == Z_STREAM_END) {
            *finished = true;
            break;
        }
        if (zret != Z_OK || (stream->total_in == previous_in && stream->total_out == previous_out)) {
            ESP_LOGW(TAG, "gzip stream failed: ret=%d in=%lu out=%lu",
                     zret, stream->total_in, stream->total_out);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

/* Ensure a fetch context has enough capacity for the next response bytes. */
esp_err_t RadarHttpClient::reserveResponse(http_fetch_context_t *context, size_t required)
{
    if (required <= context->capacity) {
        return ESP_OK;
    }

    size_t max_capacity = context->max_capacity ? context->max_capacity : HTTP_RESPONSE_MAX_BYTES;
    size_t next_capacity = context->capacity ? context->capacity : HTTP_RESPONSE_INITIAL_BYTES;
    while (next_capacity < required) {
        size_t doubled = next_capacity * 2;
        if (doubled <= next_capacity || doubled > max_capacity) {
            next_capacity = max_capacity;
            break;
        }
        next_capacity = doubled;
    }

    if (next_capacity < required) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *resized = resizeResponse(context->response, next_capacity);
    if (!resized) {
        return ESP_ERR_NO_MEM;
    }

    context->response = resized;
    context->capacity = next_capacity;
    return ESP_OK;
}

/* Append plain, uncompressed HTTP response bytes. */
esp_err_t RadarHttpClient::appendPlainResponse(http_fetch_context_t *context,
                                               const char *data, int data_len)
{
    if (data_len <= 0) {
        return ESP_OK;
    }

    size_t required = (size_t)context->length + (size_t)data_len + 1;
    esp_err_t err = reserveResponse(context, required);
    if (err != ESP_OK) {
        return err;
    }

    memcpy(context->response + context->length, data, (size_t)data_len);
    context->length += data_len;
    context->response[context->length] = '\0';
    return ESP_OK;
}

/* Append HTTP response bytes, inflating gzip content when required. */
esp_err_t RadarHttpClient::appendFetchResponse(http_fetch_context_t *context,
                                               const char *data, int data_len)
{
    if (!context || !data || data_len <= 0) {
        return ESP_OK;
    }

    if (!context->gzip_encoded) {
        return appendPlainResponse(context, data, data_len);
    }

    if (!context->gzip_started) {
        int zret = inflateInit2(&context->gzip_stream, 16 + MAX_WBITS);
        if (zret != Z_OK) {
            ESP_LOGW(TAG, "gzip init failed: %d", zret);
            return ESP_FAIL;
        }
        context->gzip_started = true;
    }

    context->compressed_length += data_len;
    esp_err_t err = appendGzipResponse(&context->gzip_stream, data, data_len,
                                       &context->response, &context->capacity,
                                       &context->gzip_finished);
    context->length = (int)context->gzip_stream.total_out;
    if (context->length >= 0 && (size_t)context->length < context->capacity) {
        context->response[context->length] = '\0';
    }
    return err;
}

/* ESP HTTP callback that records headers and appends streamed response chunks. */
esp_err_t RadarHttpClient::eventHandler(esp_http_client_event_t *evt)
{
    http_fetch_context_t *context = (http_fetch_context_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_HEADER &&
        context &&
        evt->header_key &&
        evt->header_value &&
        strcasecmp(evt->header_key, "Content-Encoding") == 0) {
        context->gzip_encoded = stringContainsCi(evt->header_value, "gzip");
        return ESP_OK;
    }

    if (evt->event_id != HTTP_EVENT_ON_DATA || !context) {
        return ESP_OK;
    }

    esp_err_t err = appendFetchResponse(context, (const char *)evt->data, evt->data_len);
    if (err != ESP_OK) {
        context->err = err;
        return ESP_FAIL;
    }

    taskYIELD();
    return ESP_OK;
}

/* Read a required numeric field from a JSON object. */
bool RadarHttpClient::jsonGetNumber(cJSON *object, const char *name, double *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *value = item->valuedouble;
    return true;
}

/* Read a numeric field that may arrive as either a JSON number or string. */
bool RadarHttpClient::jsonGetDouble(cJSON *object, const char *name, double *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsNumber(item)) {
        *value = item->valuedouble;
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        char *end = nullptr;
        double parsed = strtod(item->valuestring, &end);
        if (end != item->valuestring) {
            *value = parsed;
            return true;
        }
    }
    return false;
}

/* Copy a JSON string field after trimming surrounding whitespace. */
void RadarHttpClient::copyTrimmedField(char *dst, size_t dst_size, cJSON *object, const char *name)
{
    dst[0] = '\0';
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return;
    }

    const char *start = item->valuestring;
    while (*start && isspace((unsigned char)*start)) {
        ++start;
    }
    const char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) {
        --end;
    }

    size_t len = (size_t)(end - start);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, start, len);
    dst[len] = '\0';
}

/* Normalise an ICAO airport code to four uppercase alphanumeric characters. */
bool RadarHttpClient::normalizeIcaoCode(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size < 5) {
        return false;
    }
    dst[0] = '\0';
    if (!src) {
        return false;
    }

    size_t out = 0;
    for (const char *p = src; *p && out < 4; ++p) {
        if (isalnum((unsigned char)*p)) {
            dst[out++] = (char)toupper((unsigned char)*p);
        }
    }
    dst[out] = '\0';
    return out == 4;
}

/* Create a configured HTTP client for aircraft feed requests. */
esp_http_client_handle_t RadarHttpClient::createAircraftClient()
{
    esp_http_client_config_t config = {};
    config.url = "https://api.airplanes.live/";
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = HTTP_READ_CHUNK_BYTES;
    config.buffer_size_tx = 1024;
    config.user_agent = "esp32-p4-radar/1.0";
    config.event_handler = eventHandler;
    config.max_redirection_count = 2;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 30;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return NULL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "gzip");
    esp_http_client_set_header(client, "Connection", "keep-alive");
    return client;
}

/* Fetch and validate one aircraft JSON document using an existing client. */
esp_err_t RadarHttpClient::fetchAircraftJson(esp_http_client_handle_t client, const char *url,
                                             char **response_out, int *response_len_out)
{
    if (!client || !url) {
        return ESP_FAIL;
    }

    if (!have_aircraft_tls_heap_headroom()) {
        return ESP_ERR_NO_MEM;
    }

    char *response = allocResponse(HTTP_RESPONSE_INITIAL_BYTES);
    if (!response) {
        return ESP_ERR_NO_MEM;
    }
    response[0] = '\0';

    http_fetch_context_t context = {};
    context.response = response;
    context.capacity = HTTP_RESPONSE_INITIAL_BYTES;
    context.max_capacity = HTTP_RESPONSE_MAX_BYTES;
    context.err = ESP_OK;

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_user_data(client, &context));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_method(client, HTTP_METHOD_GET));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_url(client, url));

    if (http_mutex && xSemaphoreTake(http_mutex, pdMS_TO_TICKS(HTTP_TLS_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        heap_caps_free(response);
        ESP_LOGW(TAG, "HTTP TLS mutex timeout before aircraft fetch");
        return ESP_ERR_TIMEOUT;
    }

    if (!have_aircraft_tls_heap_headroom()) {
        if (http_mutex) {
            xSemaphoreGive(http_mutex);
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_user_data(client, NULL));
        heap_caps_free(response);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    int64_t content_length = esp_http_client_get_content_length(client);

    if (context.gzip_started) {
        inflateEnd(&context.gzip_stream);
    }
    bool complete = esp_http_client_is_complete_data_received(client);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_user_data(client, NULL));
    if (http_mutex) {
        xSemaphoreGive(http_mutex);
    }

    if (context.err != ESP_OK) {
        err = context.err;
    }

    if (content_length > HTTP_RESPONSE_MAX_BYTES) {
        err = ESP_ERR_INVALID_SIZE;
    }

    bool json_complete = jsonLooksComplete(context.response, context.length);
    if (err != ESP_OK || status_code != 200 || context.length <= 0 || !json_complete ||
        (context.gzip_encoded && !context.gzip_finished) ||
        (size_t)context.length > HTTP_RESPONSE_MAX_BYTES) {
        ESP_LOGW(TAG,
                 "HTTP fetch failed: err=%s status=%d content=%lld bytes=%d compressed=%d complete=%d gzip=%d/%d json_complete=%d errno=%d",
                 esp_err_to_name(err), status_code, (long long)content_length,
                 context.length, context.compressed_length, complete,
                 context.gzip_encoded, context.gzip_finished, json_complete, errno);
        heap_caps_free(context.response);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    char *shrunk = resizeResponse(context.response, (size_t)context.length + 1);
    if (shrunk) {
        context.response = shrunk;
    }

    ESP_LOGD(TAG, "HTTP JSON: %d bytes status=%d content=%lld compressed=%d complete=%d gzip=%d",
             context.length, status_code, (long long)content_length,
             context.compressed_length, complete, context.gzip_encoded);
    *response_out = context.response;
    *response_len_out = context.length;
    return ESP_OK;
}

/* Apply browser-like headers required by public photo and lookup APIs. */
void RadarHttpClient::setBrowserHeaders(esp_http_client_handle_t client,
                                        const char *accept, const char *origin)
{
    if (!client) {
        return;
    }

    const char *header_origin = origin && origin[0] ? origin : "http://" WIFI_SETUP_AP_IP;
    char referer[56];
    snprintf(referer, sizeof(referer), "%s/", header_origin);

    esp_http_client_set_header(client, "Accept",
                               accept && accept[0] != '\0' ?
                               accept : "application/json, text/javascript, */*; q=0.01");
    esp_http_client_set_header(client, "User-Agent", PHOTO_USER_AGENT);
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Accept-Language", "en-GB,en;q=0.9");
    esp_http_client_set_header(client, "Cache-Control", "no-cache");
    esp_http_client_set_header(client, "Pragma", "no-cache");
    esp_http_client_set_header(client, "Origin", header_origin);
    esp_http_client_set_header(client, "Referer", referer);
    esp_http_client_set_header(client, "Sec-Fetch-Dest", "empty");
    esp_http_client_set_header(client, "Sec-Fetch-Mode", "cors");
    esp_http_client_set_header(client, "Sec-Fetch-Site", "cross-site");
    esp_http_client_set_header(client, "Connection", "close");
}

/* Fetch a bounded generic HTTP response with TLS serialisation and heap guards. */
esp_err_t RadarHttpClient::fetchBuffer(const char *url, const char *accept, const char *origin,
                                       size_t initial_capacity, size_t max_bytes,
                                       char **response_out, int *response_len_out,
                                       int mutex_timeout_ms, int http_timeout_ms)
{
    if (!url || !response_out || !response_len_out || max_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (initial_capacity == 0 || initial_capacity > max_bytes) {
        initial_capacity = max_bytes;
    }

    bool https = is_https_url(url);
    if (https && !have_buffer_tls_heap_headroom()) {
        return ESP_ERR_NO_MEM;
    }

    char *response = allocResponse(initial_capacity);
    if (!response) {
        return ESP_ERR_NO_MEM;
    }
    response[0] = '\0';

    http_fetch_context_t context = {};
    context.response = response;
    context.capacity = initial_capacity;
    context.max_capacity = max_bytes;
    context.err = ESP_OK;

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = http_timeout_ms > 0 ? http_timeout_ms : PHOTO_HTTP_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = HTTP_READ_CHUNK_BYTES;
    config.buffer_size_tx = 1024;
    config.user_agent = PHOTO_USER_AGENT;
    config.event_handler = eventHandler;
    config.user_data = &context;
    config.max_redirection_count = 3;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        heap_caps_free(context.response);
        return ESP_ERR_NO_MEM;
    }

    setBrowserHeaders(client, accept, origin);

    bool mutex_taken = false;
    TickType_t wait_ticks = mutex_timeout_ms <= 0 ? 0 : pdMS_TO_TICKS(mutex_timeout_ms);
    if (https && http_mutex && xSemaphoreTake(http_mutex, wait_ticks) != pdTRUE) {
        esp_http_client_cleanup(client);
        heap_caps_free(context.response);
        char log_url[192];
        redact_url_for_log(url, log_url, sizeof(log_url));
        ESP_LOGW(TAG, "HTTP TLS mutex timeout before buffer fetch: %s", log_url);
        return ESP_ERR_TIMEOUT;
    }
    mutex_taken = https && http_mutex;

    if (https && !have_buffer_tls_heap_headroom()) {
        esp_http_client_cleanup(client);
        if (mutex_taken) {
            xSemaphoreGive(http_mutex);
        }
        heap_caps_free(context.response);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    int64_t content_length = esp_http_client_get_content_length(client);

    if (context.gzip_started) {
        inflateEnd(&context.gzip_stream);
    }
    esp_http_client_cleanup(client);
    if (mutex_taken) {
        xSemaphoreGive(http_mutex);
    }

    if (context.err != ESP_OK) {
        err = context.err;
    }

    if (content_length > (int64_t)max_bytes) {
        err = ESP_ERR_INVALID_SIZE;
    }

    if (err != ESP_OK || status_code != 200 || context.length <= 0 ||
        (size_t)context.length > max_bytes) {
        char log_url[192];
        redact_url_for_log(url, log_url, sizeof(log_url));
        ESP_LOGW(TAG, "HTTP buffer fetch failed: url=%s err=%s status=%d content=%lld bytes=%d",
                 log_url, esp_err_to_name(err), status_code, (long long)content_length,
                 context.length);
        heap_caps_free(context.response);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    *response_out = context.response;
    *response_len_out = context.length;
    return ESP_OK;
}
