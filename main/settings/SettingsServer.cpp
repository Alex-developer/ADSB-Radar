#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>

#include "cJSON.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "png.h"
#include "src/draw/snapshot/lv_snapshot.h"

#include "airport_data.h"
#include "country_boundary_data.h"
#include "SettingsServer.hpp"
#include "SettingsPageHtml.hpp"
#include "RadarApp.hpp"

static const char *TAG = "settings_server";

/*
 * The settings server is the bridge between the browser UI and the embedded
 * runtime. Handlers should keep allocations bounded, return compact JSON, and
 * apply settings through RadarApp so the live radar redraws immediately.
 */

namespace {

/* Percent-encode one URL component into a caller-supplied buffer. */
bool url_encode_component(const char *src, char *dst, size_t dst_size)
{
    if (!src || !dst || dst_size == 0) {
        return false;
    }

    size_t out = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; ++p) {
        bool safe = isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        if (safe) {
            if (out + 1 >= dst_size) {
                return false;
            }
            dst[out++] = (char)*p;
        } else {
            if (out + 3 >= dst_size) {
                return false;
            }
            snprintf(dst + out, dst_size - out, "%%%02X", *p);
            out += 3;
        }
    }
    dst[out] = '\0';
    return true;
}

/* Add one OpenWeather geocoder result to the compact settings response. */
void add_location_result(cJSON *items, cJSON *source)
{
    /*
     * OpenWeather's geocoder returns many localised names. The radar only needs
     * a concise display name plus coordinates, country, and optional state.
     */
    if (!items || !cJSON_IsObject(source)) {
        return;
    }

    cJSON *lat = cJSON_GetObjectItemCaseSensitive(source, "lat");
    cJSON *lon = cJSON_GetObjectItemCaseSensitive(source, "lon");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(source, "name");
    if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon) || !cJSON_IsString(name) || !name->valuestring) {
        return;
    }

    cJSON *item = cJSON_CreateObject();
    if (!item) {
        return;
    }

    cJSON *country = cJSON_GetObjectItemCaseSensitive(source, "country");
    cJSON *state = cJSON_GetObjectItemCaseSensitive(source, "state");
    cJSON_AddStringToObject(item, "name", name->valuestring);
    cJSON_AddStringToObject(item, "country",
                            cJSON_IsString(country) && country->valuestring ? country->valuestring : "");
    cJSON_AddStringToObject(item, "state",
                            cJSON_IsString(state) && state->valuestring ? state->valuestring : "");
    cJSON_AddNumberToObject(item, "lat", lat->valuedouble);
    cJSON_AddNumberToObject(item, "lon", lon->valuedouble);
    cJSON_AddItemToArray(items, item);
}

/* Add heap usage metrics for one ESP-IDF memory capability mask. */
void add_heap_stats(cJSON *parent, const char *name, uint32_t caps)
{
    cJSON *heap = cJSON_AddObjectToObject(parent, name);
    size_t total = heap_caps_get_total_size(caps);
    size_t free_bytes = heap_caps_get_free_size(caps);
    size_t min_free = heap_caps_get_minimum_free_size(caps);
    size_t largest = heap_caps_get_largest_free_block(caps);
    cJSON_AddNumberToObject(heap, "total", (double)total);
    cJSON_AddNumberToObject(heap, "free", (double)free_bytes);
    cJSON_AddNumberToObject(heap, "used", total > free_bytes ? (double)(total - free_bytes) : 0);
    cJSON_AddNumberToObject(heap, "minFree", (double)min_free);
    cJSON_AddNumberToObject(heap, "largestFree", (double)largest);
}

/* Add one cache or buffer row to the status response. */
void add_cache_item(cJSON *array, const char *name, size_t bytes, bool active)
{
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        return;
    }
    cJSON_AddStringToObject(item, "name", name);
    cJSON_AddNumberToObject(item, "bytes", (double)bytes);
    cJSON_AddBoolToObject(item, "active", active);
    cJSON_AddItemToArray(array, item);
}

/* Convert the aircraft filter enum into status-page text. */
const char *aircraft_filter_label(aircraft_filter_t filter)
{
    switch (filter) {
    case AIRCRAFT_FILTER_MILITARY:
        return "Military aircraft";
    case AIRCRAFT_FILTER_INTERESTING:
        return "Interesting aircraft";
    case AIRCRAFT_FILTER_ALL:
    default:
        return "All aircraft";
    }
}

/* Convert the configured aircraft data source into status-page text. */
const char *aircraft_source_label(int source)
{
    switch (source) {
    case AIRCRAFT_DATA_SOURCE_ADSB_LOL:
        return "ADSB.lol";
    case AIRCRAFT_DATA_SOURCE_ADSB_FI:
        return "ADSB.fi";
    case AIRCRAFT_DATA_SOURCE_LOCAL:
        return "Local aircraft.json";
    case AIRCRAFT_DATA_SOURCE_AIRPLANES_LIVE:
    default:
        return "Airplanes.live";
    }
}

typedef struct {
    httpd_req_t *req;
    esp_err_t err;
} png_stream_ctx_t;

/* Stream encoded PNG bytes straight to the HTTP response. */
void png_http_write(png_structp png_ptr, png_bytep data, png_size_t length)
{
    png_stream_ctx_t *ctx = (png_stream_ctx_t *)png_get_io_ptr(png_ptr);
    if (!ctx || ctx->err != ESP_OK) {
        return;
    }
    ctx->err = httpd_resp_send_chunk(ctx->req, (const char *)data, length);
}

/* libpng requires a flush hook even though the HTTP server is chunk based. */
void png_http_flush(png_structp png_ptr)
{
    (void)png_ptr;
}

/* Return true when a pixel is inside the physical round LCD area. */
bool pixel_inside_round_display(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    const int32_t centre_x = (int32_t)(width - 1U) / 2;
    const int32_t centre_y = (int32_t)(height - 1U) / 2;
    const int32_t radius = (int32_t)((width < height ? width : height) - 1U) / 2;
    const int32_t dx = (int32_t)x - centre_x;
    const int32_t dy = (int32_t)y - centre_y;
    return ((int64_t)dx * dx + (int64_t)dy * dy) <= (int64_t)radius * radius;
}

/* Copy one LVGL pixel into a PNG RGBA row, converting from LVGL's colour layout. */
bool copy_png_pixel_from_draw_buf(uint8_t *dst, const uint8_t *src, lv_color_format_t color_format)
{
    if (color_format == LV_COLOR_FORMAT_RGB565) {
        lv_color16_t colour = *(const lv_color16_t *)src;
        lv_color_t rgb = lv_color16_to_color(colour);
        dst[0] = rgb.red;
        dst[1] = rgb.green;
        dst[2] = rgb.blue;
        return true;
    }
    if (color_format == LV_COLOR_FORMAT_RGB888) {
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        return true;
    }
    return false;
}

/* Stream one LVGL draw buffer as a circular PNG with transparent corners. */
esp_err_t stream_round_png(httpd_req_t *req, const lv_draw_buf_t *draw_buf)
{
    if (!req || !draw_buf || !draw_buf->data ||
        (draw_buf->header.cf != LV_COLOR_FORMAT_RGB565 &&
         draw_buf->header.cf != LV_COLOR_FORMAT_RGB888) ||
        draw_buf->header.w == 0 || draw_buf->header.h == 0) {
        return ESP_FAIL;
    }

    const uint32_t width = draw_buf->header.w;
    const uint32_t height = draw_buf->header.h;
    const lv_color_format_t color_format = (lv_color_format_t)draw_buf->header.cf;
    const uint32_t source_stride = draw_buf->header.stride;
    const uint8_t source_pixel_bytes = LV_COLOR_FORMAT_GET_SIZE(color_format);
    const uint32_t png_row_bytes = width * 4U;

    uint8_t *row = (uint8_t *)heap_caps_malloc(png_row_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!row) {
        row = (uint8_t *)heap_caps_malloc(png_row_bytes, MALLOC_CAP_8BIT);
    }
    if (!row) {
        return ESP_ERR_NO_MEM;
    }

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        heap_caps_free(row);
        return ESP_ERR_NO_MEM;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        heap_caps_free(row);
        return ESP_ERR_NO_MEM;
    }

    png_stream_ctx_t stream = {
        .req = req,
        .err = ESP_OK,
    };

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"adsb-radar-screenshot.png\"");
    httpd_resp_set_type(req, "image/png");

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        heap_caps_free(row);
        return stream.err == ESP_OK ? ESP_FAIL : stream.err;
    }

    png_set_write_fn(png_ptr, &stream, png_http_write, png_http_flush);
    png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);

    for (uint32_t y = 0; stream.err == ESP_OK && y < height; ++y) {
        const uint8_t *src = draw_buf->data + (y * source_stride);
        for (uint32_t x = 0; x < width; ++x) {
            if (!copy_png_pixel_from_draw_buf(&row[x * 4U], &src[x * source_pixel_bytes], color_format)) {
                stream.err = ESP_FAIL;
                break;
            }
            row[(x * 4U) + 3] = pixel_inside_round_display(x, y, width, height) ? 255 : 0;
        }
        if (stream.err == ESP_OK) {
            png_write_row(png_ptr, row);
        }
    }
    if (stream.err == ESP_OK) {
        png_write_end(png_ptr, info_ptr);
    }
    png_destroy_write_struct(&png_ptr, &info_ptr);
    heap_caps_free(row);
    if (stream.err == ESP_OK) {
        stream.err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return stream.err;
}

} // namespace

/* Send the small JSON acknowledgement used by AJAX write endpoints. */
esp_err_t SettingsServer::sendJsonStatus(httpd_req_t *req, bool ok, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = json ? httpd_resp_sendstr(req, json) :
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    if (json) {
        cJSON_free(json);
    }
    return err;
}

/* Send saved settings enriched with runtime GPS and effective centre data. */
esp_err_t SettingsServer::sendSettingsJson(httpd_req_t *req)
{
    if (!owner) {
        return ESP_FAIL;
    }

    char *json = owner->settings.toJson(owner->wifi_ssid);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    cJSON *root = cJSON_Parse(json);
    cJSON_free(json);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON build failed");
    }

    gps_snapshot_t gps_snapshot = {};
    owner->gps.getSnapshot(&gps_snapshot);
    bool using_gps = false;
    double effective_lat = owner->settings.center_lat;
    double effective_lon = owner->settings.center_lon;
    owner->get_radar_center(&effective_lat, &effective_lon, &using_gps);

    cJSON *gps_json = cJSON_AddObjectToObject(root, "gps");
    cJSON_AddBoolToObject(gps_json, "selected", owner->settings.center_source == RADAR_CENTER_SOURCE_GPS);
    cJSON_AddBoolToObject(gps_json, "usingFix", using_gps);
    cJSON_AddBoolToObject(gps_json, "deviceConnected", gps_snapshot.device_connected);
    cJSON_AddBoolToObject(gps_json, "receiving", gps_snapshot.receiving);
    cJSON_AddBoolToObject(gps_json, "hasFix", gps_snapshot.has_fix && !gps_snapshot.fix_stale);
    cJSON_AddStringToObject(gps_json, "status", gps_snapshot.status);
    cJSON_AddStringToObject(gps_json, "detail", gps_snapshot.detail);
    cJSON_AddNumberToObject(gps_json, "lat", gps_snapshot.lat);
    cJSON_AddNumberToObject(gps_json, "lon", gps_snapshot.lon);
    cJSON_AddNumberToObject(gps_json, "satellites", gps_snapshot.satellites);
    cJSON_AddNumberToObject(gps_json, "sentences", gps_snapshot.sentence_count);
    cJSON_AddNumberToObject(gps_json, "effectiveLat", effective_lat);
    cJSON_AddNumberToObject(gps_json, "effectiveLon", effective_lon);

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

/* Send factory defaults without changing the current running settings. */
esp_err_t SettingsServer::sendDefaultsJson(httpd_req_t *req)
{
    if (!owner) {
        return ESP_FAIL;
    }

    RadarSettings defaults;
    char *json = defaults.toJson(owner->wifi_ssid);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

/* Serve the Bootstrap settings application from flash. */
esp_err_t SettingsServer::pageHandler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, SETTINGS_PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

/* Handle GET /api/settings by returning the live settings document. */
esp_err_t SettingsServer::apiGetHandler(httpd_req_t *req)
{
    return sendSettingsJson(req);
}

/* Validate, apply, and persist a submitted settings document. */
esp_err_t SettingsServer::apiSaveHandler(httpd_req_t *req)
{
    if (!owner) {
        return ESP_FAIL;
    }

    char *body = RadarApp::read_request_body(req, 16384);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
    }

    radar_settings_t candidate = {};
    char error[80] = "Invalid settings";
    bool parsed = owner->settings.parseJson(body, &candidate, error, sizeof(error));
    free(body);
    if (!parsed) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
    }

    owner->apply_settings(&candidate);
    esp_err_t err = owner->settings.save();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saving radar settings failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    }

    return sendJsonStatus(req, true, "saved");
}

/* Build the diagnostics JSON used by the settings status page. */
esp_err_t SettingsServer::statusHandler(httpd_req_t *req)
{
    if (!owner) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    int64_t uptime_sec = esp_timer_get_time() / 1000000LL;
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    cJSON_AddNumberToObject(root, "uptimeSec", (double)uptime_sec);
    cJSON_AddStringToObject(root, "idfVersion", esp_get_idf_version());
    cJSON_AddNumberToObject(root, "taskCount", uxTaskGetNumberOfTasks());
    cJSON_AddNumberToObject(root, "chipCores", chip.cores);
    cJSON_AddNumberToObject(root, "chipRevision", chip.revision);

    cJSON *memory = cJSON_AddObjectToObject(root, "memory");
    add_heap_stats(memory, "internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    add_heap_stats(memory, "spiram", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    add_heap_stats(memory, "dma", MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    cJSON_AddNumberToObject(memory, "freeHeap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(memory, "minFreeHeap", (double)esp_get_minimum_free_heap_size());

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    EventBits_t bits = owner->wifi_event_group ? xEventGroupGetBits(owner->wifi_event_group) : 0;
    bool connected = (bits & WIFI_CONNECTED_BIT) != 0;
    bool portal_active = owner->wifi_portal_active || ((bits & WIFI_PORTAL_ACTIVE_BIT) != 0);
    cJSON_AddBoolToObject(wifi, "connected", connected);
    cJSON_AddBoolToObject(wifi, "portalActive", portal_active);
    cJSON_AddBoolToObject(wifi, "started", owner->wifi_started);
    cJSON_AddBoolToObject(wifi, "recovering", owner->wifi_recovering);
    cJSON_AddStringToObject(wifi, "ssid", owner->wifi_ssid);
    char ip_text[24] = "--";
    if (portal_active) {
        snprintf(ip_text, sizeof(ip_text), "%s", WIFI_SETUP_AP_IP);
    } else if (connected && owner->wifi_sta_netif) {
        esp_netif_ip_info_t info = {};
        if (esp_netif_get_ip_info(owner->wifi_sta_netif, &info) == ESP_OK && info.ip.addr != 0) {
            snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&info.ip));
        }
    }
    cJSON_AddStringToObject(wifi, "ip", ip_text);

    cJSON *aircraft = cJSON_AddObjectToObject(root, "aircraft");
    size_t latest_count = owner->latest_aircraft_count;
    uint32_t latest_generation = owner->latest_aircraft_generation;
    char latest_status[sizeof(owner->latest_status)];
    aircraft_data_t *aircraft_snapshot = nullptr;
    size_t aircraft_snapshot_count = 0;
    snprintf(latest_status, sizeof(latest_status), "%s", owner->latest_status);
    if (owner->aircraft_mutex && xSemaphoreTake(owner->aircraft_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        latest_count = owner->latest_aircraft_count;
        latest_generation = owner->latest_aircraft_generation;
        snprintf(latest_status, sizeof(latest_status), "%s", owner->latest_status);
        aircraft_snapshot_count = latest_count < MAX_AIRCRAFT_TARGETS ? latest_count : MAX_AIRCRAFT_TARGETS;
        if (owner->latest_aircraft && aircraft_snapshot_count > 0) {
            size_t snapshot_bytes = aircraft_snapshot_count * sizeof(aircraft_snapshot[0]);
            aircraft_snapshot = (aircraft_data_t *)heap_caps_malloc(snapshot_bytes,
                                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!aircraft_snapshot) {
                aircraft_snapshot = (aircraft_data_t *)malloc(snapshot_bytes);
            }
        }
        if (aircraft_snapshot && aircraft_snapshot_count > 0) {
            memcpy(aircraft_snapshot, owner->latest_aircraft,
                   aircraft_snapshot_count * sizeof(aircraft_snapshot[0]));
        } else {
            aircraft_snapshot_count = 0;
        }
        xSemaphoreGive(owner->aircraft_mutex);
    }
    cJSON_AddStringToObject(aircraft, "status", latest_status);
    cJSON_AddNumberToObject(aircraft, "latestCount", (double)latest_count);
    cJSON_AddNumberToObject(aircraft, "uiCount", (double)owner->ui_aircraft_count);
    cJSON_AddNumberToObject(aircraft, "generation", latest_generation);
    cJSON_AddNumberToObject(aircraft, "rangeMi", owner->get_current_range_mi());
    cJSON_AddStringToObject(aircraft, "filter", aircraft_filter_label(owner->aircraft_filter));
    cJSON_AddStringToObject(aircraft, "source",
                            aircraft_source_label(owner->settings.aircraft_data_source));
    cJSON_AddStringToObject(aircraft, "localUrl", owner->settings.aircraft_local_url);
    cJSON_AddBoolToObject(aircraft, "photoFetchRunning", owner->aircraft_photo_fetch_running);
    cJSON_AddNumberToObject(aircraft, "photoBytes", (double)owner->aircraft_photo_pixel_size);
    cJSON *aircraft_items = cJSON_AddArrayToObject(aircraft, "items");
    int current_range_mi = owner->get_current_range_mi();
    size_t label_limit = owner->get_current_label_limit();
    size_t label_index = 0;
    for (size_t i = 0; i < aircraft_snapshot_count; ++i) {
        const aircraft_data_t *item = &aircraft_snapshot[i];
        bool on_display = item->distance_mi <= (float)current_range_mi &&
                          owner->aircraft_matches_filter(item);
        const notification_setting_t *notification = owner->matching_notification(item);
        bool labelled = false;
        if (on_display && label_index < MAX_AIRCRAFT_LABELS) {
            if (label_index < label_limit || notification) {
                labelled = true;
                ++label_index;
            }
        }

        cJSON *row = cJSON_CreateObject();
        if (!row) {
            continue;
        }
        cJSON_AddStringToObject(row, "callsign", item->callsign);
        cJSON_AddStringToObject(row, "icao", item->icao);
        cJSON_AddStringToObject(row, "registration", item->registration);
        cJSON_AddStringToObject(row, "type", item->type);
        cJSON_AddStringToObject(row, "squawk", item->squawk);
        cJSON_AddNumberToObject(row, "distanceMi", item->distance_mi);
        cJSON_AddNumberToObject(row, "bearingDeg", item->bearing_deg);
        cJSON_AddNumberToObject(row, "headingDeg", item->heading_deg);
        cJSON_AddNumberToObject(row, "altitudeFt", item->altitude_ft);
        cJSON_AddNumberToObject(row, "speedKt", item->speed_kt);
        cJSON_AddNumberToObject(row, "verticalRateFpm", item->vertical_rate_fpm);
        cJSON_AddNumberToObject(row, "seenS", item->seen_s);
        cJSON_AddBoolToObject(row, "hasDbFlags", item->has_db_flags);
        cJSON_AddNumberToObject(row, "dbFlags", item->db_flags);
        cJSON_AddBoolToObject(row, "onDisplay", on_display);
        cJSON_AddBoolToObject(row, "labelled", labelled);
        cJSON_AddItemToArray(aircraft_items, row);
    }
    if (aircraft_snapshot) {
        free(aircraft_snapshot);
    }

    gps_snapshot_t gps_snapshot = {};
    owner->gps.getSnapshot(&gps_snapshot);
    cJSON *gps = cJSON_AddObjectToObject(root, "gps");
    cJSON_AddBoolToObject(gps, "deviceConnected", gps_snapshot.device_connected);
    cJSON_AddBoolToObject(gps, "receiving", gps_snapshot.receiving);
    cJSON_AddBoolToObject(gps, "hasFix", gps_snapshot.has_fix && !gps_snapshot.fix_stale);
    cJSON_AddStringToObject(gps, "status", gps_snapshot.status);
    cJSON_AddStringToObject(gps, "detail", gps_snapshot.detail);
    cJSON_AddNumberToObject(gps, "lat", gps_snapshot.lat);
    cJSON_AddNumberToObject(gps, "lon", gps_snapshot.lon);
    cJSON_AddNumberToObject(gps, "satellites", gps_snapshot.satellites);
    cJSON_AddNumberToObject(gps, "sentences", gps_snapshot.sentence_count);

    cJSON *caches = cJSON_AddArrayToObject(root, "caches");
    add_cache_item(caches, "Radar static canvas",
                   LV_CANVAS_BUF_SIZE(RADAR_SIZE, RADAR_SIZE, 16, LV_DRAW_BUF_STRIDE_ALIGN),
                   owner->radar_canvas_buf != nullptr);
    add_cache_item(caches, "Aircraft overlay canvas",
                   LV_CANVAS_BUF_SIZE(RADAR_SIZE, RADAR_SIZE, 32, LV_DRAW_BUF_STRIDE_ALIGN),
                   owner->aircraft_canvas_buf != nullptr);
    add_cache_item(caches, "Latest aircraft buffer",
                   MAX_AIRCRAFT_TARGETS * sizeof(aircraft_data_t),
                   owner->latest_aircraft != nullptr);
    add_cache_item(caches, "UI aircraft snapshot",
                   MAX_AIRCRAFT_TARGETS * sizeof(aircraft_data_t),
                   owner->ui_aircraft_snapshot != nullptr);
    add_cache_item(caches, "Aircraft photo image",
                   owner->aircraft_photo_pixel_size,
                   owner->aircraft_photo_pixels != nullptr);
    airport_runway_cache_t runway_cache = {};
    bool runway_active = owner->get_active_runway_cache(&runway_cache);
    add_cache_item(caches, "Active runway cache",
                   runway_active ? sizeof(runway_cache) : 0,
                   runway_active);
    add_cache_item(caches, "Airport records",
                   airport_record_count * sizeof(airport_record_t),
                   airport_record_count > 0);
    add_cache_item(caches, "Country boundary points",
                   boundary_point_count * sizeof(boundary_point_t),
                   boundary_point_count > 0);
    add_cache_item(caches, "Country boundary lines",
                   boundary_line_count * sizeof(boundary_line_t),
                   boundary_line_count > 0);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

/* Capture the current LVGL screen and stream it as a circular PNG image. */
esp_err_t SettingsServer::screenshotHandler(httpd_req_t *req)
{
    if (!owner || !owner->screen_root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Display not ready");
    }

    const uint32_t width = SCREEN_W;
    const uint32_t height = SCREEN_H;
    const lv_color_format_t color_format = LV_COLOR_FORMAT_RGB565;
    const uint32_t stride = lv_draw_buf_width_to_stride(width, color_format);
    const uint32_t buffer_size = stride * height;

    uint8_t *pixels = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) {
        pixels = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_8BIT);
    }
    if (!pixels) {
        ESP_LOGW(TAG, "Screenshot buffer allocation failed (%" PRIu32 " bytes)", buffer_size);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No screenshot memory");
    }

    lv_draw_buf_t draw_buf = {};
    if (lv_draw_buf_init(&draw_buf, width, height, color_format, stride, pixels, buffer_size) != LV_RESULT_OK) {
        heap_caps_free(pixels);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Screenshot buffer failed");
    }

    esp_err_t lock_err = bsp_display_lock(3000);
    if (lock_err != ESP_OK) {
        heap_caps_free(pixels);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Display busy");
    }
    lv_result_t snap = lv_snapshot_take_to_draw_buf(owner->screen_root, color_format, &draw_buf);
    bsp_display_unlock();
    if (snap != LV_RESULT_OK) {
        heap_caps_free(pixels);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Screenshot failed");
    }

    esp_err_t err = stream_round_png(req, &draw_buf);
    heap_caps_free(pixels);
    return err;
}

/* Handle GET /api/settings/defaults. */
esp_err_t SettingsServer::defaultsHandler(httpd_req_t *req)
{
    return sendDefaultsJson(req);
}

/* Reset range presets to defaults while preserving unrelated settings. */
esp_err_t SettingsServer::rangesResetHandler(httpd_req_t *req)
{
    if (!owner) {
        return ESP_FAIL;
    }

    radar_settings_t candidate = owner->settings;
    RadarSettings defaults;

    memcpy(candidate.ranges, defaults.ranges, sizeof(candidate.ranges));
    candidate.default_range_mi = defaults.default_range_mi;
    owner->apply_settings(&candidate);
    esp_err_t err = owner->settings.save();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    }
    return sendJsonStatus(req, true, "ranges reset");
}

/* Search the generated airport table for the browser autocomplete field. */
esp_err_t SettingsServer::airportSearchHandler(httpd_req_t *req)
{
    char query[160] = {};
    char term[80] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "q", term, sizeof(term));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(root, "airports");
    if (!root || !items) {
        if (root) {
            cJSON_Delete(root);
        }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    size_t added = 0;
    if (strlen(term) >= 2) {
        for (size_t i = 0; i < airport_record_count && added < 24; ++i) {
            const airport_record_t *airport = &airport_records[i];
            bool match = RadarHttpClient::stringContainsCi(airport->name, term) ||
                         RadarHttpClient::stringContainsCi(airport->code, term);
            if (!match) {
                continue;
            }

            cJSON *item = cJSON_CreateObject();
            if (!item) {
                continue;
            }
            double lat = (double)airport->lat_e6 / 1000000.0;
            double lon = (double)airport->lon_e6 / 1000000.0;
            char label[96];
            if (airport->code[0] != '\0') {
                snprintf(label, sizeof(label), "%s - %s", airport->code, airport->name);
            } else {
                snprintf(label, sizeof(label), "%s", airport->name);
            }
            cJSON_AddStringToObject(item, "name", airport->name);
            cJSON_AddStringToObject(item, "code", airport->code);
            cJSON_AddStringToObject(item, "label", label);
            cJSON_AddNumberToObject(item, "lat", lat);
            cJSON_AddNumberToObject(item, "lon", lon);
            cJSON_AddItemToArray(items, item);
            ++added;
        }
    }
    cJSON_AddNumberToObject(root, "count", (double)added);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

/* Search OpenWeather geocoding for manual place-name radar centres. */
esp_err_t SettingsServer::locationSearchHandler(httpd_req_t *req)
{
    if (!owner) {
        return ESP_FAIL;
    }
    if (owner->settings.openweather_api_key[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OpenWeather API key required");
    }

    char query[192] = {};
    char encoded_term[96] = {};
    char term[80] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "q", encoded_term, sizeof(encoded_term)) == ESP_OK) {
        RadarApp::url_decode(term, sizeof(term), encoded_term, strlen(encoded_term));
    }
    if (strlen(term) < 2) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Location search text required");
    }

    char url_term[240];
    char url_key[256];
    if (!url_encode_component(term, url_term, sizeof(url_term)) ||
        !url_encode_component(owner->settings.openweather_api_key, url_key, sizeof(url_key))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Location query too long");
    }

    char url[560];
    snprintf(url, sizeof(url),
             "http://api.openweathermap.org/geo/1.0/direct?q=%s&limit=5&appid=%s",
             url_term, url_key);

    char *response = NULL;
    int response_len = 0;
    esp_err_t err = owner->fetch_http_buffer(url, "application/json",
                                             2048, 8192, &response, &response_len);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Location API failed");
    }

    cJSON *api = cJSON_ParseWithLength(response, response_len);
    heap_caps_free(response);
    if (!cJSON_IsArray(api)) {
        if (api) {
            cJSON_Delete(api);
        }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid location response");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(root, "locations");
    if (!root || !items) {
        if (api) {
            cJSON_Delete(api);
        }
        if (root) {
            cJSON_Delete(root);
        }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    size_t added = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, api) {
        if (added >= 5) {
            break;
        }
        int before = cJSON_GetArraySize(items);
        add_location_result(items, item);
        if (cJSON_GetArraySize(items) > before) {
            ++added;
        }
    }
    cJSON_Delete(api);
    cJSON_AddNumberToObject(root, "count", (double)added);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

/* Scan nearby WiFi networks for the settings WiFi tab. */
esp_err_t SettingsServer::wifiScanHandler(httpd_req_t *req)
{
    if (!owner) {
        return ESP_FAIL;
    }

    wifi_ap_record_t *aps = (wifi_ap_record_t *)heap_caps_calloc(MAX_WIFI_SCAN_RESULTS, sizeof(aps[0]),
                                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!aps) {
        aps = (wifi_ap_record_t *)heap_caps_calloc(MAX_WIFI_SCAN_RESULTS, sizeof(aps[0]), MALLOC_CAP_8BIT);
    }
    if (!aps) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }
    uint16_t ap_count = owner->wifi_manager.scanNetworks(aps, MAX_WIFI_SCAN_RESULTS);

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    for (uint16_t i = 0; i < ap_count; ++i) {
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", (const char *)aps[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", aps[i].rssi);
        cJSON_AddItemToArray(networks, network);
    }
    free(aps);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

/* Save WiFi credentials submitted from the settings page. */
esp_err_t SettingsServer::wifiSaveHandler(httpd_req_t *req)
{
    if (!owner) {
        return ESP_FAIL;
    }

    char *body = RadarApp::read_request_body(req, 512);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    char ssid[33];
    char password[65];
    RadarHttpClient::copyTrimmedField(ssid, sizeof(ssid), root, "ssid");
    RadarHttpClient::copyTrimmedField(password, sizeof(password), root, "password");
    cJSON_Delete(root);

    if (ssid[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
    }
    if (password[0] != '\0' && strlen(password) < 8) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password must be blank or at least 8 characters");
    }

    esp_err_t err = owner->wifi_manager.saveCredentials(ssid, password);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    }
    if (owner->wifi_event_group) {
        xEventGroupSetBits(owner->wifi_event_group, WIFI_CREDENTIALS_CHANGED_BIT);
    }
    return sendJsonStatus(req, true, "wifi saved");
}

/* Start the station-mode HTTP server and register all settings routes. */
bool SettingsServer::start()
{
    if (!owner || owner->wifi_portal_active) {
        return false;
    }
    if (owner->portal_httpd) {
        return owner->settings_http_active;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 14;
    config.stack_size = 10240;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&owner->portal_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Settings HTTP server failed: %s", esp_err_to_name(err));
        owner->portal_httpd = NULL;
        owner->settings_http_active = false;
        return false;
    }

    httpd_uri_t api_get_uri = {};
    api_get_uri.uri = "/api/settings";
    api_get_uri.method = HTTP_GET;
    api_get_uri.handler = RadarApp::settings_api_get_handler_entry;

    httpd_uri_t api_save_uri = {};
    api_save_uri.uri = "/api/settings";
    api_save_uri.method = HTTP_POST;
    api_save_uri.handler = RadarApp::settings_api_save_handler_entry;

    httpd_uri_t defaults_uri = {};
    defaults_uri.uri = "/api/settings/defaults";
    defaults_uri.method = HTTP_GET;
    defaults_uri.handler = RadarApp::settings_defaults_handler_entry;

    httpd_uri_t status_uri = {};
    status_uri.uri = "/api/status";
    status_uri.method = HTTP_GET;
    status_uri.handler = RadarApp::settings_status_handler_entry;

    httpd_uri_t screenshot_uri = {};
    screenshot_uri.uri = "/api/screenshot.png";
    screenshot_uri.method = HTTP_GET;
    screenshot_uri.handler = RadarApp::settings_screenshot_handler_entry;

    httpd_uri_t reset_ranges_uri = {};
    reset_ranges_uri.uri = "/api/ranges/reset";
    reset_ranges_uri.method = HTTP_POST;
    reset_ranges_uri.handler = RadarApp::settings_ranges_reset_handler_entry;

    httpd_uri_t airport_search_uri = {};
    airport_search_uri.uri = "/api/airports";
    airport_search_uri.method = HTTP_GET;
    airport_search_uri.handler = RadarApp::settings_airport_search_handler_entry;

    httpd_uri_t location_search_uri = {};
    location_search_uri.uri = "/api/locations";
    location_search_uri.method = HTTP_GET;
    location_search_uri.handler = RadarApp::settings_location_search_handler_entry;

    httpd_uri_t wifi_scan_uri = {};
    wifi_scan_uri.uri = "/api/wifi/scan";
    wifi_scan_uri.method = HTTP_GET;
    wifi_scan_uri.handler = RadarApp::settings_wifi_scan_handler_entry;

    httpd_uri_t wifi_save_uri = {};
    wifi_save_uri.uri = "/api/wifi";
    wifi_save_uri.method = HTTP_POST;
    wifi_save_uri.handler = RadarApp::settings_wifi_save_handler_entry;

    httpd_uri_t page_uri = {};
    page_uri.uri = "/*";
    page_uri.method = HTTP_GET;
    page_uri.handler = RadarApp::settings_page_handler_entry;

    if (httpd_register_uri_handler(owner->portal_httpd, &api_get_uri) != ESP_OK ||
        httpd_register_uri_handler(owner->portal_httpd, &api_save_uri) != ESP_OK ||
        httpd_register_uri_handler(owner->portal_httpd, &defaults_uri) != ESP_OK ||
        httpd_register_uri_handler(owner->portal_httpd, &status_uri) != ESP_OK ||
        httpd_register_uri_handler(owner->portal_httpd, &screenshot_uri) != ESP_OK ||
        httpd_register_uri_handler(owner->portal_httpd, &reset_ranges_uri) != ESP_OK ||
        httpd_register_uri_handler(owner->portal_httpd, &airport_search_uri) != ESP_OK ||
        httpd_register_uri_handler(owner->portal_httpd, &location_search_uri) != ESP_OK ||
        httpd_register_uri_handler(owner->portal_httpd, &wifi_scan_uri) != ESP_OK ||
        httpd_register_uri_handler(owner->portal_httpd, &wifi_save_uri) != ESP_OK ||
        httpd_register_uri_handler(owner->portal_httpd, &page_uri) != ESP_OK) {
        httpd_stop(owner->portal_httpd);
        owner->portal_httpd = NULL;
        owner->settings_http_active = false;
        ESP_LOGW(TAG, "Settings HTTP handler registration failed");
        return false;
    }

    owner->settings_http_active = true;
    ESP_LOGI(TAG, "Settings HTTP server active");
    return true;
}

/* Stop the station-mode settings server if it is active. */
void SettingsServer::stop()
{
    if (!owner) {
        return;
    }

    if (owner->settings_http_active && owner->portal_httpd) {
        httpd_stop(owner->portal_httpd);
        owner->portal_httpd = NULL;
    }
    owner->settings_http_active = false;
}
