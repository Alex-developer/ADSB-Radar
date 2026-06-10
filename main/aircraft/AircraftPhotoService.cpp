#include "AircraftPhotoService.hpp"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "misc/cache/instance/lv_image_cache.h"

#include "bsp/display.h"
#include "PhotoDecoder.hpp"
#include "RadarApp.hpp"
#include "RadarTypes.hpp"

static const char *TAG = "aircraft_photo";

/* Convert fetch failures into short messages that fit the aircraft popup. */
static const char *photoFetchStatus(esp_err_t err, const char *fallback)
{
    if (err == ESP_ERR_NO_MEM) {
        return "PHOTO MEM LOW";
    }
    if (err == ESP_ERR_TIMEOUT) {
        return "PHOTO BUSY";
    }
    return fallback;
}

/* Prefer the CDN's plain HTTP thumbnail path to avoid a second TLS handshake. */
static void preferPlainHttpThumbnail(char *url, size_t url_size)
{
    const char *https_prefix = "https://t.plnspttrs.net/";
    const char *http_prefix = "http://t.plnspttrs.net/";
    size_t https_prefix_len = strlen(https_prefix);

    if (!url || url_size == 0 || strncmp(url, https_prefix, https_prefix_len) != 0) {
        return;
    }

    char rewritten[256];
    snprintf(rewritten, sizeof(rewritten), "%s%s", http_prefix, url + https_prefix_len);
    snprintf(url, url_size, "%s", rewritten);
}

/* Fetch a bounded buffer, retrying short-lived low-memory TLS failures. */
esp_err_t AircraftPhotoService::fetchBufferWithRetry(const char *url,
                                                     const char *accept,
                                                     size_t initial_capacity,
                                                     size_t max_bytes,
                                                     char **response_out,
                                                     int *response_len_out)
{
    constexpr int max_attempts = 30;
    esp_err_t err = ESP_FAIL;

    /*
     * Aircraft JSON fetches and photo fetches share the same TLS stack. Photos
     * are optional, so they must not queue behind a feed download and then add
     * more TLS pressure. The zero mutex wait makes a photo attempt step aside
     * immediately if another HTTPS operation is active; short retries let it
     * catch an idle slot without blocking the aircraft data path. The retry
     * delay is deliberately small because the payloads are tiny and most
     * visible delay is waiting for an HTTPS slot, not transferring bytes.
     */
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        err = owner->fetch_http_buffer(url, accept, initial_capacity, max_bytes,
                                       response_out, response_len_out,
                                       PHOTO_OPTIONAL_TLS_MUTEX_TIMEOUT_MS,
                                       PHOTO_OPTIONAL_HTTP_TIMEOUT_MS);
        if (err != ESP_ERR_NO_MEM && err != ESP_ERR_TIMEOUT) {
            return err;
        }

        ESP_LOGW(TAG, "Photo fetch deferred (%s), retry %d/%d",
                 esp_err_to_name(err), attempt, max_attempts);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return err;
}

/* Extract the first thumbnail URL from a PlaneSpotters photo metadata response. */
bool AircraftPhotoService::parseThumbnailUrl(const char *json, int json_len, char *url, size_t url_size)
{
    if (!json || json_len <= 0 || !url || url_size == 0) {
        return false;
    }
    url[0] = '\0';

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root) {
        return false;
    }

    bool found = false;
    cJSON *photos = cJSON_GetObjectItemCaseSensitive(root, "photos");
    if (cJSON_IsArray(photos)) {
        cJSON *photo = cJSON_GetArrayItem(photos, 0);
        cJSON *thumbnail = cJSON_IsObject(photo) ?
                           cJSON_GetObjectItemCaseSensitive(photo, "thumbnail") : NULL;
        cJSON *src = cJSON_IsObject(thumbnail) ?
                     cJSON_GetObjectItemCaseSensitive(thumbnail, "src") : NULL;
        if (cJSON_IsString(src) && src->valuestring &&
            (strncmp(src->valuestring, "https://", 8) == 0 ||
             strncmp(src->valuestring, "http://", 7) == 0)) {
            snprintf(url, url_size, "%s", src->valuestring);
            found = true;
        }
    }

    cJSON_Delete(root);
    return found;
}

/* Normalise an aircraft ICAO hex value for use in a photo API URL. */
bool AircraftPhotoService::normalizeIcaoHex(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return false;
    }
    dst[0] = '\0';
    if (!src) {
        return false;
    }

    size_t out = 0;
    for (const char *p = src; *p && out + 1 < dst_size; ++p) {
        if (isxdigit((unsigned char)*p)) {
            dst[out++] = (char)toupper((unsigned char)*p);
        }
    }
    dst[out] = '\0';
    return out > 0;
}

/* Return whether a worker result still belongs to the currently visible popup. */
bool AircraftPhotoService::requestIsCurrent(uint32_t request_id) const
{
    return owner &&
           request_id == owner->aircraft_photo_request_id &&
           owner->aircraft_popup &&
           !lv_obj_has_flag(owner->aircraft_popup, LV_OBJ_FLAG_HIDDEN);
}

/* Update popup photo status from the worker task under the LVGL display lock. */
void AircraftPhotoService::updateStatusFromTask(uint32_t request_id, const char *status)
{
    if (!owner || bsp_display_lock(1000) != ESP_OK) {
        return;
    }

    if (requestIsCurrent(request_id)) {
        owner->set_aircraft_photo_status_text(status);
        if (owner->aircraft_photo_image) {
            lv_obj_add_flag(owner->aircraft_photo_image, LV_OBJ_FLAG_HIDDEN);
        }
    }

    bsp_display_unlock();
}

/* Install decoded image pixels into the popup if the request is still current. */
void AircraftPhotoService::installFromTask(uint32_t request_id,
                                           const lv_image_dsc_t *decoded_image,
                                           uint8_t *pixels, size_t pixels_size)
{
    bool installed = false;

    /*
     * LVGL objects must be touched under the display lock. If the popup has
     * moved on to another aircraft, the decoded pixels are freed below instead
     * of being installed into a stale image descriptor.
     */
    if (owner && decoded_image && pixels && pixels_size > 0 && bsp_display_lock(1000) == ESP_OK) {
        if (requestIsCurrent(request_id) && owner->aircraft_photo_image) {
            owner->release_aircraft_photo_image();

            owner->aircraft_photo_pixels = pixels;
            owner->aircraft_photo_pixel_size = pixels_size;
            owner->aircraft_photo_dsc = *decoded_image;
            owner->aircraft_photo_dsc.data = owner->aircraft_photo_pixels;

            lv_image_cache_drop(&owner->aircraft_photo_dsc);
            lv_image_set_src(owner->aircraft_photo_image, &owner->aircraft_photo_dsc);
            lv_obj_clear_flag(owner->aircraft_photo_image, LV_OBJ_FLAG_HIDDEN);
            owner->set_aircraft_photo_status_text("");
            ESP_LOGI(TAG, "Installed aircraft photo: %ux%u %u bytes",
                     (unsigned)owner->aircraft_photo_dsc.header.w,
                     (unsigned)owner->aircraft_photo_dsc.header.h,
                     (unsigned)owner->aircraft_photo_dsc.data_size);
            installed = true;
        }

        bsp_display_unlock();
    }

    if (!installed && pixels) {
        heap_caps_free(pixels);
    }
}

/* Fetch photo metadata, download the thumbnail, decode it, and update the popup. */
void AircraftPhotoService::fetchTask(void *arg)
{
    photo_fetch_request_t *request = (photo_fetch_request_t *)arg;
    if (!owner || !request) {
        if (owner) {
            owner->aircraft_photo_fetch_running = false;
        }
        vTaskDeleteWithCaps(NULL);
        return;
    }

    char metadata_url[96];
    snprintf(metadata_url, sizeof(metadata_url),
             "https://api.planespotters.net/pub/photos/hex/%s", request->icao);

    char *json = NULL;
    int json_len = 0;
    char thumbnail_url[256];
    char *jpeg = NULL;
    int jpeg_len = 0;
    lv_image_dsc_t decoded_image = {};
    uint8_t *pixels = NULL;
    size_t pixels_size = 0;
    esp_err_t err = fetchBufferWithRetry(metadata_url,
                                         "application/json, text/javascript, */*; q=0.01",
                                         PHOTO_JSON_INITIAL_BYTES, PHOTO_JSON_MAX_BYTES,
                                         &json, &json_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Photo metadata fetch failed for %s: %s",
                 request->icao, esp_err_to_name(err));
        updateStatusFromTask(request->request_id, photoFetchStatus(err, "PHOTO API ERR"));
        goto done;
    }

    if (!parseThumbnailUrl(json, json_len, thumbnail_url, sizeof(thumbnail_url))) {
        heap_caps_free(json);
        updateStatusFromTask(request->request_id, "PHOTO NONE");
        goto done;
    }
    heap_caps_free(json);
    preferPlainHttpThumbnail(thumbnail_url, sizeof(thumbnail_url));

    err = fetchBufferWithRetry(thumbnail_url, "image/jpeg",
                               PHOTO_JPEG_INITIAL_BYTES, PHOTO_JPEG_MAX_BYTES,
                               &jpeg, &jpeg_len);
    if (err != ESP_OK || jpeg_len < 2 ||
        (uint8_t)jpeg[0] != 0xff || (uint8_t)jpeg[1] != 0xd8) {
        ESP_LOGW(TAG, "Photo JPEG fetch failed for %s: %s bytes=%d",
                 request->icao, esp_err_to_name(err), jpeg_len);
        if (jpeg) {
            heap_caps_free(jpeg);
        }
        updateStatusFromTask(request->request_id, photoFetchStatus(err, "PHOTO JPG ERR"));
        goto done;
    }

    if (!PhotoDecoder::decodeJpegToRgb565((const uint8_t *)jpeg, (size_t)jpeg_len,
                                          &decoded_image, &pixels, &pixels_size)) {
        heap_caps_free(jpeg);
        updateStatusFromTask(request->request_id, "PHOTO IMG ERR");
        goto done;
    }
    heap_caps_free(jpeg);

    installFromTask(request->request_id, &decoded_image, pixels, pixels_size);

done:
    heap_caps_free(request);
    owner->aircraft_photo_fetch_running = false;
    vTaskDeleteWithCaps(NULL);
}

/* Queue a new photo fetch task for one aircraft ICAO hex address. */
void AircraftPhotoService::startFetch(const char *icao)
{
    if (!owner) {
        return;
    }

    char normalized_icao[8];
    owner->aircraft_photo_request_id++;

    if (!normalizeIcaoHex(normalized_icao, sizeof(normalized_icao), icao)) {
        owner->set_aircraft_photo_status_text("PHOTO NO ICAO");
        return;
    }
    if (owner->aircraft_photo_fetch_running) {
        owner->set_aircraft_photo_status_text("PHOTO BUSY");
        return;
    }

    photo_fetch_request_t *request = (photo_fetch_request_t *)heap_caps_calloc(1, sizeof(*request),
                                                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!request) {
        request = (photo_fetch_request_t *)heap_caps_calloc(1, sizeof(*request), MALLOC_CAP_8BIT);
    }
    if (!request) {
        owner->set_aircraft_photo_status_text("PHOTO NO MEM");
        return;
    }

    snprintf(request->icao, sizeof(request->icao), "%s", normalized_icao);
    request->request_id = owner->aircraft_photo_request_id;
    owner->aircraft_photo_fetch_running = true;
    owner->set_aircraft_photo_status_text("PHOTO FETCH");

    if (xTaskCreateWithCaps(RadarApp::aircraft_photo_fetch_task_entry, "aircraft_photo",
                            PHOTO_TASK_STACK, request, PHOTO_TASK_PRIORITY, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        owner->aircraft_photo_fetch_running = false;
        heap_caps_free(request);
        owner->set_aircraft_photo_status_text("PHOTO ERR");
    }
}
