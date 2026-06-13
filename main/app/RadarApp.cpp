#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "nvs_flash.h"
#include "zlib.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "airport_data.h"
#include "country_boundary_data.h"
#include "PhotoDecoder.hpp"
#include "RadarApp.hpp"

static const char *TAG = "radar_console";

/*
 * RadarApp is intentionally the integration point for the app. Display objects,
 * worker tasks, settings, WiFi, and data services meet here, while the helper
 * classes keep specialised behaviour out of this large coordinator file.
 */

enum {
    DATA_MENU_FILTER_ALL = 0,
    DATA_MENU_FILTER_MILITARY,
    DATA_MENU_FILTER_INTERESTING,
    DATA_MENU_TOGGLE_HEADING,
    DATA_MENU_TOGGLE_AIRPORTS,
    DATA_MENU_TOGGLE_COUNTRIES,
    DATA_MENU_TOGGLE_RUNWAYS,
    DATA_MENU_TOGGLE_GROUND_AIRCRAFT,
};

static constexpr uint32_t ROTARY_POLL_MS = 2;

RadarApp *RadarApp::active_app = nullptr;

/* Draw a filled climb/descent triangle for an aircraft label marker. */
static void trend_triangle_draw_event(lv_event_t *event)
{
    radar_marker_t *marker = (radar_marker_t *)lv_event_get_user_data(event);
    if (!marker) {
        return;
    }

    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(event);
    lv_area_t coords = {};
    lv_obj_get_coords(obj, &coords);

    lv_draw_triangle_dsc_t dsc = {};
    lv_draw_triangle_dsc_init(&dsc);
    dsc.p[0].x = marker->trend_points[0].x + coords.x1;
    dsc.p[0].y = marker->trend_points[0].y + coords.y1;
    dsc.p[1].x = marker->trend_points[1].x + coords.x1;
    dsc.p[1].y = marker->trend_points[1].y + coords.y1;
    dsc.p[2].x = marker->trend_points[2].x + coords.x1;
    dsc.p[2].y = marker->trend_points[2].y + coords.y1;
    dsc.color = lv_obj_get_style_line_color(obj, LV_PART_MAIN);
    dsc.opa = lv_obj_get_style_line_opa(obj, LV_PART_MAIN);

    lv_draw_triangle(lv_event_get_layer(event), &dsc);
}

/* Forward LVGL range-button events to the active application instance. */
void RadarApp::range_button_event_entry(lv_event_t *event)
{
    if (active_app) {
        active_app->range_button_event(event);
    }
}

/* Forward range menu row events to the active application instance. */
void RadarApp::range_menu_event_entry(lv_event_t *event)
{
    if (active_app) {
        active_app->range_menu_event(event);
    }
}

/* Forward LVGL DATA-button events to the active application instance. */
void RadarApp::status_button_event_entry(lv_event_t *event)
{
    if (active_app) {
        active_app->status_button_event(event);
    }
}

/* Forward DATA menu row events to the active application instance. */
void RadarApp::data_menu_event_entry(lv_event_t *event)
{
    if (active_app) {
        active_app->data_menu_event(event);
    }
}

/* Forward WiFi button events to the active application instance. */
void RadarApp::wifi_button_event_entry(lv_event_t *event)
{
    if (active_app) {
        active_app->wifi_button_event(event);
    }
}

/* Forward WiFi menu IP row events to the active application instance. */
void RadarApp::wifi_menu_ip_event_entry(lv_event_t *event)
{
    if (active_app) {
        active_app->wifi_menu_ip_event(event);
    }
}

/* Forward WiFi menu setup row events to the active application instance. */
void RadarApp::wifi_menu_setup_event_entry(lv_event_t *event)
{
    if (active_app) {
        active_app->wifi_menu_setup_event(event);
    }
}

/* Forward WiFi menu reboot row events to the active application instance. */
void RadarApp::wifi_menu_reboot_event_entry(lv_event_t *event)
{
    if (active_app) {
        active_app->wifi_menu_reboot_event(event);
    }
}

/* Forward WiFi menu clear-NVS row events to the active application instance. */
void RadarApp::wifi_menu_clear_nvs_event_entry(lv_event_t *event)
{
    if (active_app) {
        active_app->wifi_menu_clear_nvs_event(event);
    }
}

/* Forward radar touch-layer events to the active application instance. */
void RadarApp::radar_touch_event_entry(lv_event_t *event)
{
    if (active_app) {
        active_app->radar_touch_event(event);
    }
}

/* Forward the sweep timer tick to the active application instance. */
void RadarApp::sweep_timer_entry(lv_timer_t *timer)
{
    if (active_app) {
        active_app->sweep_timer_cb(timer);
    }
}

/* Convert the radar's north-zero clockwise bearing into LVGL's east-zero clockwise angle. */
static int radar_to_lvgl_angle(float radar_angle_deg)
{
    int angle = (int)lroundf(radar_angle_deg + 270.0f) % 360;
    if (angle < 0) {
        angle += 360;
    }
    return angle;
}

/* Draw the bright sweep and the configured fading trail sector. */
void RadarApp::sweep_draw_event_entry(lv_event_t *event)
{
    if (!active_app || !event) {
        return;
    }
    RadarApp *app = active_app;
    if (!app->settings.show_sweep || !app->settings.visible.sweep) {
        return;
    }

    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    if (!obj || !layer) {
        return;
    }

    lv_area_t coords = {};
    lv_obj_get_coords(obj, &coords);
    const int center_x = coords.x1 + (RADAR_CENTER - app->sweep_overlay_left);
    const int center_y = coords.y1 + (RADAR_CENTER - app->sweep_overlay_top);

    int trail_count = app->settings.show_sweep_trail ? app->settings.sweep_trail_count : 0;
    if (trail_count < 0) {
        trail_count = 0;
    }
    if (trail_count > (int)SWEEP_TRAIL_COUNT) {
        trail_count = (int)SWEEP_TRAIL_COUNT;
    }

    float trail_step_deg = (float)app->settings.sweep_trail_step_deg;
    if (trail_step_deg < (float)RADAR_SETTINGS_MIN_SWEEP_TRAIL_STEP_DEG) {
        trail_step_deg = (float)RADAR_SETTINGS_MIN_SWEEP_TRAIL_STEP_DEG;
    }
    if (trail_step_deg > (float)RADAR_SETTINGS_MAX_SWEEP_TRAIL_STEP_DEG) {
        trail_step_deg = (float)RADAR_SETTINGS_MAX_SWEEP_TRAIL_STEP_DEG;
    }

    if (trail_count > 0) {
        float trail_span_deg = (float)trail_count * trail_step_deg;
        if (trail_span_deg > 359.0f) {
            trail_span_deg = 359.0f;
        }

        int segment_count = (int)ceilf(trail_span_deg);
        if (segment_count < 1) {
            segment_count = 1;
        }
        if (segment_count > 16) {
            segment_count = 16;
        }
        float segment_deg = trail_span_deg / (float)segment_count;
        lv_opa_t max_opa = (lv_opa_t)(70 + (app->settings.sweep_trail_width * 6));
        if (max_opa > 170) {
            max_opa = 170;
        }

        for (int i = 0; i < segment_count; ++i) {
            float radar_start = (float)app->sweep_angle_deg - trail_span_deg +
                                ((float)i * segment_deg);
            float radar_end = radar_start + segment_deg;
            int lv_start = radar_to_lvgl_angle(radar_start);
            int lv_end = radar_to_lvgl_angle(radar_end);
            int opa = ((int)max_opa * (i + 1)) / segment_count;
            if (opa < 4) {
                opa = 4;
            }

            lv_draw_arc_dsc_t arc_dsc = {};
            lv_draw_arc_dsc_init(&arc_dsc);
            arc_dsc.color = lv_color_hex(app->settings.colors.sweep);
            arc_dsc.opa = (lv_opa_t)opa;
            arc_dsc.width = RADAR_RADIUS;
            arc_dsc.radius = RADAR_RADIUS;
            arc_dsc.center.x = center_x;
            arc_dsc.center.y = center_y;
            arc_dsc.start_angle = lv_start;
            arc_dsc.end_angle = lv_end;
            arc_dsc.rounded = false;
            lv_draw_arc(layer, &arc_dsc);
        }
    }

    float rad = ((float)app->sweep_angle_deg - 90.0f) * PI_F / 180.0f;
    lv_draw_line_dsc_t dsc = {};
    lv_draw_line_dsc_init(&dsc);
    dsc.p1.x = center_x;
    dsc.p1.y = center_y;
    dsc.p2.x = center_x + (int)lroundf(cosf(rad) * (float)RADAR_RADIUS);
    dsc.p2.y = center_y + (int)lroundf(sinf(rad) * (float)RADAR_RADIUS);
    dsc.color = lv_color_hex(app->settings.colors.sweep);
    dsc.width = app->settings.widths.sweep;
    dsc.opa = LV_OPA_COVER;
    dsc.round_start = true;
    dsc.round_end = true;
    lv_draw_line(layer, &dsc);
}

/* Forward the aircraft UI refresh timer to the active application instance. */
void RadarApp::update_aircraft_ui_entry(lv_timer_t *timer)
{
    if (active_app) {
        active_app->update_aircraft_ui(timer);
    }
}

/* Run the aircraft photo worker through the active application instance. */
void RadarApp::aircraft_photo_fetch_task_entry(void *arg)
{
    if (active_app) {
        active_app->aircraft_photo_fetch_task(arg);
        return;
    }
    heap_caps_free(arg);
    vTaskDeleteWithCaps(NULL);
}

/* Run the aircraft route worker through the active application instance. */
void RadarApp::aircraft_route_fetch_task_entry(void *arg)
{
    if (active_app) {
        active_app->aircraft_route_fetch_task(arg);
        return;
    }
    heap_caps_free(arg);
    vTaskDeleteWithCaps(NULL);
}

/* Forward captive portal GET requests to the active application instance. */
esp_err_t RadarApp::portal_get_handler_entry(httpd_req_t *req)
{
    return active_app ? active_app->portal_get_handler(req) : ESP_FAIL;
}

/* Forward captive portal save requests to the active application instance. */
esp_err_t RadarApp::portal_save_handler_entry(httpd_req_t *req)
{
    return active_app ? active_app->portal_save_handler(req) : ESP_FAIL;
}

/* Forward settings page requests to the active application instance. */
esp_err_t RadarApp::settings_page_handler_entry(httpd_req_t *req)
{
    return active_app ? active_app->settings_page_handler(req) : ESP_FAIL;
}

/* Forward settings API read requests to the active application instance. */
esp_err_t RadarApp::settings_api_get_handler_entry(httpd_req_t *req)
{
    return active_app ? active_app->settings_api_get_handler(req) : ESP_FAIL;
}

/* Forward settings API save requests to the active application instance. */
esp_err_t RadarApp::settings_api_save_handler_entry(httpd_req_t *req)
{
    return active_app ? active_app->settings_api_save_handler(req) : ESP_FAIL;
}

/* Forward settings status requests to the active application instance. */
esp_err_t RadarApp::settings_status_handler_entry(httpd_req_t *req)
{
    return active_app ? active_app->settings_status_handler(req) : ESP_FAIL;
}

/* Forward settings screenshot requests to the active application instance. */
esp_err_t RadarApp::settings_screenshot_handler_entry(httpd_req_t *req)
{
    return active_app ? active_app->settings_screenshot_handler(req) : ESP_FAIL;
}

/* Forward settings default requests to the active application instance. */
esp_err_t RadarApp::settings_defaults_handler_entry(httpd_req_t *req)
{
    return active_app ? active_app->settings_defaults_handler(req) : ESP_FAIL;
}

/* Forward range-reset requests to the active application instance. */
esp_err_t RadarApp::settings_ranges_reset_handler_entry(httpd_req_t *req)
{
    return active_app ? active_app->settings_ranges_reset_handler(req) : ESP_FAIL;
}

/* Forward airport-search requests to the active application instance. */
esp_err_t RadarApp::settings_airport_search_handler_entry(httpd_req_t *req)
{
    return active_app ? active_app->settings_airport_search_handler(req) : ESP_FAIL;
}

/* Forward location-search requests to the active application instance. */
esp_err_t RadarApp::settings_location_search_handler_entry(httpd_req_t *req)
{
    return active_app ? active_app->settings_location_search_handler(req) : ESP_FAIL;
}

/* Forward WiFi-scan requests to the active application instance. */
esp_err_t RadarApp::settings_wifi_scan_handler_entry(httpd_req_t *req)
{
    return active_app ? active_app->settings_wifi_scan_handler(req) : ESP_FAIL;
}

/* Forward WiFi-save requests to the active application instance. */
esp_err_t RadarApp::settings_wifi_save_handler_entry(httpd_req_t *req)
{
    return active_app ? active_app->settings_wifi_save_handler(req) : ESP_FAIL;
}

/* Run the captive portal DNS task through a concrete application instance. */
void RadarApp::dns_server_task_entry(void *arg)
{
    RadarApp *app = (RadarApp *)arg;
    if (!app) {
        app = active_app;
    }
    if (app) {
        app->dns_server_task(arg);
        return;
    }
    vTaskDeleteWithCaps(NULL);
}

/* Forward ESP-IDF WiFi/IP events to a concrete application instance. */
void RadarApp::wifi_event_handler_entry(void *arg, esp_event_base_t event_base,
                                        int32_t event_id, void *event_data)
{
    RadarApp *app = (RadarApp *)arg;
    if (!app) {
        app = active_app;
    }
    if (app) {
        app->wifi_event_handler(arg, event_base, event_id, event_data);
    }
}

/* Run the aircraft fetch worker through a concrete application instance. */
void RadarApp::aircraft_fetch_task_entry(void *arg)
{
    RadarApp *app = (RadarApp *)arg;
    if (!app) {
        app = active_app;
    }
    if (app) {
        app->aircraft_fetch_task(arg);
        return;
    }
    vTaskDeleteWithCaps(NULL);
}

/* Forward aircraft popup close events to the active application instance. */
void RadarApp::aircraft_popup_close_event_entry(lv_event_t *event)
{
    if (active_app) {
        active_app->aircraft_popup_close_event(event);
    }
}

/* Allocate the shared aircraft snapshot buffer, preferring PSRAM. */
aircraft_data_t *RadarApp::alloc_aircraft_buffer(void)
{
    /*
     * Aircraft snapshots can be hundreds of records. Prefer PSRAM so internal
     * RAM remains available for TLS, SDIO WiFi buffers, and LVGL draw work.
     */
    size_t bytes = MAX_AIRCRAFT_TARGETS * sizeof(aircraft_data_t);
    aircraft_data_t *buffer = (aircraft_data_t *)heap_caps_calloc(MAX_AIRCRAFT_TARGETS,
                                                                  sizeof(aircraft_data_t),
                                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        buffer = (aircraft_data_t *)heap_caps_calloc(MAX_AIRCRAFT_TARGETS, sizeof(aircraft_data_t),
                                                     MALLOC_CAP_8BIT);
    }
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate aircraft buffer (%u bytes)", (unsigned)bytes);
    }
    return buffer;
}

/* Allocate shared aircraft buffers and bind them to the parser service. */
void RadarApp::init_aircraft_storage(void)
{
    latest_aircraft = alloc_aircraft_buffer();
    ui_aircraft_snapshot = alloc_aircraft_buffer();
    aircraft_ui_state = (aircraft_ui_state_t *)heap_caps_calloc(MAX_AIRCRAFT_TARGETS,
                                                                sizeof(aircraft_ui_state[0]),
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!aircraft_ui_state) {
        aircraft_ui_state = (aircraft_ui_state_t *)heap_caps_calloc(MAX_AIRCRAFT_TARGETS,
                                                                    sizeof(aircraft_ui_state[0]),
                                                                    MALLOC_CAP_8BIT);
    }
    if (!aircraft_ui_state) {
        ESP_LOGE(TAG, "Failed to allocate aircraft UI state buffer (%u bytes)",
                 (unsigned)(MAX_AIRCRAFT_TARGETS * sizeof(aircraft_ui_state[0])));
    }
    ESP_ERROR_CHECK(latest_aircraft && ui_aircraft_snapshot && aircraft_ui_state ?
                    ESP_OK : ESP_ERR_NO_MEM);
    aircraft_service.bind(aircraft_mutex, latest_aircraft, &latest_aircraft_count,
                          &latest_aircraft_generation, latest_status, sizeof(latest_status));
}

/* Return the currently selected display range in statute miles. */
int RadarApp::get_current_range_mi(void) const
{
    size_t count = get_range_count();
    if (count == 0) {
        return 50;
    }
    size_t index = range_index < count ? range_index : 0;
    return settings.ranges[index].miles;
}

/* Return the refresh interval for the current range in milliseconds. */
int RadarApp::get_current_refresh_interval_ms(void) const
{
    size_t count = get_range_count();
    if (count == 0) {
        return 10000;
    }
    size_t index = range_index < count ? range_index : 0;
    int refresh_sec = settings.ranges[index].refresh_sec;
    if (refresh_sec < 2) {
        refresh_sec = 2;
    }
    if (refresh_sec > 300) {
        refresh_sec = 300;
    }
    return refresh_sec * 1000;
}

/* Return the aircraft label limit configured for the current range. */
size_t RadarApp::get_current_label_limit(void) const
{
    size_t count = get_range_count();
    if (count == 0) {
        return MAX_AIRCRAFT_LABELS;
    }
    size_t index = range_index < count ? range_index : 0;
    int label_count = settings.ranges[index].label_count;
    if (label_count < 0) {
        label_count = 0;
    }
    if (label_count > MAX_AIRCRAFT_LABELS) {
        label_count = MAX_AIRCRAFT_LABELS;
    }
    return (size_t)label_count;
}

/* Return the number of configured non-zero range presets. */
size_t RadarApp::get_range_count(void) const
{
    return settings.rangeCount();
}

/* Select the configured default range, falling back to the first preset. */
void RadarApp::set_range_to_default(void)
{
    size_t count = get_range_count();
    if (count == 0) {
        range_index = 0;
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        if (settings.ranges[i].miles == settings.default_range_mi) {
            range_index = i;
            return;
        }
    }
    range_index = 0;
}

/* Calculate the smallest signed angular difference between two bearings. */
int RadarApp::angle_delta(int a, int b)
{
    return RadarGeometry::angleDelta(a, b);
}

/* Convert degrees to radians through the shared geometry helper. */
double RadarApp::deg_to_rad(double degrees)
{
    return RadarGeometry::degToRad(degrees);
}

/* Convert radians to degrees through the shared geometry helper. */
double RadarApp::rad_to_deg(double radians)
{
    return RadarGeometry::radToDeg(radians);
}

/* Calculate great-circle distance in statute miles. */
double RadarApp::distance_miles(double lat1, double lon1, double lat2, double lon2)
{
    return RadarGeometry::distanceMiles(lat1, lon1, lat2, lon2);
}

/* Calculate true bearing from one coordinate to another. */
int RadarApp::bearing_degrees(double lat1, double lon1, double lat2, double lon2)
{
    return RadarGeometry::bearingDegrees(lat1, lon1, lat2, lon2);
}

/* Set the short aircraft-data status text shared with the UI. */
void RadarApp::set_data_status(const char *fmt, ...)
{
    char text[sizeof(latest_status)];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    if (aircraft_mutex && xSemaphoreTake(aircraft_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        snprintf(latest_status, sizeof(latest_status), "%s", text);
        xSemaphoreGive(aircraft_mutex);
    } else {
        snprintf(latest_status, sizeof(latest_status), "%s", text);
    }
    refresh_oled_dashboard();
}

/* Set the captive portal status text shared with the UI overlay. */
void RadarApp::set_portal_status(const char *fmt, ...)
{
    char text[sizeof(portal_status)];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    if (aircraft_mutex && xSemaphoreTake(aircraft_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        snprintf(portal_status, sizeof(portal_status), "%s", text);
        xSemaphoreGive(aircraft_mutex);
    } else {
        snprintf(portal_status, sizeof(portal_status), "%s", text);
    }
}

/* Resolve the active radar centre from manual, airport, location, or GPS settings. */
bool RadarApp::get_radar_center(double *lat, double *lon, bool *using_gps) const
{
    return RadarPositionProvider::resolve(settings, gps, lat, lon, using_gps);
}

/* Apply an LVGL font to a text object. */
void RadarApp::set_obj_font(lv_obj_t *obj, const lv_font_t *font)
{
    lv_obj_set_style_text_font(obj, font, 0);
}

/* Create a styled LVGL label with the requested text, font, and colour. */
lv_obj_t *RadarApp::make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    set_obj_font(label, font);
    return label;
}

/* Resolve a saved label font family and size to one of the built-in LVGL fonts. */
const lv_font_t *RadarApp::configured_label_font(const label_font_setting_t &style, int size_delta)
{
    if (style.font == RADAR_SETTINGS_LABEL_FONT_DEFAULT) {
        return LV_FONT_DEFAULT;
    }

    int size = (int)style.size + size_delta;
    if (size < RADAR_SETTINGS_MIN_LABEL_SIZE) {
        size = RADAR_SETTINGS_MIN_LABEL_SIZE;
    }
    if (size > RADAR_SETTINGS_MAX_LABEL_SIZE) {
        size = RADAR_SETTINGS_MAX_LABEL_SIZE;
    }

    if (size <= 10) {
        return &lv_font_montserrat_10;
    }
    if (size <= 12) {
        return &lv_font_montserrat_12;
    }
    if (size <= 14) {
        return &lv_font_montserrat_14;
    }
    if (size <= 16) {
        return &lv_font_montserrat_16;
    }
    if (size <= 18) {
        return &lv_font_montserrat_18;
    }
    if (size <= 20) {
        return &lv_font_montserrat_20;
    }
    return &lv_font_montserrat_22;
}

/* Apply a saved font style to an LVGL label-like object. */
void RadarApp::apply_label_font(lv_obj_t *obj, const label_font_setting_t &style, int size_delta)
{
    if (obj) {
        lv_obj_set_style_text_font(obj, configured_label_font(style, size_delta), 0);
    }
}

/* Normalise any degree value into the 0..359 range. */
int RadarApp::normalize_degrees(int degrees)
{
    degrees %= 360;
    if (degrees < 0) {
        degrees += 360;
    }
    return degrees;
}

/* Recalculate the individual glyph positions for a curved button. */
void RadarApp::position_curved_button_text(curved_button_t *button)
{
    CurvedButtonController::positionText(button);
}

/* Update a curved button caption and reposition its glyphs. */
void RadarApp::set_curved_button_text(curved_button_t *button, const char *text)
{
    CurvedButtonController::setText(button, text);
}

/* Create a curved bezel control and its practical touch hitbox. */
void RadarApp::make_curved_button(lv_obj_t *parent, curved_button_t *button,
                               int start_deg, int end_deg,
                               int x, int y, int w, int h,
                               const char *text, lv_event_cb_t cb,
                               uint32_t text_color, bool top_text)
{
    curved_button_controller.create(parent, button, start_deg, end_deg, x, y, w, h,
                                    text, cb, text_color, top_text);
}

/* Refresh range-ring labels after the selected range changes. */
void RadarApp::update_range_ring_labels(void)
{
    for (size_t i = 0; i < RANGE_RING_COUNT; ++i) {
        if (!range_ring_labels[i]) {
            continue;
        }

        int distance_mi = (get_current_range_mi() * (int)(i + 1)) / RANGE_RING_COUNT;
        char text[16];
        snprintf(text, sizeof(text), "%dMI", distance_mi);
        lv_label_set_text(range_ring_labels[i], text);
    }
}

/* Refresh the range button caption and dependent ring labels. */
void RadarApp::refresh_range_label(void)
{
    char text[24];
    snprintf(text, sizeof(text), "RNG %dMI", get_current_range_mi());
    set_curved_button_text(&range_button, text);
    update_range_ring_labels();
}

/* Move to another configured range and request fresh aircraft data. */
void RadarApp::change_range_by_delta(int delta)
{
    size_t range_count = get_range_count();
    if (range_count == 0 || delta == 0) {
        return;
    }

    int next = (int)range_index + delta;
    while (next < 0) {
        next += (int)range_count;
    }
    next %= (int)range_count;
    if ((size_t)next == range_index) {
        return;
    }

    range_index = (size_t)next;
    airport_weather_last_fetch_ms = 0;
    hide_range_menu();
    refresh_range_label();
    set_data_status("RNG %dMI", get_current_range_mi());
    if (wifi_event_group) {
        xEventGroupSetBits(wifi_event_group, FETCH_NOW_BIT);
    }
    ESP_LOGI(TAG, "Range changed to %dMI", get_current_range_mi());
}

/* Create one row in the range picker popup. */
lv_obj_t *RadarApp::create_range_menu_row(lv_obj_t *parent, int y, size_t index, lv_obj_t **label_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 248, 34);
    lv_obj_set_pos(row, 12, y);
    lv_obj_set_style_bg_color(row, lv_color_hex(settings.colors.popup_border), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_10, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(settings.colors.button_pressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(row, lv_color_hex(settings.colors.popup_border), 0);
    lv_obj_set_style_border_opa(row, LV_OPA_40, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, RadarApp::range_menu_event_entry,
                        LV_EVENT_CLICKED, (void *)index);

    lv_obj_t *label = make_label(row, "",
                                 configured_label_font(settings.label_styles.button, -2),
                                 settings.colors.button_text);
    lv_obj_set_width(label, 224);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    if (label_out) {
        *label_out = label;
    }
    return row;
}

/* Create the range picker popup. */
void RadarApp::create_range_menu(lv_obj_t *screen)
{
    range_menu = lv_obj_create(screen);
    lv_obj_set_size(range_menu, 272, 426);
    lv_obj_set_pos(range_menu, 28, 170);
    lv_obj_set_style_bg_color(range_menu, lv_color_hex(settings.colors.popup_bg), 0);
    lv_obj_set_style_bg_opa(range_menu, LV_OPA_90, 0);
    lv_obj_set_style_border_color(range_menu, lv_color_hex(settings.colors.popup_border), 0);
    lv_obj_set_style_border_opa(range_menu, LV_OPA_70, 0);
    lv_obj_set_style_border_width(range_menu, 1, 0);
    lv_obj_set_style_radius(range_menu, 8, 0);
    lv_obj_set_style_pad_all(range_menu, 0, 0);
    lv_obj_clear_flag(range_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(range_menu, LV_OBJ_FLAG_HIDDEN);

    for (size_t i = 0; i < MAX_RANGE_SETTINGS; ++i) {
        range_menu_rows[i] = create_range_menu_row(range_menu, 12 + ((int)i * 40), i,
                                                   &range_menu_labels[i]);
    }
    refresh_range_menu();
}

/* Refresh range picker rows from configured range presets. */
void RadarApp::refresh_range_menu(void)
{
    if (!range_menu) {
        return;
    }

    char text[32];
    for (size_t i = 0; i < MAX_RANGE_SETTINGS; ++i) {
        if (!range_menu_rows[i] || !range_menu_labels[i]) {
            continue;
        }
        if (settings.ranges[i].miles <= 0) {
            lv_obj_add_flag(range_menu_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        snprintf(text, sizeof(text), "%s %d MI",
                 i == range_index ? "[*]" : "[ ]", settings.ranges[i].miles);
        lv_label_set_text(range_menu_labels[i], text);
        lv_obj_clear_flag(range_menu_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Show the range picker and close other button menus. */
void RadarApp::show_range_menu(void)
{
    if (!range_menu) {
        return;
    }
    hide_wifi_menu();
    hide_data_menu();
    refresh_range_menu();
    lv_obj_clear_flag(range_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(range_menu);
}

/* Hide the range picker. */
void RadarApp::hide_range_menu(void)
{
    if (range_menu) {
        lv_obj_add_flag(range_menu, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Toggle the range picker. */
void RadarApp::toggle_range_menu(void)
{
    if (!range_menu) {
        return;
    }
    if (lv_obj_has_flag(range_menu, LV_OBJ_FLAG_HIDDEN)) {
        show_range_menu();
    } else {
        hide_range_menu();
    }
}

/* Format the current station or portal IP address for the WiFi menu. */
void RadarApp::format_wifi_ip_text(char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "IP --");
    EventBits_t bits = wifi_event_group ? xEventGroupGetBits(wifi_event_group) : 0;

    if (wifi_portal_active || ((bits & WIFI_PORTAL_ACTIVE_BIT) != 0)) {
        snprintf(dst, dst_size, "IP %s", WIFI_SETUP_AP_IP);
    } else if ((bits & WIFI_CONNECTED_BIT) != 0 && wifi_sta_netif) {
        esp_netif_ip_info_t info = {};
        if (esp_netif_get_ip_info(wifi_sta_netif, &info) == ESP_OK && info.ip.addr != 0) {
            snprintf(dst, dst_size, "IP " IPSTR, IP2STR(&info.ip));
        }
    }
}

/* Show a short network status on the optional SSD1306 status display. */
void RadarApp::update_oled_status(const char *status)
{
    if (status && status[0]) {
        snprintf(oled_wifi_line, sizeof(oled_wifi_line), "WIFI %s", status);
    }
    refresh_oled_dashboard();
}

/* Show the current station or setup AP IP on the optional SSD1306 status display. */
void RadarApp::update_oled_ip(const char *ip, bool setup_ap)
{
    if (ip && ip[0]) {
        snprintf(oled_wifi_line, sizeof(oled_wifi_line), "%s %s", setup_ap ? "SETUP" : "WIFI", ip);
    } else {
        snprintf(oled_wifi_line, sizeof(oled_wifi_line), "%s", setup_ap ? "SETUP --" : "WIFI --");
    }
    refresh_oled_dashboard();
}

/* Show the current background task activity on the optional SSD1306. */
void RadarApp::update_oled_activity(const char *fmt, ...)
{
    if (!fmt || fmt[0] == '\0') {
        snprintf(oled_activity_line, sizeof(oled_activity_line), "%s", "IDLE");
    } else {
        va_list args;
        va_start(args, fmt);
        vsnprintf(oled_activity_line, sizeof(oled_activity_line), fmt, args);
        va_end(args);
    }
    refresh_oled_dashboard();
}

/* Keep the optional OLED focused on network state and current radar health. */
void RadarApp::refresh_oled_dashboard(void)
{
    char status[sizeof(latest_status)] = {};
    if (aircraft_mutex && xSemaphoreTake(aircraft_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        snprintf(status, sizeof(status), "%s", latest_status);
        xSemaphoreGive(aircraft_mutex);
    } else {
        snprintf(status, sizeof(status), "%s", latest_status);
    }

    char line1[24];
    char line2[24];
    char line3[24];
    char line4[24];
    snprintf(line1, sizeof(line1), "%s", oled_activity_line[0] ? oled_activity_line : "IDLE");
    snprintf(line2, sizeof(line2), "DATA %s", status[0] ? status : "WAIT");
    snprintf(line3, sizeof(line3), "%dMI %u AC", get_current_range_mi(), (unsigned)ui_aircraft_count);
    gps_snapshot_t gps_snapshot = {};
    gps.getSnapshot(&gps_snapshot);
    snprintf(line4, sizeof(line4), "GPS %s",
             (gps_snapshot.has_fix && !gps_snapshot.fix_stale) ? "LOCK" :
             (gps_snapshot.device_connected ? "WAIT" : "NO GPS"));
    oled_status.showDashboard(oled_wifi_line, line1, line2, line3, line4);
}

/* Refresh WiFi menu rows from the current connection or portal state. */
void RadarApp::refresh_wifi_menu(void)
{
    if (!wifi_menu || !wifi_menu_ip_label) {
        return;
    }

    char ip_text[32];
    format_wifi_ip_text(ip_text, sizeof(ip_text));
    lv_label_set_text(wifi_menu_ip_label, ip_text);
}

/* Show the WiFi popup menu and close the DATA menu. */
void RadarApp::show_wifi_menu(void)
{
    if (!wifi_menu) {
        return;
    }

    hide_data_menu();
    hide_range_menu();
    refresh_wifi_menu();
    lv_obj_clear_flag(wifi_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wifi_menu);
}

/* Hide the WiFi popup menu if it exists. */
void RadarApp::hide_wifi_menu(void)
{
    if (wifi_menu) {
        lv_obj_add_flag(wifi_menu, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Toggle visibility of the WiFi popup menu. */
void RadarApp::toggle_wifi_menu(void)
{
    if (!wifi_menu) {
        return;
    }

    if (lv_obj_has_flag(wifi_menu, LV_OBJ_FLAG_HIDDEN)) {
        show_wifi_menu();
    } else {
        hide_wifi_menu();
    }
}

/* Refresh DATA menu labels from the current filter and visibility settings. */
void RadarApp::refresh_data_menu(void)
{
    if (!data_menu) {
        return;
    }

    const bool runways_available = settings.center_source == RADAR_CENTER_SOURCE_AIRPORT;
    const char *filter_prefixes[3] = {
        aircraft_filter == AIRCRAFT_FILTER_ALL ? "[*]" : "[ ]",
        aircraft_filter == AIRCRAFT_FILTER_MILITARY ? "[*]" : "[ ]",
        aircraft_filter == AIRCRAFT_FILTER_INTERESTING ? "[*]" : "[ ]",
    };
    char text[64];
    if (data_menu_labels[DATA_MENU_FILTER_ALL]) {
        snprintf(text, sizeof(text), "%s ALL AIRCRAFT", filter_prefixes[0]);
        lv_label_set_text(data_menu_labels[DATA_MENU_FILTER_ALL], text);
    }
    if (data_menu_labels[DATA_MENU_FILTER_MILITARY]) {
        snprintf(text, sizeof(text), "%s MILITARY AIRCRAFT", filter_prefixes[1]);
        lv_label_set_text(data_menu_labels[DATA_MENU_FILTER_MILITARY], text);
    }
    if (data_menu_labels[DATA_MENU_FILTER_INTERESTING]) {
        snprintf(text, sizeof(text), "%s INTERESTING AIRCRAFT", filter_prefixes[2]);
        lv_label_set_text(data_menu_labels[DATA_MENU_FILTER_INTERESTING], text);
    }
    if (data_menu_labels[DATA_MENU_TOGGLE_HEADING]) {
        const char *heading_label = "OFF";
        if (settings.show_aircraft_heading &&
            settings.aircraft_heading_style == RADAR_HEADING_STYLE_LINE) {
            heading_label = "LINE";
        } else if (settings.show_aircraft_heading) {
            heading_label = "ARROW";
        }
        snprintf(text, sizeof(text), "HEADING %s", heading_label);
        lv_label_set_text(data_menu_labels[DATA_MENU_TOGGLE_HEADING], text);
    }
    if (data_menu_labels[DATA_MENU_TOGGLE_AIRPORTS]) {
        snprintf(text, sizeof(text), "AIRPORTS %s", settings.show_airports ? "ON" : "OFF");
        lv_label_set_text(data_menu_labels[DATA_MENU_TOGGLE_AIRPORTS], text);
    }
    if (data_menu_labels[DATA_MENU_TOGGLE_COUNTRIES]) {
        snprintf(text, sizeof(text), "COUNTRIES %s", settings.show_countries ? "ON" : "OFF");
        lv_label_set_text(data_menu_labels[DATA_MENU_TOGGLE_COUNTRIES], text);
    }
    if (data_menu_labels[DATA_MENU_TOGGLE_RUNWAYS]) {
        snprintf(text, sizeof(text), "RUNWAYS %s",
                 runways_available ? (settings.show_airport_runways ? "ON" : "OFF") : "N/A");
        lv_label_set_text(data_menu_labels[DATA_MENU_TOGGLE_RUNWAYS], text);
        lv_obj_set_style_text_opa(data_menu_labels[DATA_MENU_TOGGLE_RUNWAYS],
                                  runways_available ? LV_OPA_COVER : LV_OPA_50, 0);
    }
    if (data_menu_labels[DATA_MENU_TOGGLE_GROUND_AIRCRAFT]) {
        snprintf(text, sizeof(text), "GROUND A/C %s", settings.show_ground_aircraft ? "ON" : "OFF");
        lv_label_set_text(data_menu_labels[DATA_MENU_TOGGLE_GROUND_AIRCRAFT], text);
    }
    if (data_menu_rows[DATA_MENU_TOGGLE_RUNWAYS]) {
        lv_obj_set_style_opa(data_menu_rows[DATA_MENU_TOGGLE_RUNWAYS],
                             runways_available ? LV_OPA_COVER : LV_OPA_60, 0);
    }
}

/* Show the DATA popup menu and close the WiFi menu. */
void RadarApp::show_data_menu(void)
{
    if (!data_menu) {
        return;
    }

    hide_wifi_menu();
    hide_range_menu();
    refresh_data_menu();
    lv_obj_clear_flag(data_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(data_menu);
}

/* Hide the DATA popup menu if it exists. */
void RadarApp::hide_data_menu(void)
{
    if (data_menu) {
        lv_obj_add_flag(data_menu, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Toggle visibility of the DATA popup menu. */
void RadarApp::toggle_data_menu(void)
{
    if (!data_menu) {
        return;
    }

    if (lv_obj_has_flag(data_menu, LV_OBJ_FLAG_HIDDEN)) {
        show_data_menu();
    } else {
        hide_data_menu();
    }
}

/* Handle left, middle, and right range-button hit zones. */
void RadarApp::range_button_event(lv_event_t *event)
{
    static int64_t last_click_ms = 0;
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - last_click_ms < 250) {
        return;
    }
    last_click_ms = now_ms;

    size_t range_count = get_range_count();
    if (range_count == 0) {
        return;
    }

    intptr_t direction = event ? (intptr_t)lv_event_get_user_data(event) : 1;
    if (direction == 0) {
        toggle_range_menu();
        return;
    }

    change_range_by_delta(direction < 0 ? -1 : 1);
}

/* Select one configured range from the range popup and request an immediate fetch. */
void RadarApp::range_menu_event(lv_event_t *event)
{
    size_t index = event ? (size_t)(intptr_t)lv_event_get_user_data(event) : 0;
    if (index >= get_range_count() || settings.ranges[index].miles <= 0) {
        return;
    }

    range_index = index;
    airport_weather_last_fetch_ms = 0;
    hide_range_menu();
    refresh_range_label();
    set_data_status("RNG %dMI", get_current_range_mi());
    if (wifi_event_group) {
        xEventGroupSetBits(wifi_event_group, FETCH_NOW_BIT);
    }
    ESP_LOGI(TAG, "Range selected from menu: %dMI", get_current_range_mi());
}

/* Handle the DATA button by toggling the DATA menu. */
void RadarApp::status_button_event(lv_event_t *event)
{
    (void)event;
    toggle_data_menu();
}

/* Apply a selected DATA menu action to filters or live visibility settings. */
void RadarApp::data_menu_event(lv_event_t *event)
{
    intptr_t action = event ? (intptr_t)lv_event_get_user_data(event) : DATA_MENU_FILTER_ALL;
    bool close_menu = false;
    bool changed = false;

    switch (action) {
    case DATA_MENU_FILTER_ALL:
        aircraft_filter = AIRCRAFT_FILTER_ALL;
        close_menu = true;
        changed = true;
        break;
    case DATA_MENU_FILTER_MILITARY:
        aircraft_filter = AIRCRAFT_FILTER_MILITARY;
        close_menu = true;
        changed = true;
        break;
    case DATA_MENU_FILTER_INTERESTING:
        aircraft_filter = AIRCRAFT_FILTER_INTERESTING;
        close_menu = true;
        changed = true;
        break;
    case DATA_MENU_TOGGLE_HEADING:
        if (!settings.show_aircraft_heading ||
            settings.aircraft_heading_style == RADAR_HEADING_STYLE_NONE) {
            settings.show_aircraft_heading = true;
            settings.aircraft_heading_style = RADAR_HEADING_STYLE_ARROW;
        } else if (settings.aircraft_heading_style == RADAR_HEADING_STYLE_ARROW) {
            settings.aircraft_heading_style = RADAR_HEADING_STYLE_LINE;
        } else {
            settings.show_aircraft_heading = false;
            settings.aircraft_heading_style = RADAR_HEADING_STYLE_NONE;
        }
        settings.visible.aircraft_heading = settings.show_aircraft_heading;
        changed = true;
        break;
    case DATA_MENU_TOGGLE_AIRPORTS:
        settings.show_airports = !settings.show_airports;
        settings.visible.airport = settings.show_airports;
        changed = true;
        break;
    case DATA_MENU_TOGGLE_COUNTRIES:
        settings.show_countries = !settings.show_countries;
        settings.visible.country_boundary = settings.show_countries;
        changed = true;
        break;
    case DATA_MENU_TOGGLE_RUNWAYS:
        if (settings.center_source == RADAR_CENTER_SOURCE_AIRPORT) {
            settings.show_airport_runways = !settings.show_airport_runways;
            settings.visible.runway = settings.show_airport_runways;
            if (!settings.show_airport_runways) {
                clear_active_runway_cache();
            } else if (wifi_event_group) {
                xEventGroupSetBits(wifi_event_group, FETCH_NOW_BIT);
            }
            changed = true;
        }
        break;
    case DATA_MENU_TOGGLE_GROUND_AIRCRAFT:
        settings.show_ground_aircraft = !settings.show_ground_aircraft;
        changed = true;
        break;
    default:
        break;
    }

    if (changed) {
        settings_generation++;
        invalidate_aircraft_display();
        refresh_data_menu();
    }
    if (close_menu) {
        hide_data_menu();
    }
}

/* Handle the WiFi button by toggling the WiFi menu. */
void RadarApp::wifi_button_event(lv_event_t *event)
{
    (void)event;
    toggle_wifi_menu();
}

/* Show the current IP address briefly in the DATA status area. */
void RadarApp::wifi_menu_ip_event(lv_event_t *event)
{
    (void)event;
    char ip_text[32];
    format_wifi_ip_text(ip_text, sizeof(ip_text));
    set_data_status("%s", ip_text);
    hide_wifi_menu();
}

/* Request captive portal mode from the WiFi menu. */
void RadarApp::wifi_menu_setup_event(lv_event_t *event)
{
    (void)event;
    hide_wifi_menu();
    request_wifi_portal();
}

/* Reboot the ESP32 after the WiFi menu reboot action is selected. */
void RadarApp::wifi_menu_reboot_event(lv_event_t *event)
{
    (void)event;
    hide_wifi_menu();
    ESP_LOGW(TAG, "Reboot requested from WiFi menu");
    esp_restart();
}

/* Erase saved WiFi/settings NVS and reboot so the app returns to first-run setup. */
void RadarApp::wifi_menu_clear_nvs_event(lv_event_t *event)
{
    (void)event;
    hide_wifi_menu();
    set_data_status("NVS CLR");
    ESP_LOGW(TAG, "NVS erase requested from WiFi menu");

    esp_err_t err = nvs_flash_deinit();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGW(TAG, "NVS deinit before erase failed: %s", esp_err_to_name(err));
    }

    err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(err));
        set_data_status("NVS ERR");
        return;
    }

    set_data_status("REBOOT");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* Darken a colour while keeping enough channel detail for the marker to remain visible. */
uint32_t RadarApp::dim_colour(uint32_t color)
{
    const uint32_t r = ((color >> 16) & 0xff) * 32U / 100U;
    const uint32_t g = ((color >> 8) & 0xff) * 32U / 100U;
    const uint32_t b = (color & 0xff) * 32U / 100U;
    return (r << 16) | (g << 8) | b;
}

/* Return a stable key for aircraft history, preferring ICAO hex over callsign. */
const char *RadarApp::aircraft_track_key(const aircraft_data_t *aircraft)
{
    if (!aircraft) {
        return "";
    }
    return aircraft->icao[0] != '\0' ? aircraft->icao : aircraft->callsign;
}

/* Find or create a fixed history slot for one aircraft. */
aircraft_track_history_t *RadarApp::track_history_for_aircraft(const aircraft_data_t *aircraft)
{
    const char *key = aircraft_track_key(aircraft);
    if (!key || key[0] == '\0') {
        return nullptr;
    }
    if (!aircraft_tracks) {
        aircraft_tracks = (aircraft_track_history_t *)heap_caps_calloc(MAX_AIRCRAFT_TRACKS,
                                                                       sizeof(aircraft_tracks[0]),
                                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!aircraft_tracks) {
            aircraft_tracks = (aircraft_track_history_t *)heap_caps_calloc(MAX_AIRCRAFT_TRACKS,
                                                                           sizeof(aircraft_tracks[0]),
                                                                           MALLOC_CAP_8BIT);
        }
        if (!aircraft_tracks) {
            ESP_LOGW(TAG, "Unable to allocate aircraft history cache");
            return nullptr;
        }
    }

    aircraft_track_history_t *oldest = &aircraft_tracks[0];
    for (size_t i = 0; i < MAX_AIRCRAFT_TRACKS; ++i) {
        aircraft_track_history_t *track = &aircraft_tracks[i];
        if (track->key[0] != '\0' && strncmp(track->key, key, sizeof(track->key)) == 0) {
            return track;
        }
        if (track->key[0] == '\0') {
            oldest = track;
            break;
        }
        if (track->last_generation < oldest->last_generation) {
            oldest = track;
        }
    }

    memset(oldest, 0, sizeof(*oldest));
    snprintf(oldest->key, sizeof(oldest->key), "%s", key);
    return oldest;
}

/* Append current positions to the aircraft history cache after a displayed update. */
void RadarApp::update_aircraft_track_history(const aircraft_data_t *snapshot, size_t count,
                                             int range_mi, uint32_t generation)
{
    for (size_t i = 0; i < count; ++i) {
        if (snapshot[i].distance_mi > (float)range_mi) {
            break;
        }
        if (!aircraft_matches_filter(&snapshot[i])) {
            continue;
        }

        aircraft_track_history_t *track = track_history_for_aircraft(&snapshot[i]);
        if (!track || track->last_generation == generation) {
            continue;
        }
        if (track->count > 0 &&
            fabsf(track->points[0].distance_mi - snapshot[i].distance_mi) < 0.03f &&
            abs(angle_delta(track->points[0].bearing_deg, snapshot[i].bearing_deg)) < 1) {
            track->last_generation = generation;
            continue;
        }

        size_t move_count = track->count < AIRCRAFT_HISTORY_POINTS - 1 ?
                            track->count : AIRCRAFT_HISTORY_POINTS - 1;
        if (move_count > 0) {
            memmove(&track->points[1], &track->points[0],
                    move_count * sizeof(track->points[0]));
        }
        track->points[0].distance_mi = snapshot[i].distance_mi;
        track->points[0].bearing_deg = snapshot[i].bearing_deg;
        if (track->count < AIRCRAFT_HISTORY_POINTS) {
            ++track->count;
        }
        track->last_generation = generation;
    }
}

/* Choose the colour used to plot one aircraft marker. */
uint32_t RadarApp::marker_color(const aircraft_data_t *aircraft, bool hit, bool dimmed) const
{
    return marker_color_with_notification(aircraft, matching_notification(aircraft), hit, dimmed);
}

/* Choose the colour used to plot one aircraft marker when notification matching is already known. */
uint32_t RadarApp::marker_color_with_notification(const aircraft_data_t *aircraft,
                                                  const notification_setting_t *notification,
                                                  bool hit, bool dimmed) const
{
    uint32_t color = settings.altitude_colors.above_40000;
    if (hit) {
        color = settings.colors.aircraft_hit;
    } else if (aircraft && settings.emergency_squawks_red &&
               (strcmp(aircraft->squawk, "7500") == 0 ||
                strcmp(aircraft->squawk, "7600") == 0 ||
                strcmp(aircraft->squawk, "7700") == 0)) {
        color = settings.colors.aircraft_emergency;
    } else {
        if (notification) {
            color = notification->color;
        } else if (aircraft->seen_s > 7.5f) {
            color = settings.colors.aircraft_stale;
        } else if (aircraft->altitude_ft <= 0) {
            color = settings.altitude_colors.ground;
        } else if (aircraft->altitude_ft < 2000) {
            color = settings.altitude_colors.below_2000;
        } else if (aircraft->altitude_ft < 10000) {
            color = settings.altitude_colors.below_10000;
        } else if (aircraft->altitude_ft < 20000) {
            color = settings.altitude_colors.below_20000;
        } else if (aircraft->altitude_ft < 30000) {
            color = settings.altitude_colors.below_30000;
        } else if (aircraft->altitude_ft < 40000) {
            color = settings.altitude_colors.below_40000;
        }
    }
    return dimmed ? dim_colour(color) : color;
}

/* Return the first matching notification row index for an aircraft type. */
int RadarApp::matching_notification_index(const aircraft_data_t *aircraft) const
{
    if (!aircraft || aircraft->type[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < MAX_NOTIFICATION_SETTINGS; ++i) {
        const notification_setting_t *notification = &settings.notifications[i];
        if (!notification->enabled || notification->type_match[0] == '\0') {
            continue;
        }
        if (string_equals_trimmed_ci(aircraft->type, notification->type_match)) {
            return (int)i;
        }
    }
    return -1;
}

/* Return the first notification rule whose aircraft-type text matches this aircraft. */
const notification_setting_t *RadarApp::matching_notification(const aircraft_data_t *aircraft) const
{
    int index = matching_notification_index(aircraft);
    if (index < 0 || index >= (int)MAX_NOTIFICATION_SETTINGS) {
        return nullptr;
    }
    return &settings.notifications[index];
}

/* Check whether an aircraft matches a notification rule that focuses the radar. */
bool RadarApp::matches_focus_notification(const aircraft_data_t *aircraft) const
{
    if (!aircraft || aircraft->type[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < MAX_NOTIFICATION_SETTINGS; ++i) {
        const notification_setting_t *notification = &settings.notifications[i];
        if (!notification->enabled || !notification->dim_others || notification->type_match[0] == '\0') {
            continue;
        }
        if (string_equals_trimmed_ci(aircraft->type, notification->type_match)) {
            return true;
        }
    }
    return false;
}

/* Return whether any currently visible aircraft should focus the radar. */
bool RadarApp::notification_focus_active(const aircraft_data_t *snapshot, size_t count, int range_mi) const
{
    for (size_t i = 0; i < count; ++i) {
        if (snapshot[i].distance_mi > (float)range_mi) {
            break;
        }
        if (!aircraft_matches_filter(&snapshot[i])) {
            continue;
        }
        if (matches_focus_notification(&snapshot[i])) {
            return true;
        }
    }
    return false;
}

/* Check whether an aircraft should be visible under the current DATA filter. */
bool RadarApp::aircraft_matches_filter(const aircraft_data_t *aircraft) const
{
    if (!aircraft) {
        return false;
    }
    if (aircraft->altitude_ft == 0) {
        if (aircraft->speed_kt < settings.ground_speed_kt) {
            return false;
        }
        if (!settings.show_ground_aircraft) {
            return false;
        }
    }

    switch (aircraft_filter) {
    case AIRCRAFT_FILTER_MILITARY:
        return aircraft->has_db_flags && ((aircraft->db_flags & 1) != 0);
    case AIRCRAFT_FILTER_INTERESTING:
        return aircraft->has_db_flags && ((aircraft->db_flags & 2) != 0);
    case AIRCRAFT_FILTER_ALL:
    default:
        return true;
    }
}

/* Force the next aircraft UI update to redraw data and settings-dependent layers. */
void RadarApp::invalidate_aircraft_display()
{
    displayed_generation = UINT32_MAX;
    displayed_settings_generation = UINT32_MAX;
}

/* Clamp an integer through the shared geometry helper. */
int RadarApp::clamp_int(int value, int min_value, int max_value)
{
    return RadarGeometry::clampInt(value, min_value, max_value);
}

/* Project one aircraft into radar canvas coordinates for drawing and hit testing. */
bool RadarApp::project_aircraft_to_radar(const aircraft_data_t *aircraft, int range_mi, int *x, int *y)
{
    if (!aircraft || !x || !y || aircraft->distance_mi > (float)range_mi || range_mi <= 0) {
        return false;
    }

    float rad = ((float)aircraft->bearing_deg - 90.0f) * PI_F / 180.0f;
    int distance = (int)(((float)RADAR_RADIUS * aircraft->distance_mi) / (float)range_mi);
    *x = RADAR_CENTER + (int)(cosf(rad) * distance);
    *y = RADAR_CENTER + (int)(sinf(rad) * distance);
    return true;
}

/* Place an aircraft label away from the symbol and choose the nearest leader endpoint. */
void RadarApp::position_aircraft_label(aircraft_ui_state_t *state)
{
    if (!state) {
        return;
    }

    static constexpr int LABEL_W = 124;
    static constexpr int LABEL_H = 44;
    static constexpr int GAP_X = 24;
    static constexpr int GAP_Y = 12;

    int side = state->x < RADAR_CENTER ? 1 : -1;
    int vertical = state->y < RADAR_CENTER ? 1 : -1;
    int label_x = side > 0 ? state->x + GAP_X : state->x - GAP_X - LABEL_W;
    int label_y = state->y + (vertical * GAP_Y) - (LABEL_H / 2);

    int min_x = RADAR_CENTER - RADAR_RADIUS + 8;
    int max_x = RADAR_CENTER + RADAR_RADIUS - LABEL_W - 8;
    int min_y = RADAR_CENTER - RADAR_RADIUS + 8;
    int max_y = RADAR_CENTER + RADAR_RADIUS - LABEL_H - 8;
    label_x = clamp_int(label_x, min_x, max_x);
    label_y = clamp_int(label_y, min_y, max_y);

    state->label_x = label_x;
    state->label_y = label_y;
    state->leader_x = side > 0 ? label_x : label_x + LABEL_W;
    state->leader_y = clamp_int(state->y, label_y + 8, label_y + LABEL_H - 8);
}

/* Precompute aircraft display state once per fetched snapshot. */
void RadarApp::build_aircraft_ui_state(const aircraft_data_t *snapshot, size_t count, int range_mi)
{
    if (!snapshot || !aircraft_ui_state || range_mi <= 0) {
        aircraft_ui_focus_active = false;
        return;
    }
    if (count > MAX_AIRCRAFT_TARGETS) {
        count = MAX_AIRCRAFT_TARGETS;
    }

    aircraft_ui_focus_active = false;
    for (size_t i = 0; i < count; ++i) {
        aircraft_ui_state_t *state = &aircraft_ui_state[i];
        *state = {};
        state->notification_index = -1;
        state->label_slot = -1;

        if (snapshot[i].distance_mi > (float)range_mi) {
            continue;
        }

        state->in_range = true;
        if (!aircraft_matches_filter(&snapshot[i])) {
            continue;
        }

        state->visible = true;
        state->notification_index = matching_notification_index(&snapshot[i]);
        if (state->notification_index >= 0 &&
            settings.notifications[state->notification_index].dim_others) {
            state->focus_match = true;
            aircraft_ui_focus_active = true;
        }
        if (project_aircraft_to_radar(&snapshot[i], range_mi, &state->x, &state->y)) {
            position_aircraft_label(state);
        }
    }

    for (size_t i = 0; i < count; ++i) {
        aircraft_ui_state_t *state = &aircraft_ui_state[i];
        if (!state->visible) {
            continue;
        }
        const notification_setting_t *notification =
            state->notification_index >= 0 ? &settings.notifications[state->notification_index] : nullptr;
        state->dimmed = aircraft_ui_focus_active && !state->focus_match;
        state->color = marker_color_with_notification(&snapshot[i], notification, false, state->dimmed);
    }

    size_t label_limit = get_current_label_limit();
    size_t label_slot = 0;
    for (size_t i = 0; i < count && label_slot < MAX_AIRCRAFT_LABELS; ++i) {
        aircraft_ui_state_t *state = &aircraft_ui_state[i];
        if (!state->visible) {
            continue;
        }
        if (label_slot >= label_limit && state->notification_index < 0) {
            continue;
        }
        state->label_slot = (int)label_slot++;
    }
}

/* Find the plotted aircraft nearest to the supplied screen point. */
int RadarApp::find_aircraft_at_point(int x, int y)
{
    return popup_controller.findAircraftAtPoint(x, y);
}

/* Return popup text or a dash when the field is empty. */
const char *RadarApp::popup_value_or_dash(const char *value)
{
    return AircraftPopupController::valueOrDash(value);
}

/* Format an altitude value for the aircraft popup. */
void RadarApp::format_altitude(char *dst, size_t dst_size, int altitude_ft)
{
    AircraftPopupController::formatAltitude(dst, dst_size, altitude_ft);
}

/* Update the aircraft popup photo status text. */
void RadarApp::set_aircraft_photo_status_text(const char *text)
{
    popup_controller.setPhotoStatusText(text);
}

/* Release any image buffer currently owned by the aircraft popup. */
void RadarApp::release_aircraft_photo_image(void)
{
    popup_controller.releasePhotoImage();
}

/* Hide the aircraft popup and invalidate outstanding popup work. */
void RadarApp::hide_aircraft_popup(void)
{
    popup_controller.hide();
}

/* Show the aircraft details popup at a touch-derived screen position. */
void RadarApp::show_aircraft_popup(const aircraft_data_t *aircraft, int screen_x, int screen_y)
{
    popup_controller.show(aircraft, screen_x, screen_y);
}

/* Handle touches on the transparent radar hit layer. */
void RadarApp::radar_touch_event(lv_event_t *event)
{
    popup_controller.handleTouchEvent(event);
}

/* Create the transparent touch layer above the radar canvases. */
void RadarApp::create_radar_touch_layer(lv_obj_t *radar)
{
    popup_controller.createTouchLayer(radar);
}

/* Keep the sweep animation independent from aircraft label styling. */
void RadarApp::update_target_highlights(void)
{
}

/* Advance the sweep line. */
void RadarApp::sweep_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    static uint32_t last_draw_ms = 0;
    if (!settings.show_sweep || !settings.visible.sweep) {
        if (sweep_overlay) {
            lv_obj_add_flag(sweep_overlay, LV_OBJ_FLAG_HIDDEN);
        }
        last_draw_ms = 0;
        return;
    }

    uint32_t now_ms = lv_tick_get();
    int draw_interval_ms = settings.sweep_draw_interval_ms;
    if (draw_interval_ms < RADAR_SETTINGS_MIN_SWEEP_DRAW_INTERVAL_MS) {
        draw_interval_ms = RADAR_SETTINGS_MIN_SWEEP_DRAW_INTERVAL_MS;
    }
    if (last_draw_ms != 0 && now_ms - last_draw_ms < (uint32_t)draw_interval_ms) {
        return;
    }
    last_draw_ms = now_ms;

    int trail_count = settings.show_sweep_trail ? settings.sweep_trail_count : 0;
    if (trail_count < 0) {
        trail_count = 0;
    }
    if (trail_count > (int)SWEEP_TRAIL_COUNT) {
        trail_count = (int)SWEEP_TRAIL_COUNT;
    }

    int step_deg = settings.sweep_step_deg;
    if (step_deg < RADAR_SETTINGS_MIN_SWEEP_STEP_DEG) {
        step_deg = RADAR_SETTINGS_MIN_SWEEP_STEP_DEG;
    }
    sweep_angle_deg = (sweep_angle_deg + step_deg) % 360;

    int max_sweep_width = settings.widths.sweep > settings.sweep_trail_width ?
                          settings.widths.sweep : settings.sweep_trail_width;
    int pad = max_sweep_width + 4;
    if (pad < 6) {
        pad = 6;
    }

    int left = RADAR_CENTER;
    int top = RADAR_CENTER;
    int right = RADAR_CENTER;
    int bottom = RADAR_CENTER;
    auto normalise_radar_angle = [](float angle_deg) {
        while (angle_deg < 0.0f) {
            angle_deg += 360.0f;
        }
        while (angle_deg >= 360.0f) {
            angle_deg -= 360.0f;
        }
        return angle_deg;
    };
    auto clockwise_delta = [&](float from_deg, float to_deg) {
        float delta = normalise_radar_angle(to_deg) - normalise_radar_angle(from_deg);
        if (delta < 0.0f) {
            delta += 360.0f;
        }
        return delta;
    };
    auto include_angle = [&](float angle_deg) {
        float rad = (angle_deg - 90.0f) * PI_F / 180.0f;
        int x2 = RADAR_CENTER + (int)(cosf(rad) * RADAR_RADIUS);
        int y2 = RADAR_CENTER + (int)(sinf(rad) * RADAR_RADIUS);
        if (x2 < left) {
            left = x2;
        }
        if (x2 > right) {
            right = x2;
        }
        if (y2 < top) {
            top = y2;
        }
        if (y2 > bottom) {
            bottom = y2;
        }
    };

    float trail_step_deg = (float)settings.sweep_trail_step_deg;
    if (trail_step_deg < (float)RADAR_SETTINGS_MIN_SWEEP_TRAIL_STEP_DEG) {
        trail_step_deg = (float)RADAR_SETTINGS_MIN_SWEEP_TRAIL_STEP_DEG;
    }
    if (trail_step_deg > (float)RADAR_SETTINGS_MAX_SWEEP_TRAIL_STEP_DEG) {
        trail_step_deg = (float)RADAR_SETTINGS_MAX_SWEEP_TRAIL_STEP_DEG;
    }

    float trail_span_deg = (float)trail_count * trail_step_deg;
    if (trail_span_deg > 359.0f) {
        trail_span_deg = 359.0f;
    }
    float trail_start_deg = normalise_radar_angle((float)sweep_angle_deg - trail_span_deg);

    include_angle((float)sweep_angle_deg);
    if (trail_count > 0) {
        include_angle(trail_start_deg);
        const float cardinals[] = {0.0f, 90.0f, 180.0f, 270.0f};
        for (size_t i = 0; i < sizeof(cardinals) / sizeof(cardinals[0]); ++i) {
            if (clockwise_delta(trail_start_deg, cardinals[i]) <= trail_span_deg) {
                include_angle(cardinals[i]);
            }
        }
    }

    left -= pad;
    top -= pad;
    right += pad;
    bottom += pad;
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right >= RADAR_SIZE) {
        right = RADAR_SIZE - 1;
    }
    if (bottom >= RADAR_SIZE) {
        bottom = RADAR_SIZE - 1;
    }

    sweep_overlay_left = left;
    sweep_overlay_top = top;
    if (sweep_overlay) {
        lv_obj_clear_flag(sweep_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(sweep_overlay, left, top);
        lv_obj_set_size(sweep_overlay, right - left + 1, bottom - top + 1);
        lv_obj_invalidate(sweep_overlay);
    }
}

/* Write one opaque canvas pixel when the coordinate is inside the radar canvas. */
void RadarApp::canvas_set_px_safe(lv_obj_t *canvas, int x, int y, uint32_t color)
{
    if (x >= 0 && x < RADAR_SIZE && y >= 0 && y < RADAR_SIZE) {
        lv_canvas_set_px(canvas, x, y, lv_color_hex(color), LV_OPA_COVER);
    }
}

/* Mix two 8-bit colour channels by a 0..1 blend amount. */
uint8_t RadarApp::mix_channel(uint8_t a, uint8_t b, float amount)
{
    return (uint8_t)((float)a + ((float)b - (float)a) * amount);
}

/* Mix two packed RGB colours by a 0..1 blend amount. */
uint32_t RadarApp::mix_color_hex(uint32_t from, uint32_t to, float amount)
{
    if (amount <= 0.0f) {
        return from;
    }
    if (amount >= 1.0f) {
        return to;
    }

    uint8_t fr = (from >> 16) & 0xff;
    uint8_t fg = (from >> 8) & 0xff;
    uint8_t fb = from & 0xff;
    uint8_t tr = (to >> 16) & 0xff;
    uint8_t tg = (to >> 8) & 0xff;
    uint8_t tb = to & 0xff;
    return ((uint32_t)mix_channel(fr, tr, amount) << 16) |
           ((uint32_t)mix_channel(fg, tg, amount) << 8) |
           mix_channel(fb, tb, amount);
}

/* Calculate antialias-style alpha for a soft ring edge. */
float RadarApp::ring_alpha(float distance, float radius, float width, float feather)
{
    float edge = fabsf(distance - radius);
    float half_width = width * 0.5f;
    if (edge <= half_width) {
        return 1.0f;
    }
    if (edge <= half_width + feather) {
        return 1.0f - ((edge - half_width) / feather);
    }
    return 0.0f;
}

/* Check whether a bearing lies inside a clockwise arc, including wrapped arcs. */
bool RadarApp::angle_in_range(float degrees, int start_deg, int end_deg)
{
    while (degrees < 0.0f) {
        degrees += 360.0f;
    }
    while (degrees >= 360.0f) {
        degrees -= 360.0f;
    }
    start_deg = normalize_degrees(start_deg);
    end_deg = normalize_degrees(end_deg);

    if (start_deg <= end_deg) {
        return degrees >= (float)start_deg && degrees <= (float)end_deg;
    }
    return degrees >= (float)start_deg || degrees <= (float)end_deg;
}

/* Convert one polar arc endpoint to canvas coordinates. */
void RadarApp::arc_endpoint(int degrees, int radius, float *x, float *y)
{
    float radians = ((float)degrees - 90.0f) * PI_F / 180.0f;
    *x = (float)RADAR_CENTER + cosf(radians) * (float)radius;
    *y = (float)RADAR_CENTER + sinf(radians) * (float)radius;
}

/* Expand a bounding box to include a point and drawing margin. */
void RadarApp::expand_bounds(float x, float y, float margin,
                          int *min_x, int *min_y, int *max_x, int *max_y)
{
    int left = (int)floorf(x - margin);
    int top = (int)floorf(y - margin);
    int right = (int)ceilf(x + margin);
    int bottom = (int)ceilf(y + margin);

    if (left < *min_x) {
        *min_x = left;
    }
    if (top < *min_y) {
        *min_y = top;
    }
    if (right > *max_x) {
        *max_x = right;
    }
    if (bottom > *max_y) {
        *max_y = bottom;
    }
}

/* Draw a filled curved control band with rounded end caps. */
void RadarApp::draw_control_arc_band(lv_obj_t *canvas, int start_deg, int end_deg,
                                  int radius, float width, uint32_t color)
{
    float half_width = width * 0.5f;
    float inner_radius = (float)radius - half_width;
    float outer_radius = (float)radius + half_width;
    float inner_radius_sq = inner_radius * inner_radius;
    float outer_radius_sq = outer_radius * outer_radius;
    float cap_radius_sq = half_width * half_width;
    float start_x;
    float start_y;
    float end_x;
    float end_y;
    int min_x = RADAR_SIZE;
    int min_y = RADAR_SIZE;
    int max_x = 0;
    int max_y = 0;

    arc_endpoint(start_deg, radius, &start_x, &start_y);
    arc_endpoint(end_deg, radius, &end_x, &end_y);
    expand_bounds(start_x, start_y, half_width + 2.0f, &min_x, &min_y, &max_x, &max_y);
    expand_bounds(end_x, end_y, half_width + 2.0f, &min_x, &min_y, &max_x, &max_y);

    int span = normalize_degrees(end_deg - start_deg);
    for (int step = 0; step <= span; step += 3) {
        float x;
        float y;
        arc_endpoint(normalize_degrees(start_deg + step), radius, &x, &y);
        expand_bounds(x, y, half_width + 2.0f, &min_x, &min_y, &max_x, &max_y);
    }
    if ((span % 3) != 0) {
        float x;
        float y;
        arc_endpoint(end_deg, radius, &x, &y);
        expand_bounds(x, y, half_width + 2.0f, &min_x, &min_y, &max_x, &max_y);
    }

    if (min_x < 0) {
        min_x = 0;
    }
    if (min_y < 0) {
        min_y = 0;
    }
    if (max_x >= RADAR_SIZE) {
        max_x = RADAR_SIZE - 1;
    }
    if (max_y >= RADAR_SIZE) {
        max_y = RADAR_SIZE - 1;
    }

    for (int y = min_y; y <= max_y; ++y) {
        float dy = (float)y + 0.5f - (float)RADAR_CENTER;
        for (int x = min_x; x <= max_x; ++x) {
            float dx = (float)x + 0.5f - (float)RADAR_CENTER;
            float distance_sq = (dx * dx) + (dy * dy);
            bool draw = false;

            if (distance_sq >= inner_radius_sq && distance_sq <= outer_radius_sq) {
                float degrees = atan2f(dy, dx) * 180.0f / PI_F + 90.0f;
                draw = angle_in_range(degrees, start_deg, end_deg);
            }

            float sx = ((float)x + 0.5f) - start_x;
            float sy = ((float)y + 0.5f) - start_y;
            float ex = ((float)x + 0.5f) - end_x;
            float ey = ((float)y + 0.5f) - end_y;
            draw = draw || ((sx * sx) + (sy * sy) <= cap_radius_sq);
            draw = draw || ((ex * ex) + (ey * ey) <= cap_radius_sq);

            if (draw) {
                canvas_set_px_safe(canvas, x, y, color);
            }
        }
    }
}

/* Draw one configured lower-bezel control arc segment. */
void RadarApp::draw_control_arc_segment(lv_obj_t *canvas, int start_deg, int end_deg)
{
    draw_control_arc_band(canvas, start_deg, end_deg, CONTROL_ARC_RADIUS,
                          settings.widths.control_arc, settings.colors.control_arc);
}

/* Draw all lower-bezel control arcs when that UI layer is enabled. */
void RadarApp::draw_control_arcs(lv_obj_t *canvas)
{
    if (!settings.visible.control_arc) {
        return;
    }

    draw_control_arc_segment(canvas, 212, 248);
    draw_control_arc_segment(canvas, 162, 198);
    draw_control_arc_segment(canvas, 112, 148);
}

/* Draw a square brush centred on one canvas pixel. */
void RadarApp::canvas_draw_brush(lv_obj_t *canvas, int x, int y, int width, uint32_t color)
{
    int radius = width / 2;
    for (int yy = -radius; yy <= radius; ++yy) {
        for (int xx = -radius; xx <= radius; ++xx) {
            canvas_set_px_safe(canvas, x + xx, y + yy, color);
        }
    }
}

/* Rasterise a line with a square brush using integer Bresenham steps. */
void RadarApp::canvas_draw_line(lv_obj_t *canvas, int x0, int y0, int x1, int y1, int width, uint32_t color)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        canvas_draw_brush(canvas, x0, y0, width, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* Draw a square brush with LVGL alpha blending. */
void RadarApp::canvas_draw_brush_opa(lv_obj_t *canvas, int x, int y, int width,
                                     uint32_t color, lv_opa_t opa)
{
    int radius = width / 2;
    for (int yy = -radius; yy <= radius; ++yy) {
        for (int xx = -radius; xx <= radius; ++xx) {
            canvas_set_px_opa_safe(canvas, x + xx, y + yy, color, opa);
        }
    }
}

/* Rasterise an alpha-blended line with a square brush. */
void RadarApp::canvas_draw_line_opa(lv_obj_t *canvas, int x0, int y0, int x1, int y1,
                                    int width, uint32_t color, lv_opa_t opa)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        canvas_draw_brush_opa(canvas, x0, y0, width, color, opa);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* Draw a radial line between two radii at the supplied bearing. */
void RadarApp::canvas_draw_radial_line(lv_obj_t *canvas, int degrees, int r0, int r1, int width, uint32_t color)
{
    float rad = ((float)degrees - 90.0f) * PI_F / 180.0f;
    int x0 = RADAR_CENTER + (int)lroundf(cosf(rad) * (float)r0);
    int y0 = RADAR_CENTER + (int)lroundf(sinf(rad) * (float)r0);
    int x1 = RADAR_CENTER + (int)lroundf(cosf(rad) * (float)r1);
    int y1 = RADAR_CENTER + (int)lroundf(sinf(rad) * (float)r1);
    canvas_draw_line(canvas, x0, y0, x1, y1, width, color);
}

/* Draw the square grid clipped so it stays inside the round radar face. */
void RadarApp::draw_square_grid(lv_obj_t *canvas)
{
    if (!settings.visible.radar_grid) {
        return;
    }

    int radius = RADAR_RADIUS - (settings.widths.radar_grid / 2);

    for (int offset = RADAR_GRID_SPACING; offset < RADAR_CENTER; offset += RADAR_GRID_SPACING) {
        if (offset >= radius) {
            break;
        }

        int extent = (int)floorf(sqrtf((float)(radius * radius - offset * offset)));
        int left = RADAR_CENTER - offset;
        int right = RADAR_CENTER + offset;
        int min_pos = RADAR_CENTER - extent;
        int max_pos = RADAR_CENTER + extent;
        canvas_draw_line(canvas, left, min_pos, left, max_pos,
                         settings.widths.radar_grid, settings.colors.radar_grid);
        canvas_draw_line(canvas, right, min_pos, right, max_pos,
                         settings.widths.radar_grid, settings.colors.radar_grid);
        canvas_draw_line(canvas, min_pos, left, max_pos, left,
                         settings.widths.radar_grid, settings.colors.radar_grid);
        canvas_draw_line(canvas, min_pos, right, max_pos, right,
                         settings.widths.radar_grid, settings.colors.radar_grid);
    }
}

/* Draw radial bearing lines and compass ticks around the radar face. */
void RadarApp::draw_compass_scale(lv_obj_t *canvas)
{
    if (settings.visible.radar_radial) {
        int step = settings.ui.radial_degrees > 0 ? settings.ui.radial_degrees : 30;
        for (int degrees = 0; degrees < 360; degrees += step) {
            canvas_draw_radial_line(canvas, degrees, 72, RADAR_RADIUS,
                                    settings.widths.radar_radial, settings.colors.radar_radial);
        }
    }

    int major_step = settings.ui.major_tick_degrees > 0 ? settings.ui.major_tick_degrees : 30;
    int medium_step = settings.ui.medium_tick_degrees > 0 ? settings.ui.medium_tick_degrees : 10;
    int minor_step = settings.ui.minor_tick_degrees > 0 ? settings.ui.minor_tick_degrees : 5;

    if (settings.visible.radar_tick_minor) {
        for (int degrees = 0; degrees < 360; degrees += minor_step) {
            if ((major_step > 0 && degrees % major_step == 0) ||
                (medium_step > 0 && degrees % medium_step == 0)) {
                continue;
            }
            canvas_draw_radial_line(canvas, degrees,
                                    RADAR_RADIUS - settings.ui.minor_tick_length,
                                    RADAR_RADIUS,
                                    settings.widths.radar_tick_minor,
                                    settings.colors.radar_tick_minor);
        }
    }

    if (settings.visible.radar_tick_medium) {
        for (int degrees = 0; degrees < 360; degrees += medium_step) {
            if (major_step > 0 && degrees % major_step == 0) {
                continue;
            }
            canvas_draw_radial_line(canvas, degrees,
                                    RADAR_RADIUS - settings.ui.medium_tick_length,
                                    RADAR_RADIUS,
                                    settings.widths.radar_tick_medium,
                                    settings.colors.radar_tick_medium);
        }
    }

    if (settings.visible.radar_bright) {
        for (int degrees = 0; degrees < 360; degrees += major_step) {
            canvas_draw_radial_line(canvas, degrees,
                                    RADAR_RADIUS - settings.ui.major_tick_length,
                                    RADAR_RADIUS,
                                    settings.widths.radar_bright,
                                    settings.colors.radar_bright);
        }
    }

    if (settings.visible.radar_axis) {
        canvas_draw_line(canvas, RADAR_CENTER - RADAR_RADIUS, RADAR_CENTER,
                         RADAR_CENTER + RADAR_RADIUS, RADAR_CENTER,
                         settings.widths.radar_axis, settings.colors.radar_axis);
        canvas_draw_line(canvas, RADAR_CENTER, RADAR_CENTER - RADAR_RADIUS,
                         RADAR_CENTER, RADAR_CENTER + RADAR_RADIUS,
                         settings.widths.radar_axis, settings.colors.radar_axis);
    }

}

/* Draw the static radar background and map context into the RGB565 canvas. */
void RadarApp::draw_radar_canvas(lv_obj_t *canvas)
{
    /*
     * The radar background and map context are expensive to draw but change only
     * when settings, range, centre, or runway cache state changes. Invalidation
     * is disabled while pixels are written so LVGL does not try to flush a
     * half-drawn canvas.
     */
    lv_display_t *display = lv_obj_get_display(canvas);
    lv_display_enable_invalidation(display, false);

    for (int y = 0; y < RADAR_SIZE; ++y) {
        float dy = (float)y + 0.5f - (float)RADAR_CENTER;
        for (int x = 0; x < RADAR_SIZE; ++x) {
            float dx = (float)x + 0.5f - (float)RADAR_CENTER;
            float distance = sqrtf((dx * dx) + (dy * dy));
            float max_distance = (float)RADAR_CENTER * 1.42f;
            float vignette = 1.0f - fminf(distance / max_distance, 1.0f);
            uint32_t color = mix_color_hex(settings.colors.screen_bg, settings.colors.radar_bg,
                                           0.74f + (vignette * 0.18f));

            if (distance <= RADAR_BG_RADIUS) {
                float glow = 1.0f - (distance / (float)RADAR_BG_RADIUS);
                color = mix_color_hex(color, settings.colors.radar_glow, glow * 0.34f);

                float axis = 0.0f;
                if (settings.visible.radar_axis &&
                    fabsf(dy) < 0.8f && fabsf(dx) < (float)RADAR_RADIUS) {
                    axis = 0.20f;
                }
                if (settings.visible.radar_axis &&
                    fabsf(dx) < 0.8f && fabsf(dy) < (float)RADAR_RADIUS) {
                    axis = 0.16f;
                }

                float ring = 0.0f;
                if (settings.visible.radar_bright) {
                    for (int i = 1; i <= RANGE_RING_COUNT; ++i) {
                        float radius = ((float)RADAR_RADIUS * (float)i) / (float)RANGE_RING_COUNT;
                        float width = (float)settings.widths.radar_bright + (i == RANGE_RING_COUNT ? 1.0f : 0.0f);
                        float strength = i == RANGE_RING_COUNT ? 0.92f : 0.55f;
                        ring = fmaxf(ring, ring_alpha(distance, radius, width, 1.6f) * strength);
                    }
                }

                float amount = fmaxf(axis, ring);
                color = mix_color_hex(color, settings.colors.radar_bright, amount);
            }

            canvas_set_px_safe(canvas, x, y, color);
        }
    }

    draw_square_grid(canvas);
    int range_mi = get_current_range_mi();
    size_t boundary_segments = draw_country_boundaries(canvas, range_mi);
    size_t airport_count = draw_airports(canvas, range_mi);
    size_t runway_count = draw_airport_runways(canvas, range_mi);
    draw_compass_scale(canvas);
    draw_control_arcs(canvas);

    lv_display_enable_invalidation(display, true);
    lv_obj_invalidate(canvas);
    ESP_LOGI(TAG, "Drew static map: %u boundary segments, %u airports and %u runways in %dMI range",
             (unsigned)boundary_segments, (unsigned)airport_count, (unsigned)runway_count, range_mi);
}

/* Allocate and initialise the static radar background canvas. */
lv_obj_t *RadarApp::create_radar_canvas(lv_obj_t *parent)
{
    size_t buf_size = LV_CANVAS_BUF_SIZE(RADAR_SIZE, RADAR_SIZE, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    radar_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!radar_canvas_buf) {
        radar_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_8BIT);
    }
    if (!radar_canvas_buf) {
        ESP_LOGE(TAG, "Failed to allocate radar canvas buffer (%u bytes)", (unsigned)buf_size);
        return NULL;
    }

    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, radar_canvas_buf, RADAR_SIZE, RADAR_SIZE, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(canvas, RADAR_SIZE, RADAR_SIZE);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    draw_radar_canvas(canvas);
    return canvas;
}

/* Allocate and initialise the transparent dynamic aircraft overlay canvas. */
lv_obj_t *RadarApp::create_aircraft_canvas(lv_obj_t *parent)
{
    size_t buf_size = LV_CANVAS_BUF_SIZE(RADAR_SIZE, RADAR_SIZE, 32, LV_DRAW_BUF_STRIDE_ALIGN);
    aircraft_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!aircraft_canvas_buf) {
        aircraft_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_8BIT);
    }
    if (!aircraft_canvas_buf) {
        ESP_LOGE(TAG, "Failed to allocate aircraft canvas buffer (%u bytes)", (unsigned)buf_size);
        return NULL;
    }

    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, aircraft_canvas_buf, RADAR_SIZE, RADAR_SIZE, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_size(canvas, RADAR_SIZE, RADAR_SIZE);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_set_style_bg_opa(canvas, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(canvas, 0, 0);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_fill_bg(canvas, lv_color_hex(0x000000), LV_OPA_TRANSP);
    return canvas;
}

/* Create fixed compass heading labels around the radar face. */
void RadarApp::create_heading_labels(lv_obj_t *radar)
{
    for (int i = 0; i < HEADING_LABEL_COUNT; ++i) {
        int degrees = i * 30;
        char text[8];
        snprintf(text, sizeof(text), "%03d", degrees);

        lv_obj_t *label = make_label(radar, text,
                                     configured_label_font(settings.label_styles.heading_label),
                                     settings.colors.heading_label);
        heading_labels[i] = label;
        lv_obj_set_width(label, 34);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_opa(label, LV_OPA_70, 0);

        float rad = ((float)degrees - 90.0f) * PI_F / 180.0f;
        int x = RADAR_CENTER + (int)lroundf(cosf(rad) * (float)HEADING_LABEL_RADIUS);
        int y = RADAR_CENTER + (int)lroundf(sinf(rad) * (float)HEADING_LABEL_RADIUS);
        lv_obj_set_pos(label, x - 17, y - 7);
    }
}

/* Create labels that show the mileage represented by each range ring. */
void RadarApp::create_range_ring_labels(lv_obj_t *radar)
{
    const float label_angle = -38.0f * PI_F / 180.0f;
    for (size_t i = 0; i < RANGE_RING_COUNT; ++i) {
        int radius = (RADAR_RADIUS * (int)(i + 1)) / RANGE_RING_COUNT;
        int x = RADAR_CENTER + (int)lroundf(cosf(label_angle) * (float)radius);
        int y = RADAR_CENTER + (int)lroundf(sinf(label_angle) * (float)radius);

        range_ring_labels[i] = make_label(radar, "",
                                          configured_label_font(settings.label_styles.range_label),
                                          settings.colors.range_label);
        lv_obj_set_width(range_ring_labels[i], 54);
        lv_label_set_long_mode(range_ring_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_opa(range_ring_labels[i], LV_OPA_80, 0);
        lv_obj_set_style_text_align(range_ring_labels[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(range_ring_labels[i], x + 5, y - 8);
    }

    update_range_ring_labels();
}

/* Create the reusable pool of aircraft callsign, detail, and trend widgets. */
void RadarApp::create_aircraft_markers(lv_obj_t *radar)
{
    for (size_t i = 0; i < MAX_AIRCRAFT_LABELS; ++i) {
        markers[i].callsign_label = make_label(radar, "",
                                               configured_label_font(settings.label_styles.aircraft),
                                               settings.colors.text_primary);
        lv_obj_set_width(markers[i].callsign_label, 124);
        lv_label_set_long_mode(markers[i].callsign_label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_opa(markers[i].callsign_label, LV_OPA_80, 0);
        lv_obj_add_flag(markers[i].callsign_label, LV_OBJ_FLAG_HIDDEN);

        markers[i].detail_label = make_label(radar, "",
                                             configured_label_font(settings.label_styles.aircraft, -2),
                                             settings.colors.text_secondary);
        lv_obj_set_width(markers[i].detail_label, 124);
        lv_obj_set_height(markers[i].detail_label, 30);
        lv_label_set_long_mode(markers[i].detail_label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_opa(markers[i].detail_label, LV_OPA_70, 0);
        lv_obj_add_flag(markers[i].detail_label, LV_OBJ_FLAG_HIDDEN);

        markers[i].trend_icon = lv_obj_create(radar);
        lv_obj_remove_style_all(markers[i].trend_icon);
        markers[i].trend_points[0] = {1, 8};
        markers[i].trend_points[1] = {5, 1};
        markers[i].trend_points[2] = {9, 8};
        lv_obj_set_size(markers[i].trend_icon, 10, 10);
        lv_obj_set_style_bg_opa(markers[i].trend_icon, LV_OPA_TRANSP, 0);
        lv_obj_set_style_line_color(markers[i].trend_icon, lv_color_hex(settings.colors.climb_triangle), 0);
        lv_obj_set_style_line_opa(markers[i].trend_icon, LV_OPA_80, 0);
        lv_obj_add_event_cb(markers[i].trend_icon, trend_triangle_draw_event,
                            LV_EVENT_DRAW_MAIN, &markers[i]);
        lv_obj_add_flag(markers[i].trend_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Create the reusable pool of subtle airport ICAO labels. */
void RadarApp::create_airport_labels(lv_obj_t *radar)
{
    for (size_t i = 0; i < MAX_AIRPORT_LABELS; ++i) {
        airport_labels[i] = make_label(radar, "",
                                       configured_label_font(settings.label_styles.airport),
                                       settings.colors.airport);
        lv_obj_set_width(airport_labels[i], 38);
        lv_label_set_long_mode(airport_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_opa(airport_labels[i], LV_OPA_50, 0);
        lv_obj_set_style_text_align(airport_labels[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_add_flag(airport_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Draw a small circular aircraft marker with a softer edge. */
void RadarApp::draw_aircraft_dot(lv_obj_t *canvas, int x, int y, uint32_t color)
{
    for (int yy = -3; yy <= 3; ++yy) {
        for (int xx = -3; xx <= 3; ++xx) {
            int radius_sq = (xx * xx) + (yy * yy);
            if (radius_sq > 9) {
                continue;
            }

            lv_opa_t opa = radius_sq <= 4 ? LV_OPA_COVER : LV_OPA_70;
            lv_canvas_set_px(canvas, x + xx, y + yy, lv_color_hex(color), opa);
        }
    }
}

/* Draw an ATC-style square aircraft head symbol at the current target position. */
void RadarApp::draw_aircraft_head_symbol(lv_obj_t *canvas, int x, int y, uint32_t color)
{
    canvas_draw_line_opa(canvas, x - 3, y - 3, x + 3, y - 3, 1, color, LV_OPA_COVER);
    canvas_draw_line_opa(canvas, x + 3, y - 3, x + 3, y + 3, 1, color, LV_OPA_COVER);
    canvas_draw_line_opa(canvas, x + 3, y + 3, x - 3, y + 3, 1, color, LV_OPA_COVER);
    canvas_draw_line_opa(canvas, x - 3, y + 3, x - 3, y - 3, 1, color, LV_OPA_COVER);
}

/* Draw one older aircraft position as a subtle dot. */
void RadarApp::draw_aircraft_history_dot(lv_obj_t *canvas, int x, int y, uint32_t color, lv_opa_t opa)
{
    canvas_set_px_opa_safe(canvas, x, y, color, opa);
    canvas_set_px_opa_safe(canvas, x - 1, y, color, opa);
    canvas_set_px_opa_safe(canvas, x + 1, y, color, opa);
    canvas_set_px_opa_safe(canvas, x, y - 1, color, opa);
    canvas_set_px_opa_safe(canvas, x, y + 1, color, opa);
}

/* Draw a compact heading indicator that shows direction only, not predicted position. */
void RadarApp::draw_aircraft_heading_indicator(lv_obj_t *canvas, int x, int y, int heading_deg,
                                               uint32_t color, int width, bool arrow_head)
{
    if (heading_deg < 0) {
        return;
    }

    float rad = ((float)heading_deg - 90.0f) * PI_F / 180.0f;
    int tail_x = x + (int)lroundf(cosf(rad) * 5.0f);
    int tail_y = y + (int)lroundf(sinf(rad) * 5.0f);
    int tip_x = x + (int)lroundf(cosf(rad) * 16.0f);
    int tip_y = y + (int)lroundf(sinf(rad) * 16.0f);
    canvas_draw_line_opa(canvas, tail_x, tail_y, tip_x, tip_y, width, color, LV_OPA_70);

    if (!arrow_head) {
        return;
    }

    float left_rad = ((float)heading_deg + 145.0f - 90.0f) * PI_F / 180.0f;
    float right_rad = ((float)heading_deg - 145.0f - 90.0f) * PI_F / 180.0f;
    int left_x = tip_x + (int)lroundf(cosf(left_rad) * 5.0f);
    int left_y = tip_y + (int)lroundf(sinf(left_rad) * 5.0f);
    int right_x = tip_x + (int)lroundf(cosf(right_rad) * 5.0f);
    int right_y = tip_y + (int)lroundf(sinf(right_rad) * 5.0f);

    canvas_draw_line_opa(canvas, tip_x, tip_y, left_x, left_y, width, color, LV_OPA_70);
    canvas_draw_line_opa(canvas, tip_x, tip_y, right_x, right_y, width, color, LV_OPA_70);
}

/* Write one alpha-blended canvas pixel when it lies inside the radar canvas. */
void RadarApp::canvas_set_px_opa_safe(lv_obj_t *canvas, int x, int y, uint32_t color, lv_opa_t opa)
{
    if (x >= 0 && x < RADAR_SIZE && y >= 0 && y < RADAR_SIZE) {
        lv_canvas_set_px(canvas, x, y, lv_color_hex(color), opa);
    }
}

/* Draw a subtle cross marker for an airport. */
void RadarApp::draw_airport_marker(lv_obj_t *canvas, int x, int y)
{
    uint32_t color = settings.colors.airport;
    int width = settings.widths.airport;
    int arm = 3 + (width / 2);
    canvas_draw_line_opa(canvas, x - arm, y, x + arm, y, width, color, LV_OPA_50);
    canvas_draw_line_opa(canvas, x, y - arm, x, y + arm, width, color, LV_OPA_50);
}

/* Draw a small weather glyph using only canvas primitives. */
void RadarApp::draw_airport_weather_icon(lv_obj_t *canvas, int x, int y, int weather_code)
{
    uint32_t color = settings.colors.airport_weather;
    lv_opa_t opa = LV_OPA_70;
    int size = clamp_int(settings.airport_weather_icon_size,
                         RADAR_SETTINGS_MIN_AIRPORT_WEATHER_ICON_SIZE,
                         RADAR_SETTINGS_MAX_AIRPORT_WEATHER_ICON_SIZE);
    float scale = (float)size / (float)RADAR_SETTINGS_DEFAULT_AIRPORT_WEATHER_ICON_SIZE;
    int cx = x - (size / 2) - 8;
    int cy = y + (size / 2) + 8;

    auto s = [&](float value) {
        int scaled = (int)lroundf(value * scale);
        if (value > 0.0f && scaled < 1) {
            scaled = 1;
        }
        if (value < 0.0f && scaled > -1) {
            scaled = -1;
        }
        return scaled;
    };

    auto dot = [&](int px, int py) {
        canvas_set_px_opa_safe(canvas, px, py, color, opa);
    };
    auto small_circle = [&](int px, int py, int radius) {
        if (radius < 1) {
            radius = 1;
        }
        for (int yy = -radius; yy <= radius; ++yy) {
            for (int xx = -radius; xx <= radius; ++xx) {
                int d = (xx * xx) + (yy * yy);
                if (d >= (radius - 1) * (radius - 1) && d <= radius * radius) {
                    dot(px + xx, py + yy);
                }
            }
        }
    };
    auto filled_circle = [&](int px, int py, int radius) {
        if (radius < 1) {
            radius = 1;
        }
        for (int yy = -radius; yy <= radius; ++yy) {
            for (int xx = -radius; xx <= radius; ++xx) {
                if ((xx * xx) + (yy * yy) <= radius * radius) {
                    dot(px + xx, py + yy);
                }
            }
        }
    };
    auto cloud = [&]() {
        filled_circle(cx - s(3), cy + s(1), s(3));
        filled_circle(cx + s(1), cy - s(1), s(4));
        filled_circle(cx + s(6), cy + s(1), s(3));
        canvas_draw_line_opa(canvas, cx - s(6), cy + s(4), cx + s(8), cy + s(4), 1, color, opa);
    };
    auto rain = [&]() {
        cloud();
        canvas_draw_line_opa(canvas, cx - s(4), cy + s(7), cx - s(6), cy + s(11), 1, color, opa);
        canvas_draw_line_opa(canvas, cx + s(1), cy + s(7), cx - s(1), cy + s(11), 1, color, opa);
        canvas_draw_line_opa(canvas, cx + s(6), cy + s(7), cx + s(4), cy + s(11), 1, color, opa);
    };

    if (weather_code == 0) {
        small_circle(cx, cy, s(4));
        canvas_draw_line_opa(canvas, cx, cy - s(8), cx, cy - s(6), 1, color, opa);
        canvas_draw_line_opa(canvas, cx, cy + s(6), cx, cy + s(8), 1, color, opa);
        canvas_draw_line_opa(canvas, cx - s(8), cy, cx - s(6), cy, 1, color, opa);
        canvas_draw_line_opa(canvas, cx + s(6), cy, cx + s(8), cy, 1, color, opa);
        return;
    }

    if (weather_code == 1 || weather_code == 2) {
        small_circle(cx - s(5), cy - s(4), s(3));
        cloud();
        return;
    }

    if (weather_code == 3) {
        cloud();
        return;
    }

    if (weather_code == 45 || weather_code == 48) {
        canvas_draw_line_opa(canvas, cx - s(7), cy - s(2), cx + s(8), cy - s(2), 1, color, opa);
        canvas_draw_line_opa(canvas, cx - s(8), cy + s(2), cx + s(7), cy + s(2), 1, color, opa);
        canvas_draw_line_opa(canvas, cx - s(6), cy + s(6), cx + s(8), cy + s(6), 1, color, opa);
        return;
    }

    if ((weather_code >= 51 && weather_code <= 67) ||
        (weather_code >= 80 && weather_code <= 82)) {
        rain();
        return;
    }

    if (weather_code >= 71 && weather_code <= 77) {
        cloud();
        canvas_draw_line_opa(canvas, cx - s(4), cy + s(8), cx - s(4), cy + s(10), 1, color, opa);
        canvas_draw_line_opa(canvas, cx - s(5), cy + s(9), cx - s(3), cy + s(9), 1, color, opa);
        canvas_draw_line_opa(canvas, cx + s(3), cy + s(8), cx + s(3), cy + s(10), 1, color, opa);
        canvas_draw_line_opa(canvas, cx + s(2), cy + s(9), cx + s(4), cy + s(9), 1, color, opa);
        return;
    }

    if (weather_code >= 95 && weather_code <= 99) {
        cloud();
        canvas_draw_line_opa(canvas, cx + s(1), cy + s(6), cx - s(2), cy + s(11), 1, color, opa);
        canvas_draw_line_opa(canvas, cx - s(2), cy + s(11), cx + s(3), cy + s(9), 1, color, opa);
        canvas_draw_line_opa(canvas, cx + s(3), cy + s(9), cx, cy + s(14), 1, color, opa);
        return;
    }

    cloud();
}

/* Read cached weather for an airport record index. */
bool RadarApp::get_airport_weather(size_t airport_index, airport_weather_t *weather)
{
    if (!settings.show_airport_weather || !weather || !airport_weather_mutex) {
        return false;
    }
    bool found = false;
    if (xSemaphoreTake(airport_weather_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
        for (size_t i = 0; i < airport_weather_count; ++i) {
            if (airport_weather[i].valid && airport_weather[i].airport_index == airport_index) {
                *weather = airport_weather[i];
                found = true;
                break;
            }
        }
        xSemaphoreGive(airport_weather_mutex);
    }
    return found;
}

/* Draw visible country boundary segments for the current centre and range. */
size_t RadarApp::draw_country_boundaries(lv_obj_t *canvas, int range_mi)
{
    if (!settings.show_countries || !settings.visible.country_boundary || range_mi <= 0) {
        return 0;
    }

    double center_lat = settings.center_lat;
    double center_lon = settings.center_lon;
    get_radar_center(&center_lat, &center_lon);

    double lat_span = ((double)range_mi / 69.0) * 1.15;
    double cos_lat = fabs(cos(deg_to_rad(center_lat)));
    double lon_span = ((double)range_mi / (69.0 * fmax(cos_lat, 0.15))) * 1.15;
    int32_t min_lat_e5 = (int32_t)lround((center_lat - lat_span) * COUNTRY_BOUNDARY_COORD_SCALE);
    int32_t max_lat_e5 = (int32_t)lround((center_lat + lat_span) * COUNTRY_BOUNDARY_COORD_SCALE);
    int32_t min_lon_e5 = (int32_t)lround((center_lon - lon_span) * COUNTRY_BOUNDARY_COORD_SCALE);
    int32_t max_lon_e5 = (int32_t)lround((center_lon + lon_span) * COUNTRY_BOUNDARY_COORD_SCALE);

    size_t segments = 0;
    uint32_t color = settings.colors.country_boundary;
    double miles_per_lon_degree = 69.0 * cos_lat;

    for (size_t i = 0; i < boundary_line_count; ++i) {
        const boundary_line_t *line = &boundary_lines[i];
        if (line->point_count < 2 ||
            (size_t)line->first_point + (size_t)line->point_count > boundary_point_count ||
            line->max_lat_e5 < min_lat_e5 || line->min_lat_e5 > max_lat_e5 ||
            line->max_lon_e5 < min_lon_e5 || line->min_lon_e5 > max_lon_e5) {
            continue;
        }

        int prev_x = 0;
        int prev_y = 0;
        int32_t prev_lat_e5 = 0;
        int32_t prev_lon_e5 = 0;
        bool have_prev = false;
        bool prev_visible = false;

        for (uint16_t point_index = 0; point_index < line->point_count; ++point_index) {
            const boundary_point_t *point = &boundary_points[line->first_point + point_index];
            double lat = (double)point->lat_e5 / COUNTRY_BOUNDARY_COORD_SCALE;
            double lon = (double)point->lon_e5 / COUNTRY_BOUNDARY_COORD_SCALE;
            double east_miles = (lon - center_lon) * miles_per_lon_degree;
            double north_miles = (lat - center_lat) * 69.0;
            int x = RADAR_CENTER + (int)lround((east_miles * (double)RADAR_RADIUS) / (double)range_mi);
            int y = RADAR_CENTER - (int)lround((north_miles * (double)RADAR_RADIUS) / (double)range_mi);
            bool visible = x >= -4 && x <= RADAR_SIZE + 4 && y >= -4 && y <= RADAR_SIZE + 4;

            if (have_prev) {
                int32_t seg_min_lat = point->lat_e5 < prev_lat_e5 ? point->lat_e5 : prev_lat_e5;
                int32_t seg_max_lat = point->lat_e5 > prev_lat_e5 ? point->lat_e5 : prev_lat_e5;
                int32_t seg_min_lon = point->lon_e5 < prev_lon_e5 ? point->lon_e5 : prev_lon_e5;
                int32_t seg_max_lon = point->lon_e5 > prev_lon_e5 ? point->lon_e5 : prev_lon_e5;
                bool segment_in_range = seg_max_lat >= min_lat_e5 && seg_min_lat <= max_lat_e5 &&
                                        seg_max_lon >= min_lon_e5 && seg_min_lon <= max_lon_e5;
                bool crosses_dateline = abs(point->lon_e5 - prev_lon_e5) > 180 * (int32_t)COUNTRY_BOUNDARY_COORD_SCALE;
                if (segment_in_range && !crosses_dateline && (visible || prev_visible)) {
                    canvas_draw_line_opa(canvas, prev_x, prev_y, x, y,
                                         settings.widths.country_boundary, color, LV_OPA_40);
                    ++segments;
                }
            }

            prev_x = x;
            prev_y = y;
            prev_lat_e5 = point->lat_e5;
            prev_lon_e5 = point->lon_e5;
            prev_visible = visible;
            have_prev = true;
        }
    }

    return segments;
}

/* Draw airport markers within the current radar range. */
size_t RadarApp::draw_airports(lv_obj_t *canvas, int range_mi)
{
    if (!settings.show_airports || !settings.visible.airport) {
        return 0;
    }

    double center_lat = settings.center_lat;
    double center_lon = settings.center_lon;
    get_radar_center(&center_lat, &center_lon);
    double lat_span = (double)range_mi / 69.0;
    double cos_lat = fabs(cos(deg_to_rad(center_lat)));
    double lon_span = (double)range_mi / (69.0 * fmax(cos_lat, 0.15));
    size_t drawn = 0;

    for (size_t i = 0; i < airport_record_count; ++i) {
        double lat = (double)airport_records[i].lat_e6 / 1000000.0;
        double lon = (double)airport_records[i].lon_e6 / 1000000.0;
        if (lat < center_lat - lat_span || lat > center_lat + lat_span ||
            lon < center_lon - lon_span || lon > center_lon + lon_span) {
            continue;
        }

        double distance = distance_miles(center_lat, center_lon, lat, lon);
        if (distance < 0.0 || distance > (double)range_mi) {
            continue;
        }

        int bearing = bearing_degrees(center_lat, center_lon, lat, lon);
        float rad = ((float)bearing - 90.0f) * PI_F / 180.0f;
        int radius = (int)lround(((double)RADAR_RADIUS * distance) / (double)range_mi);
        int x = RADAR_CENTER + (int)lroundf(cosf(rad) * (float)radius);
        int y = RADAR_CENTER + (int)lroundf(sinf(rad) * (float)radius);
        draw_airport_marker(canvas, x, y);
        airport_weather_t weather = {};
        if (get_airport_weather(i, &weather)) {
            draw_airport_weather_icon(canvas, x, y, weather.weather_code);
        }
        ++drawn;
    }

    return drawn;
}

/* Draw cached runway centre lines when centred on an airport. */
size_t RadarApp::draw_airport_runways(lv_obj_t *canvas, int range_mi)
{
    if (!canvas || range_mi <= 0 ||
        !settings.show_airport_runways ||
        !settings.visible.runway ||
        settings.center_source != RADAR_CENTER_SOURCE_AIRPORT) {
        return 0;
    }

    char icao[5];
    if (!normalize_icao_code(icao, sizeof(icao), settings.center_airport_code)) {
        return 0;
    }

    airport_runway_cache_t cache = {};
    if (!get_active_runway_cache(&cache) || strcmp(cache.icao, icao) != 0) {
        return 0;
    }

    double center_lat = settings.center_airport_lat;
    double center_lon = settings.center_airport_lon;
    double cos_lat = fabs(cos(deg_to_rad(center_lat)));
    double miles_per_lon_degree = 69.0 * fmax(cos_lat, 0.15);
    uint32_t color = settings.colors.runway;
    int width = settings.widths.runway;
    size_t drawn = 0;

    for (size_t i = 0; i < cache.count && i < MAX_AIRPORT_RUNWAYS; ++i) {
        const airport_runway_t *runway = &cache.runways[i];
        double le_lat = (double)runway->le_lat_e6 / 1000000.0;
        double le_lon = (double)runway->le_lon_e6 / 1000000.0;
        double he_lat = (double)runway->he_lat_e6 / 1000000.0;
        double he_lon = (double)runway->he_lon_e6 / 1000000.0;
        double le_east_mi = (le_lon - center_lon) * miles_per_lon_degree;
        double le_north_mi = (le_lat - center_lat) * 69.0;
        double he_east_mi = (he_lon - center_lon) * miles_per_lon_degree;
        double he_north_mi = (he_lat - center_lat) * 69.0;
        int x0 = RADAR_CENTER + (int)lround((le_east_mi * (double)RADAR_RADIUS) / (double)range_mi);
        int y0 = RADAR_CENTER - (int)lround((le_north_mi * (double)RADAR_RADIUS) / (double)range_mi);
        int x1 = RADAR_CENTER + (int)lround((he_east_mi * (double)RADAR_RADIUS) / (double)range_mi);
        int y1 = RADAR_CENTER - (int)lround((he_north_mi * (double)RADAR_RADIUS) / (double)range_mi);

        bool visible = (x0 >= -8 && x0 <= RADAR_SIZE + 8 && y0 >= -8 && y0 <= RADAR_SIZE + 8) ||
                       (x1 >= -8 && x1 <= RADAR_SIZE + 8 && y1 >= -8 && y1 <= RADAR_SIZE + 8);
        if (!visible) {
            continue;
        }

        canvas_draw_line_opa(canvas, x0, y0, x1, y1, width, color, LV_OPA_70);
        ++drawn;
    }

    return drawn;
}

/* Position and show ICAO labels for nearby airports. */
void RadarApp::update_airport_labels(int range_mi)
{
    for (size_t i = 0; i < MAX_AIRPORT_LABELS; ++i) {
        if (airport_labels[i]) {
            lv_obj_add_flag(airport_labels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!settings.show_airports || !settings.visible.airport) {
        return;
    }

    double center_lat = settings.center_lat;
    double center_lon = settings.center_lon;
    get_radar_center(&center_lat, &center_lon);
    double lat_span = (double)range_mi / 69.0;
    double cos_lat = fabs(cos(deg_to_rad(center_lat)));
    double lon_span = (double)range_mi / (69.0 * fmax(cos_lat, 0.15));
    size_t label_index = 0;

    for (size_t i = 0; i < airport_record_count && label_index < MAX_AIRPORT_LABELS; ++i) {
        if (airport_records[i].code[0] == '\0') {
            continue;
        }

        double lat = (double)airport_records[i].lat_e6 / 1000000.0;
        double lon = (double)airport_records[i].lon_e6 / 1000000.0;
        if (lat < center_lat - lat_span || lat > center_lat + lat_span ||
            lon < center_lon - lon_span || lon > center_lon + lon_span) {
            continue;
        }

        double distance = distance_miles(center_lat, center_lon, lat, lon);
        if (distance < 0.0 || distance > (double)range_mi) {
            continue;
        }

        int bearing = bearing_degrees(center_lat, center_lon, lat, lon);
        float rad = ((float)bearing - 90.0f) * PI_F / 180.0f;
        int radius = (int)lround(((double)RADAR_RADIUS * distance) / (double)range_mi);
        int x = RADAR_CENTER + (int)lroundf(cosf(rad) * (float)radius);
        int y = RADAR_CENTER + (int)lroundf(sinf(rad) * (float)radius);

        lv_obj_t *label = airport_labels[label_index++];
        lv_label_set_text(label, airport_records[i].code);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);

        int label_x = x + 6;
        int label_y = y - 16;
        if (label_x > RADAR_SIZE - 38) {
            label_x = x - 40;
        }
        if (label_x < 0) {
            label_x = 0;
        }
        if (label_y < 0) {
            label_y = y + 4;
        }
        if (label_y > RADAR_SIZE - 14) {
            label_y = RADAR_SIZE - 14;
        }
        lv_obj_set_pos(label, label_x, label_y);
    }
}

/* Redraw the transparent aircraft overlay without rebuilding static map context. */
void RadarApp::update_aircraft_plot(const aircraft_data_t *snapshot, size_t count, int range_mi)
{
    if (!aircraft_canvas) {
        return;
    }

    /*
     * Aircraft positions change every fetch, but countries, airports, and
     * runways do not. Keep this path short so a successful fetch does not block
     * the LVGL timer that animates the sweep line.
     */
    lv_display_t *display = lv_obj_get_display(aircraft_canvas);
    lv_display_enable_invalidation(display, false);
    lv_canvas_fill_bg(aircraft_canvas, lv_color_hex(0x000000), LV_OPA_TRANSP);

    const bool show_history = range_index < MAX_RANGE_SETTINGS &&
                              settings.ranges[range_index].show_history_trail;
    for (size_t i = 0; i < count; ++i) {
        if (snapshot[i].distance_mi > (float)range_mi) {
            break;
        }
        const aircraft_ui_state_t *state = aircraft_ui_state ? &aircraft_ui_state[i] : nullptr;
        if (!state) {
            continue;
        }
        if (!state->visible) {
            continue;
        }

        if (show_history) {
            aircraft_track_history_t *track = track_history_for_aircraft(&snapshot[i]);
            if (track) {
                for (size_t h = 1; h < track->count && h < AIRCRAFT_HISTORY_POINTS; ++h) {
                    if (track->points[h].distance_mi > (float)range_mi) {
                        continue;
                    }
                    float hist_rad = ((float)track->points[h].bearing_deg - 90.0f) * PI_F / 180.0f;
                    int hist_distance = (int)(((float)RADAR_RADIUS * track->points[h].distance_mi) /
                                              (float)range_mi);
                    int hist_x = RADAR_CENTER + (int)(cosf(hist_rad) * hist_distance);
                    int hist_y = RADAR_CENTER + (int)(sinf(hist_rad) * hist_distance);
                    int opa = state->dimmed ? LV_OPA_20 : (LV_OPA_60 - ((int)h * 4));
                    if (opa < LV_OPA_20) {
                        opa = LV_OPA_20;
                    }
                    draw_aircraft_history_dot(aircraft_canvas, hist_x, hist_y, state->color, (lv_opa_t)opa);
                }
            }
        }
        if (settings.show_aircraft_heading &&
            settings.visible.aircraft_heading &&
            settings.aircraft_heading_style != RADAR_HEADING_STYLE_NONE) {
            draw_aircraft_heading_indicator(aircraft_canvas, state->x, state->y, snapshot[i].heading_deg,
                                            state->color, settings.widths.aircraft_heading,
                                            settings.aircraft_heading_style == RADAR_HEADING_STYLE_ARROW);
        }
        draw_aircraft_head_symbol(aircraft_canvas, state->x, state->y, state->color);
    }

    lv_display_enable_invalidation(display, true);
    lv_obj_invalidate(aircraft_canvas);
}

/* Update aircraft callsign/detail labels and climb/descent indicators. */
const lv_font_t *RadarApp::aircraft_label_font(bool detail, bool emphasised) const
{
    const label_font_setting_t &style = emphasised ?
                                        settings.label_styles.aircraft_notification :
                                        settings.label_styles.aircraft;
    return configured_label_font(style, detail ? -2 : 0);
}

void RadarApp::update_aircraft_labels(const aircraft_data_t *snapshot, size_t count, int range_mi)
{
    for (size_t i = 0; i < MAX_AIRCRAFT_LABELS; ++i) {
        markers[i].visible = false;
        markers[i].highlighted = false;
    }

    size_t label_index = 0;
    for (size_t i = 0; i < count; ++i) {
        if (snapshot[i].distance_mi > (float)range_mi) {
            break;
        }
        const aircraft_ui_state_t *state = aircraft_ui_state ? &aircraft_ui_state[i] : nullptr;
        if (!state) {
            continue;
        }
        if (!state->visible || state->label_slot < 0 ||
            state->label_slot >= (int)MAX_AIRCRAFT_LABELS) {
            continue;
        }
        const notification_setting_t *notification =
            state->notification_index >= 0 ? &settings.notifications[state->notification_index] : nullptr;

        radar_marker_t *marker = &markers[state->label_slot];
        if ((size_t)state->label_slot >= label_index) {
            label_index = (size_t)state->label_slot + 1;
        }
        marker->data = snapshot[i];
        marker->visible = true;
        marker->highlighted = false;

        lv_obj_clear_flag(marker->callsign_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(marker->detail_label, LV_OBJ_FLAG_HIDDEN);
        char callsign_text[32];
        snprintf(callsign_text, sizeof(callsign_text), "%s %s",
                 snapshot[i].callsign[0] ? snapshot[i].callsign : "--------",
                 snapshot[i].type[0] ? snapshot[i].type : "----");
        const char *current_callsign = lv_label_get_text(marker->callsign_label);
        if (!current_callsign || strcmp(current_callsign, callsign_text) != 0) {
            lv_label_set_text(marker->callsign_label, callsign_text);
        }

        char altitude_text[16];
        if (snapshot[i].altitude_ft <= 0) {
            snprintf(altitude_text, sizeof(altitude_text), "GND");
        } else if (snapshot[i].altitude_ft >= 18000) {
            snprintf(altitude_text, sizeof(altitude_text), "%03d",
                     (snapshot[i].altitude_ft + 50) / 100);
        } else {
            snprintf(altitude_text, sizeof(altitude_text), "%d", snapshot[i].altitude_ft);
        }

        char detail_text[32];
        snprintf(detail_text, sizeof(detail_text), "%s\n%d", altitude_text,
                 snapshot[i].speed_kt >= 0 ? snapshot[i].speed_kt : 0);
        const char *current_detail = lv_label_get_text(marker->detail_label);
        if (!current_detail || strcmp(current_detail, detail_text) != 0) {
            lv_label_set_text(marker->detail_label, detail_text);
        }
        lv_obj_set_style_text_color(marker->callsign_label, lv_color_hex(state->color), 0);
        lv_obj_set_style_text_color(marker->detail_label, lv_color_hex(state->color), 0);
        bool notification_highlight = notification != nullptr;
        lv_obj_set_style_text_font(marker->callsign_label,
                                   aircraft_label_font(false, notification_highlight), 0);
        lv_obj_set_style_text_font(marker->detail_label,
                                   aircraft_label_font(true, notification_highlight), 0);
        lv_obj_set_style_text_opa(marker->callsign_label,
                                  state->dimmed ? LV_OPA_40 :
                                  (notification && notification->bold_text ? LV_OPA_COVER : LV_OPA_80), 0);
        lv_obj_set_style_text_opa(marker->detail_label,
                                  state->dimmed ? LV_OPA_30 :
                                  (notification && notification->bold_text ? LV_OPA_90 : LV_OPA_70), 0);

        int label_x = state->label_x;
        int label_y = state->label_y;

        int trend = 0;
        if (settings.show_climb_descent && snapshot[i].vertical_rate_fpm != INT_MIN &&
            abs(snapshot[i].vertical_rate_fpm) >= 100) {
            trend = snapshot[i].vertical_rate_fpm > 0 ? 1 : -1;
        }

        lv_obj_set_pos(marker->callsign_label, label_x, label_y);
        if (trend != 0) {
            if (trend > 0) {
                marker->trend_points[0] = {1, 8};
                marker->trend_points[1] = {5, 1};
                marker->trend_points[2] = {9, 8};
                uint32_t trend_color = settings.colors.climb_triangle;
                lv_obj_set_style_line_color(marker->trend_icon,
                                            lv_color_hex(state->dimmed ? dim_colour(trend_color) : trend_color), 0);
            } else {
                marker->trend_points[0] = {1, 1};
                marker->trend_points[1] = {5, 8};
                marker->trend_points[2] = {9, 1};
                uint32_t trend_color = settings.colors.descent_triangle;
                lv_obj_set_style_line_color(marker->trend_icon,
                                            lv_color_hex(state->dimmed ? dim_colour(trend_color) : trend_color), 0);
            }
            lv_obj_set_style_line_opa(marker->trend_icon, state->dimmed ? LV_OPA_40 : LV_OPA_COVER, 0);
            lv_obj_set_pos(marker->trend_icon, label_x, label_y + 15);
            lv_obj_clear_flag(marker->trend_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(marker->detail_label, label_x + 13, label_y + 13);
        } else {
            lv_obj_add_flag(marker->trend_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(marker->detail_label, label_x, label_y + 13);
        }
    }

    for (size_t i = label_index; i < MAX_AIRCRAFT_LABELS; ++i) {
        lv_obj_add_flag(markers[i].callsign_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(markers[i].detail_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(markers[i].trend_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Show every configured notification rule that matches visible aircraft. */
void RadarApp::update_notification_banner(const aircraft_data_t *snapshot, size_t count, int range_mi)
{
    bool active[MAX_NOTIFICATION_SETTINGS] = {};

    for (size_t aircraft_index = 0; aircraft_index < count; ++aircraft_index) {
        if (snapshot[aircraft_index].distance_mi > (float)range_mi) {
            break;
        }
        const aircraft_ui_state_t *state = aircraft_ui_state ? &aircraft_ui_state[aircraft_index] : nullptr;
        if (!state) {
            continue;
        }
        if (!state->visible || state->notification_index < 0) {
            continue;
        }
        size_t rule_index = (size_t)state->notification_index;
        if (rule_index < MAX_NOTIFICATION_SETTINGS &&
            settings.notifications[rule_index].text[0] != '\0') {
            active[rule_index] = true;
        }
    }

    int visible_line = 0;
    for (size_t rule_index = 0; rule_index < MAX_NOTIFICATION_SETTINGS; ++rule_index) {
        lv_obj_t *label = notification_labels[rule_index];
        if (!label) {
            continue;
        }

        if (!active[rule_index]) {
            lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const notification_setting_t *notification = &settings.notifications[rule_index];
        const char *current_text = lv_label_get_text(label);
        if (!current_text || strcmp(current_text, notification->text) != 0) {
            lv_label_set_text(label, notification->text);
        }
        lv_obj_set_style_text_color(label, lv_color_hex(notification->color), 0);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 42 + (visible_line * 20));
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        ++visible_line;
    }
}

/* Refresh the on-radar GPS status label when GPS is the selected centre source. */
void RadarApp::update_gps_status_label()
{
    if (!gps_status_label) {
        return;
    }

    if (settings.center_source != RADAR_CENTER_SOURCE_GPS) {
        lv_obj_add_flag(gps_status_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    gps_snapshot_t snapshot = {};
    gps.getSnapshot(&snapshot);
    lv_label_set_text(gps_status_label, snapshot.status);
    uint32_t color = settings.colors.gps_neutral;
    if (snapshot.has_fix && !snapshot.fix_stale) {
        color = settings.colors.gps_lock;
    } else if (snapshot.device_connected) {
        color = settings.colors.gps_wait;
    }
    lv_obj_set_style_text_color(gps_status_label, lv_color_hex(color), 0);
    lv_obj_clear_flag(gps_status_label, LV_OBJ_FLAG_HIDDEN);
}

/* Apply a new text colour to all pieces of a curved button. */
void RadarApp::set_curved_button_color(curved_button_t *button, uint32_t color)
{
    CurvedButtonController::setColor(button, color);
}

/* Reapply settings-driven fonts to labels that are not recreated on save. */
void RadarApp::refresh_static_ui_fonts()
{
    for (size_t i = 0; i < HEADING_LABEL_COUNT; ++i) {
        apply_label_font(heading_labels[i], settings.label_styles.heading_label);
    }
    for (size_t i = 0; i < RANGE_RING_COUNT; ++i) {
        apply_label_font(range_ring_labels[i], settings.label_styles.range_label);
    }
    for (size_t i = 0; i < MAX_AIRPORT_LABELS; ++i) {
        apply_label_font(airport_labels[i], settings.label_styles.airport);
    }
    for (size_t i = 0; i < MAX_NOTIFICATION_SETTINGS; ++i) {
        apply_label_font(notification_labels[i], settings.label_styles.notification);
    }
    apply_label_font(gps_status_label, settings.label_styles.gps);

    curved_button_t *buttons[] = {&range_button, &wifi_button, &status_button};
    for (size_t b = 0; b < sizeof(buttons) / sizeof(buttons[0]); ++b) {
        apply_label_font(buttons[b]->label, settings.label_styles.button, -2);
        for (size_t i = 0; i < CONTROL_TEXT_MAX; ++i) {
            apply_label_font(buttons[b]->chars[i], settings.label_styles.button);
        }
        position_curved_button_text(buttons[b]);
    }

    for (size_t i = 0; i < MAX_RANGE_SETTINGS; ++i) {
        apply_label_font(range_menu_labels[i], settings.label_styles.button, -2);
    }
    lv_obj_t *wifi_menu_labels[] = {
        wifi_menu_ip_label,
        wifi_menu_change_label,
        wifi_menu_reboot_label,
        wifi_menu_clear_nvs_label,
    };
    for (size_t i = 0; i < sizeof(wifi_menu_labels) / sizeof(wifi_menu_labels[0]); ++i) {
        apply_label_font(wifi_menu_labels[i], settings.label_styles.button);
    }
    for (size_t i = 0; i < DATA_MENU_ROW_COUNT; ++i) {
        apply_label_font(data_menu_labels[i], settings.label_styles.button, -2);
    }
    hardware_controls.refreshFonts();
}

/* Reapply settings-driven colours, widths, and redraw static UI layers. */
void RadarApp::refresh_static_ui_colors()
{
    refresh_static_ui_fonts();
    if (screen_root) {
        lv_obj_set_style_bg_color(screen_root, lv_color_hex(settings.colors.screen_bg), 0);
    }
    if (radar_canvas) {
        draw_radar_canvas(radar_canvas);
    }
    for (size_t i = 0; i < HEADING_LABEL_COUNT; ++i) {
        if (heading_labels[i]) {
            lv_obj_set_style_text_color(heading_labels[i], lv_color_hex(settings.colors.heading_label), 0);
        }
    }
    for (size_t i = 0; i < RANGE_RING_COUNT; ++i) {
        if (range_ring_labels[i]) {
            lv_obj_set_style_text_color(range_ring_labels[i], lv_color_hex(settings.colors.range_label), 0);
        }
    }
    for (size_t i = 0; i < MAX_AIRPORT_LABELS; ++i) {
        if (airport_labels[i]) {
            lv_obj_set_style_text_color(airport_labels[i], lv_color_hex(settings.colors.airport), 0);
        }
    }
    for (size_t i = 0; i < MAX_AIRCRAFT_LABELS; ++i) {
        if (!markers[i].trend_icon || !markers[i].visible) {
            continue;
        }
        uint32_t color = markers[i].data.vertical_rate_fpm > 0 ?
                         settings.colors.climb_triangle : settings.colors.descent_triangle;
        lv_obj_set_style_line_color(markers[i].trend_icon, lv_color_hex(color), 0);
    }
    if (sweep_overlay) {
        lv_obj_invalidate(sweep_overlay);
    }

    set_curved_button_color(&range_button, settings.colors.button_text);
    set_curved_button_color(&wifi_button, settings.colors.button_text);
    set_curved_button_color(&status_button, settings.colors.button_status);
    curved_button_t *buttons[] = {&range_button, &wifi_button, &status_button};
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        if (buttons[i]->hitbox) {
            lv_obj_set_style_bg_color(buttons[i]->hitbox,
                                      lv_color_hex(settings.colors.button_pressed),
                                      LV_STATE_PRESSED);
        }
    }
    lv_obj_t *range_hitboxes[] = {
        range_button_left_hitbox,
        range_button_middle_hitbox,
        range_button_right_hitbox,
    };
    for (size_t i = 0; i < sizeof(range_hitboxes) / sizeof(range_hitboxes[0]); ++i) {
        if (range_hitboxes[i]) {
            lv_obj_set_style_bg_color(range_hitboxes[i],
                                      lv_color_hex(settings.colors.button_pressed),
                                      LV_STATE_PRESSED);
        }
    }
    if (wifi_menu) {
        lv_obj_set_style_bg_color(wifi_menu, lv_color_hex(settings.colors.popup_bg), 0);
        lv_obj_set_style_border_color(wifi_menu, lv_color_hex(settings.colors.popup_border), 0);
    }
    if (range_menu) {
        lv_obj_set_style_bg_color(range_menu, lv_color_hex(settings.colors.popup_bg), 0);
        lv_obj_set_style_border_color(range_menu, lv_color_hex(settings.colors.popup_border), 0);
    }
    for (size_t i = 0; i < MAX_RANGE_SETTINGS; ++i) {
        if (range_menu_rows[i]) {
            lv_obj_set_style_bg_color(range_menu_rows[i], lv_color_hex(settings.colors.popup_border), 0);
            lv_obj_set_style_bg_color(range_menu_rows[i], lv_color_hex(settings.colors.button_pressed), LV_STATE_PRESSED);
            lv_obj_set_style_border_color(range_menu_rows[i], lv_color_hex(settings.colors.popup_border), 0);
        }
        if (range_menu_labels[i]) {
            lv_obj_set_style_text_color(range_menu_labels[i], lv_color_hex(settings.colors.button_text), 0);
        }
    }
    refresh_range_menu();
    for (size_t i = 0; i < sizeof(wifi_menu_rows) / sizeof(wifi_menu_rows[0]); ++i) {
        if (wifi_menu_rows[i]) {
            lv_obj_set_style_bg_color(wifi_menu_rows[i], lv_color_hex(settings.colors.popup_border), 0);
            lv_obj_set_style_bg_color(wifi_menu_rows[i], lv_color_hex(settings.colors.button_pressed), LV_STATE_PRESSED);
            lv_obj_set_style_border_color(wifi_menu_rows[i], lv_color_hex(settings.colors.popup_border), 0);
        }
    }
    lv_obj_t *wifi_menu_labels[] = {
        wifi_menu_ip_label,
        wifi_menu_change_label,
        wifi_menu_reboot_label,
        wifi_menu_clear_nvs_label,
    };
    for (size_t i = 0; i < sizeof(wifi_menu_labels) / sizeof(wifi_menu_labels[0]); ++i) {
        if (wifi_menu_labels[i]) {
            lv_obj_set_style_text_color(wifi_menu_labels[i], lv_color_hex(settings.colors.button_text), 0);
        }
    }
    if (data_menu) {
        lv_obj_set_style_bg_color(data_menu, lv_color_hex(settings.colors.popup_bg), 0);
        lv_obj_set_style_border_color(data_menu, lv_color_hex(settings.colors.popup_border), 0);
    }
    for (size_t i = 0; i < DATA_MENU_ROW_COUNT; ++i) {
        if (data_menu_rows[i]) {
            lv_obj_set_style_bg_color(data_menu_rows[i], lv_color_hex(settings.colors.popup_border), 0);
            lv_obj_set_style_bg_color(data_menu_rows[i], lv_color_hex(settings.colors.button_pressed), LV_STATE_PRESSED);
            lv_obj_set_style_border_color(data_menu_rows[i], lv_color_hex(settings.colors.popup_border), 0);
        }
        if (data_menu_labels[i]) {
            lv_obj_set_style_text_color(data_menu_labels[i], lv_color_hex(settings.colors.button_text), 0);
        }
    }
    refresh_data_menu();
    hardware_controls.refreshColors();

    if (aircraft_popup) {
        lv_obj_set_style_bg_color(aircraft_popup, lv_color_hex(settings.colors.popup_bg), 0);
        lv_obj_set_style_border_color(aircraft_popup, lv_color_hex(settings.colors.popup_border), 0);
    }
    if (aircraft_popup_title) {
        lv_obj_set_style_text_color(aircraft_popup_title, lv_color_hex(settings.colors.button_text), 0);
    }
    if (aircraft_photo_status) {
        lv_obj_set_style_text_color(aircraft_photo_status, lv_color_hex(settings.colors.text_secondary), 0);
    }
    if (aircraft_popup_body) {
        lv_obj_set_style_text_color(aircraft_popup_body, lv_color_hex(settings.colors.text_primary), 0);
    }
    if (portal_overlay) {
        lv_obj_set_style_bg_color(portal_overlay, lv_color_hex(settings.colors.portal_bg), 0);
    }
    if (portal_ap_label) {
        lv_obj_set_style_text_color(portal_ap_label, lv_color_hex(settings.colors.button_text), 0);
    }
    if (portal_status_label) {
        lv_obj_set_style_text_color(portal_status_label, lv_color_hex(settings.colors.button_status), 0);
    }
}

/* Refresh captive portal visibility and the WiFi button status text. */
void RadarApp::update_portal_overlay(void)
{
    EventBits_t bits = wifi_event_group ? xEventGroupGetBits(wifi_event_group) : 0;
    bool portal_visible = wifi_portal_active || ((bits & WIFI_PORTAL_REQUEST_BIT) != 0);
    bool wifi_connected = (bits & WIFI_CONNECTED_BIT) != 0;

    set_curved_button_text(&wifi_button, portal_visible ? "WIFI SETUP" : (wifi_connected ? "WIFI OK" : "WIFI WAIT"));
    refresh_wifi_menu();
    if (portal_visible) {
        hide_wifi_menu();
        hide_data_menu();
    }

    if (!portal_overlay) {
        return;
    }

    if (portal_visible) {
        char status[sizeof(portal_status)];
        if (aircraft_mutex && xSemaphoreTake(aircraft_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            snprintf(status, sizeof(status), "%s", portal_status);
            xSemaphoreGive(aircraft_mutex);
        } else {
            snprintf(status, sizeof(status), "%s", portal_status);
        }

        lv_label_set_text(portal_ap_label, setup_ap_ssid);
        lv_label_set_text(portal_status_label, status);
        lv_obj_clear_flag(portal_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(portal_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Copy fetched aircraft data into the LVGL task and update dynamic UI layers. */
void RadarApp::update_aircraft_ui(lv_timer_t *timer)
{
    (void)timer;
    aircraft_data_t *snapshot = ui_aircraft_snapshot;
    size_t count = 0;
    uint32_t generation = 0;
    char status[sizeof(latest_status)] = "WAIT";
    int range_mi = get_current_range_mi();

    update_gps_status_label();
    update_portal_overlay();
    if (!snapshot) {
        return;
    }

    /*
     * Copy the shared aircraft list quickly, then release the mutex before any
     * drawing. The fetch task should never be blocked by LVGL rendering.
     */
    if (aircraft_mutex && xSemaphoreTake(aircraft_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        count = latest_aircraft_count;
        if (count > MAX_AIRCRAFT_TARGETS) {
            count = MAX_AIRCRAFT_TARGETS;
        }
        memcpy(snapshot, latest_aircraft, count * sizeof(snapshot[0]));
        generation = latest_aircraft_generation;
        snprintf(status, sizeof(status), "%s", latest_status);
        xSemaphoreGive(aircraft_mutex);
    }
    char data_text[40];
    snprintf(data_text, sizeof(data_text), "DATA %s", status);
    set_curved_button_text(&status_button, data_text);
    ui_aircraft_count = count;
    ui_aircraft_range_mi = range_mi;
    refresh_oled_dashboard();
    bool settings_changed = settings_generation != displayed_settings_generation;
    bool range_changed = range_mi != displayed_range_mi;
    if (settings_changed) {
        refresh_static_ui_colors();
    } else if (range_changed && radar_canvas) {
        draw_radar_canvas(radar_canvas);
    }
    if (range_changed || settings_changed) {
        refresh_range_label();
        update_airport_labels(range_mi);
    }
    if (generation == displayed_generation &&
        !range_changed &&
        !settings_changed) {
        return;
    }

    displayed_generation = generation;
    displayed_range_mi = range_mi;
    displayed_settings_generation = settings_generation;
    build_aircraft_ui_state(snapshot, count, range_mi);
    if (range_index < MAX_RANGE_SETTINGS && settings.ranges[range_index].show_history_trail) {
        update_aircraft_track_history(snapshot, count, range_mi, generation);
    }
    update_aircraft_plot(snapshot, count, range_mi);
    update_aircraft_labels(snapshot, count, range_mi);
    update_notification_banner(snapshot, count, range_mi);
}

/* Install cJSON allocation hooks through the shared HTTP client. */
void RadarApp::init_json_allocator(void)
{
    RadarHttpClient::initJsonAllocator();
}

/* Return whether two strings are equal after trimming whitespace, ignoring case. */
bool RadarApp::string_equals_trimmed_ci(const char *left, const char *right)
{
    if (!left || !right) {
        return false;
    }

    while (*left && isspace((unsigned char)*left)) {
        ++left;
    }
    while (*right && isspace((unsigned char)*right)) {
        ++right;
    }

    const char *left_end = left + strlen(left);
    const char *right_end = right + strlen(right);
    while (left_end > left && isspace((unsigned char)*(left_end - 1))) {
        --left_end;
    }
    while (right_end > right && isspace((unsigned char)*(right_end - 1))) {
        --right_end;
    }

    size_t left_len = (size_t)(left_end - left);
    size_t right_len = (size_t)(right_end - right);
    if (left_len == 0 || left_len != right_len) {
        return false;
    }

    for (size_t i = 0; i < left_len; ++i) {
        if (tolower((unsigned char)left[i]) != tolower((unsigned char)right[i])) {
            return false;
        }
    }
    return true;
}

/* Convert a display range in miles to the nautical-mile API radius. */
int RadarApp::miles_to_nautical_request(int range_mi)
{
    return AircraftFetchService::milesToNauticalRequest(range_mi);
}

/* Build the selected aircraft feed URL for the current centre and range. */
void RadarApp::build_aircraft_url(char *url, size_t url_size, int range_mi)
{
    fetch_service.buildAircraftUrl(url, url_size, range_mi, settings.aircraft_data_source);
}

/* Read a required numeric JSON field. */
bool RadarApp::json_get_number(cJSON *object, const char *name, double *value)
{
    return RadarHttpClient::jsonGetNumber(object, name, value);
}

/* Read a numeric JSON field that may be encoded as text. */
bool RadarApp::json_get_double(cJSON *object, const char *name, double *value)
{
    return RadarHttpClient::jsonGetDouble(object, name, value);
}

/* Copy and trim a JSON string field. */
void RadarApp::copy_trimmed_field(char *dst, size_t dst_size, cJSON *object, const char *name)
{
    RadarHttpClient::copyTrimmedField(dst, dst_size, object, name);
}

/* Normalise an ICAO airport code into a four-character uppercase value. */
bool RadarApp::normalize_icao_code(char *dst, size_t dst_size, const char *src)
{
    return RadarHttpClient::normalizeIcaoCode(dst, dst_size, src);
}

/* Publish runway cache data and invalidate the settings-driven overlay. */
void RadarApp::publish_runway_cache(const airport_runway_cache_t *cache)
{
    runway_service.publishCache(cache, &settings_generation);
}

/* Copy the active runway cache from the runway service. */
bool RadarApp::get_active_runway_cache(airport_runway_cache_t *cache)
{
    return runway_service.getActiveCache(cache);
}

/* Clear active runway data and mark the radar overlay for redraw. */
void RadarApp::clear_active_runway_cache(void)
{
    runway_service.clearActiveCache(&settings_generation);
}

/* Ensure runway data has been cached for the selected airport centre. */
void RadarApp::ensure_airport_runways_cached(void)
{
    if (!settings.show_airport_runways ||
        settings.center_source != RADAR_CENTER_SOURCE_AIRPORT ||
        settings.center_airport_code[0] == '\0') {
        return;
    }

    char icao[5] = {};
    if (!normalize_icao_code(icao, sizeof(icao), settings.center_airport_code)) {
        return;
    }

    airport_runway_cache_t active = {};
    if (!runway_service.getActiveCache(&active) || strcmp(active.icao, icao) != 0) {
        update_oled_activity("RUNWAY FETCH");
    }

    char origin[48];
    build_photo_origin(origin, sizeof(origin));
    runway_service.ensureCached(settings, http_client, origin, &settings_generation);

    if (runway_service.getActiveCache(&active) && strcmp(active.icao, icao) == 0) {
        update_oled_activity("RUNWAY %u OK", (unsigned)active.count);
    }
}

/* Parse an aircraft altitude field into feet. */
int RadarApp::parse_altitude_ft(cJSON *aircraft)
{
    return AircraftDataService::parseAltitudeFt(aircraft);
}

/* Compare two aircraft records by distance for nearest-first sorting. */
int RadarApp::aircraft_compare_distance(const void *left, const void *right)
{
    return AircraftDataService::compareDistance(left, right);
}

/* Publish parsed aircraft data into the shared UI snapshot store. */
void RadarApp::publish_aircraft_data(aircraft_data_t *items, size_t count, int total)
{
    aircraft_service.publish(items, count, total);
}

/* Parse an aircraft JSON response and publish the resulting snapshot. */
bool RadarApp::parse_aircraft_json(const char *json, int json_len, int range_mi)
{
    return aircraft_service.parseJson(json, json_len, range_mi, settings, gps);
}

/* Create the long-lived HTTP client used by the aircraft fetch worker. */
esp_http_client_handle_t RadarApp::create_aircraft_http_client(void)
{
    return active_app ? active_app->http_client.createAircraftClient() : NULL;
}

/* Fetch aircraft JSON through the aircraft fetch service. */
esp_err_t RadarApp::fetch_aircraft_json(esp_http_client_handle_t client,
                                     char **response_out, int *response_len_out,
                                     int range_mi)
{
    return fetch_service.fetchAircraftJson(client, response_out, response_len_out, range_mi,
                                           settings.aircraft_data_source);
}

/* Build the browser origin used by PlaneSpotters and generic API calls. */
void RadarApp::build_photo_origin(char *origin, size_t origin_size)
{
    if (!origin || origin_size == 0) {
        return;
    }

    if (wifi_sta_netif) {
        esp_netif_ip_info_t ip_info = {};
        if (esp_netif_get_ip_info(wifi_sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            snprintf(origin, origin_size, "http://" IPSTR, IP2STR(&ip_info.ip));
            return;
        }
    }

    snprintf(origin, origin_size, "http://" WIFI_SETUP_AP_IP);
}

/* Apply browser-like headers to an HTTP client used for aircraft photos. */
void RadarApp::set_photo_http_headers(esp_http_client_handle_t client, const char *accept)
{
    if (!client) {
        return;
    }

    char origin[48];
    char referer[56];
    build_photo_origin(origin, sizeof(origin));
    snprintf(referer, sizeof(referer), "%s/", origin);

    esp_http_client_set_header(client, "Accept",
                               accept && accept[0] != '\0' ?
                               accept : "application/json, text/javascript, */*; q=0.01");
    esp_http_client_set_header(client, "User-Agent", PHOTO_USER_AGENT);
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Accept-Language", "en-GB,en;q=0.9");
    esp_http_client_set_header(client, "Cache-Control", "no-cache");
    esp_http_client_set_header(client, "Pragma", "no-cache");
    esp_http_client_set_header(client, "Origin", origin);
    esp_http_client_set_header(client, "Referer", referer);
    esp_http_client_set_header(client, "Sec-Fetch-Dest", "empty");
    esp_http_client_set_header(client, "Sec-Fetch-Mode", "cors");
    esp_http_client_set_header(client, "Sec-Fetch-Site", "cross-site");
    esp_http_client_set_header(client, "Connection", "close");
}

/* Fetch a bounded HTTP buffer through the shared TLS-serialised HTTP client. */
esp_err_t RadarApp::fetch_http_buffer(const char *url, const char *accept,
                                   size_t initial_capacity, size_t max_bytes,
                                   char **response_out, int *response_len_out,
                                   int mutex_timeout_ms, int http_timeout_ms)
{
    char origin[48];
    build_photo_origin(origin, sizeof(origin));
    return http_client.fetchBuffer(url, accept, origin, initial_capacity, max_bytes,
                                   response_out, response_len_out,
                                   mutex_timeout_ms, http_timeout_ms);
}

/* Extract the first usable thumbnail URL from a photo metadata response. */
bool RadarApp::parse_photo_thumbnail_url(const char *json, int json_len, char *url, size_t url_size)
{
    return AircraftPhotoService::parseThumbnailUrl(json, json_len, url, url_size);
}

/* Normalise an aircraft ICAO hex string for the photo API path. */
bool RadarApp::normalize_icao_hex(char *dst, size_t dst_size, const char *src)
{
    return AircraftPhotoService::normalizeIcaoHex(dst, dst_size, src);
}

/* Check whether a photo worker result still belongs to the active popup. */
bool RadarApp::photo_request_is_current(uint32_t request_id)
{
    return photo_service.requestIsCurrent(request_id);
}

/* Update popup photo status from the photo worker task. */
void RadarApp::update_aircraft_photo_status_from_task(uint32_t request_id, const char *status)
{
    photo_service.updateStatusFromTask(request_id, status);
}

/* Install decoded popup photo pixels from the photo worker task. */
void RadarApp::install_aircraft_photo_from_task(uint32_t request_id,
                                             const lv_image_dsc_t *decoded_image,
                                             uint8_t *pixels, size_t pixels_size)
{
    photo_service.installFromTask(request_id, decoded_image, pixels, pixels_size);
}

/* Run the photo service worker for metadata fetch, image fetch, and decode. */
void RadarApp::aircraft_photo_fetch_task(void *arg)
{
    photo_service.fetchTask(arg);
}

/* Start an asynchronous aircraft photo fetch for the supplied ICAO hex. */
void RadarApp::start_aircraft_photo_fetch(const char *icao)
{
    photo_service.startFetch(icao);
}

/* Check whether a route worker result still belongs to the active popup. */
bool RadarApp::route_request_is_current(uint32_t request_id)
{
    return route_service.requestIsCurrent(request_id);
}

/* Update popup route text from the route worker task. */
void RadarApp::update_aircraft_route_from_task(uint32_t request_id, const char *route_text)
{
    route_service.updateRouteFromTask(request_id, route_text);
}

/* Run the route service worker for callsign route lookup. */
void RadarApp::aircraft_route_fetch_task(void *arg)
{
    route_service.fetchTask(arg);
}

/* Start an asynchronous aircraft route fetch for the supplied callsign. */
void RadarApp::start_aircraft_route_fetch(const char *callsign)
{
    route_service.startFetch(callsign);
}

/* Request captive portal mode from the WiFi manager. */
void RadarApp::request_wifi_portal(void)
{
    wifi_manager.requestPortal();
}

/* Consume one pending captive portal request from the WiFi manager. */
bool RadarApp::consume_wifi_portal_request(void)
{
    return wifi_manager.consumePortalRequest();
}

/* Initialise the setup access-point SSID. */
void RadarApp::init_setup_ap_ssid(void)
{
    wifi_manager.initSetupApSsid();
}

/* Load station WiFi credentials from persistent storage. */
bool RadarApp::load_wifi_credentials(void)
{
    return wifi_manager.loadCredentials();
}

/* Save station WiFi credentials through the WiFi manager. */
esp_err_t RadarApp::save_wifi_credentials(const char *ssid, const char *password)
{
    return wifi_manager.saveCredentials(ssid, password);
}

/* Validate and apply a settings candidate to the live radar. */
void RadarApp::apply_settings(const radar_settings_t *candidate)
{
    if (!candidate || !RadarSettings::isValid(*candidate)) {
        return;
    }

    /*
     * Apply settings immediately so the browser UI can save and see the radar
     * update without a reboot. Preserve the current range where possible, unless
     * the user changed the default range.
     */
    int current_range = get_current_range_mi();
    bool default_changed = candidate->default_range_mi != settings.default_range_mi;
    bool weather_changed = candidate->show_airport_weather != settings.show_airport_weather ||
                           candidate->airport_weather_refresh_min != settings.airport_weather_refresh_min ||
                           candidate->airport_weather_icon_size != settings.airport_weather_icon_size ||
                           candidate->show_airports != settings.show_airports;
    bool hardware_changed = candidate->hardware_oled_i2c_addr != settings.hardware_oled_i2c_addr ||
                            candidate->hardware_confirm_gpio != settings.hardware_confirm_gpio ||
                            candidate->hardware_back_gpio != settings.hardware_back_gpio ||
                            candidate->hardware_rotary_a_gpio != settings.hardware_rotary_a_gpio ||
                            candidate->hardware_rotary_b_gpio != settings.hardware_rotary_b_gpio ||
                            candidate->hardware_rotary_push_gpio != settings.hardware_rotary_push_gpio;
    settings.apply(*candidate);

    size_t count = get_range_count();
    if (default_changed) {
        set_range_to_default();
    } else {
        range_index = 0;
        for (size_t i = 0; i < count; ++i) {
            if (settings.ranges[i].miles == current_range) {
                range_index = i;
                break;
            }
        }
    }

    settings_generation++;
    if (weather_changed) {
        airport_weather_last_fetch_ms = 0;
    }
    if (hardware_changed) {
        oled_status.init((uint8_t)settings.hardware_oled_i2c_addr);
        hardware_controls.initInputs();
    }
    if (wifi_event_group) {
        xEventGroupSetBits(wifi_event_group, FETCH_NOW_BIT);
    }
}

/* Read a bounded HTTP request body into a null-terminated heap buffer. */
char *RadarApp::read_request_body(httpd_req_t *req, size_t max_len)
{
    if (!req || req->content_len <= 0 || (size_t)req->content_len > max_len) {
        return nullptr;
    }

    char *body = (char *)heap_caps_calloc((size_t)req->content_len + 1, 1,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = (char *)heap_caps_calloc((size_t)req->content_len + 1, 1, MALLOC_CAP_8BIT);
    }
    if (!body) {
        return nullptr;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(body);
            return nullptr;
        }
        received += ret;
    }
    body[received] = '\0';
    return body;
}

/* Send a compact JSON status response through the settings server. */
esp_err_t RadarApp::send_json_status(httpd_req_t *req, bool ok, const char *message)
{
    return settings_server.sendJsonStatus(req, ok, message);
}

/* Send the current settings JSON document through the settings server. */
esp_err_t RadarApp::send_settings_json(httpd_req_t *req)
{
    return settings_server.sendSettingsJson(req);
}

/* Send factory-default settings JSON through the settings server. */
esp_err_t RadarApp::send_defaults_json(httpd_req_t *req)
{
    return settings_server.sendDefaultsJson(req);
}

/* Serve the browser settings application. */
esp_err_t RadarApp::settings_page_handler(httpd_req_t *req)
{
    return settings_server.pageHandler(req);
}

/* Serve the current settings API endpoint. */
esp_err_t RadarApp::settings_api_get_handler(httpd_req_t *req)
{
    return settings_server.apiGetHandler(req);
}

/* Handle a settings API save request. */
esp_err_t RadarApp::settings_api_save_handler(httpd_req_t *req)
{
    return settings_server.apiSaveHandler(req);
}

/* Serve the live ESP32 status endpoint. */
esp_err_t RadarApp::settings_status_handler(httpd_req_t *req)
{
    return settings_server.statusHandler(req);
}

/* Serve the current LVGL display as a BMP screenshot. */
esp_err_t RadarApp::settings_screenshot_handler(httpd_req_t *req)
{
    return settings_server.screenshotHandler(req);
}

/* Serve the factory-default settings endpoint. */
esp_err_t RadarApp::settings_defaults_handler(httpd_req_t *req)
{
    return settings_server.defaultsHandler(req);
}

/* Reset configured range presets through the settings server. */
esp_err_t RadarApp::settings_ranges_reset_handler(httpd_req_t *req)
{
    return settings_server.rangesResetHandler(req);
}

/* Serve airport autocomplete requests through the settings server. */
esp_err_t RadarApp::settings_airport_search_handler(httpd_req_t *req)
{
    return settings_server.airportSearchHandler(req);
}

/* Serve OpenWeather location lookup requests through the settings server. */
esp_err_t RadarApp::settings_location_search_handler(httpd_req_t *req)
{
    return settings_server.locationSearchHandler(req);
}

/* Serve WiFi scan requests through the settings server. */
esp_err_t RadarApp::settings_wifi_scan_handler(httpd_req_t *req)
{
    return settings_server.wifiScanHandler(req);
}

/* Save WiFi credentials submitted by the settings server. */
esp_err_t RadarApp::settings_wifi_save_handler(httpd_req_t *req)
{
    return settings_server.wifiSaveHandler(req);
}

/* Start the station-mode settings HTTP server. */
bool RadarApp::start_settings_http_server(void)
{
    return settings_server.start();
}

/* Stop the station-mode settings HTTP server. */
void RadarApp::stop_settings_http_server(void)
{
    settings_server.stop();
}

/* Convert one hexadecimal character to its numeric value. */
int RadarApp::hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

/* Decode a URL-encoded string into a fixed-size buffer. */
void RadarApp::url_decode(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    size_t out = 0;
    for (size_t i = 0; i < src_len && out + 1 < dst_size; ++i) {
        if (src[i] == '+') {
            dst[out++] = ' ';
        } else if (src[i] == '%' && i + 2 < src_len) {
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                dst[out++] = src[i];
            }
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

/* Extract and decode one value from an application/x-www-form-urlencoded body. */
bool RadarApp::form_get_value(const char *body, const char *key, char *out, size_t out_size)
{
    out[0] = '\0';
    size_t key_len = strlen(key);
    const char *pos = body;

    while (*pos) {
        const char *next = strchr(pos, '&');
        size_t pair_len = next ? (size_t)(next - pos) : strlen(pos);
        const char *equals = (const char *)memchr(pos, '=', pair_len);
        if (equals && (size_t)(equals - pos) == key_len && strncmp(pos, key, key_len) == 0) {
            const char *value = equals + 1;
            size_t value_len = pair_len - key_len - 1;
            url_decode(out, out_size, value, value_len);
            return true;
        }
        pos = next ? next + 1 : pos + pair_len;
    }

    return false;
}

/* Stream text to an HTTP response with HTML-sensitive characters escaped. */
void RadarApp::http_send_escaped(httpd_req_t *req, const char *text)
{
    const char *start = text;
    for (const char *p = text; *p; ++p) {
        const char *escaped = NULL;
        switch (*p) {
        case '&':
            escaped = "&amp;";
            break;
        case '<':
            escaped = "&lt;";
            break;
        case '>':
            escaped = "&gt;";
            break;
        case '"':
            escaped = "&quot;";
            break;
        case '\'':
            escaped = "&#39;";
            break;
        default:
            break;
        }

        if (escaped) {
            if (p > start) {
                httpd_resp_send_chunk(req, start, p - start);
            }
            httpd_resp_sendstr_chunk(req, escaped);
            start = p + 1;
        }
    }

    if (*start) {
        httpd_resp_sendstr_chunk(req, start);
    }
}

/* Scan nearby WiFi access points through the WiFi manager. */
uint16_t RadarApp::scan_wifi_networks(wifi_ap_record_t *aps, uint16_t max_count)
{
    return wifi_manager.scanNetworks(aps, max_count);
}

/* Serve the captive portal WiFi setup page. */
esp_err_t RadarApp::portal_get_handler(httpd_req_t *req)
{
    return captive_portal.getHandler(req);
}

/* Handle captive portal WiFi credential submission. */
esp_err_t RadarApp::portal_save_handler(httpd_req_t *req)
{
    return captive_portal.saveHandler(req);
}

/* Start the captive portal HTTP server. */
bool RadarApp::start_portal_http_server(void)
{
    return captive_portal.startHttpServer();
}

/* Run the captive portal DNS responder task. */
void RadarApp::dns_server_task(void *arg)
{
    captive_portal.dnsServerTask(arg);
}

/* Start the captive portal DNS responder. */
bool RadarApp::start_portal_dns_server(void)
{
    return captive_portal.startDnsServer();
}

/* Stop captive portal HTTP and DNS services. */
void RadarApp::stop_portal_services(void)
{
    captive_portal.stopServices();
}

/* Start setup AP mode and captive portal services. */
bool RadarApp::start_wifi_portal(void)
{
    return wifi_manager.startPortal();
}

/* Stop setup AP mode and return to normal WiFi operation. */
void RadarApp::stop_wifi_portal(void)
{
    wifi_manager.stopPortal();
}

/* Start station mode using saved WiFi credentials. */
bool RadarApp::start_wifi_station(void)
{
    return wifi_manager.startStation();
}

/* Attempt WiFi recovery after an HTTP/TLS failure. */
bool RadarApp::recover_wifi_after_http_failure(void)
{
    return wifi_manager.recoverAfterHttpFailure();
}

/* Dispatch WiFi and IP events into the WiFi manager. */
void RadarApp::wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    wifi_manager.handleEvent(arg, event_base, event_id, event_data);
}

/* Initialise WiFi interfaces, event handling, and manager state. */
bool RadarApp::wifi_manager_init(void)
{
    return wifi_manager.init();
}

/* Run the aircraft fetch worker task. */
void RadarApp::aircraft_fetch_task(void *arg)
{
    fetch_service.task(arg);
}

/* Forward the airport weather task to the active application instance. */
void RadarApp::airport_weather_task_entry(void *arg)
{
    RadarApp *app = static_cast<RadarApp *>(arg);
    if (app) {
        app->airport_weather_task(arg);
    }
}

/* Fetch and cache weather for the airports currently visible on the radar. */
void RadarApp::refresh_airport_weather()
{
    if (!settings.show_airports || !settings.show_airport_weather) {
        if (airport_weather_mutex && xSemaphoreTake(airport_weather_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            airport_weather_count = 0;
            xSemaphoreGive(airport_weather_mutex);
        }
        update_oled_activity("WEATHER OFF");
        return;
    }

    EventBits_t bits = wifi_event_group ? xEventGroupGetBits(wifi_event_group) : 0;
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        update_oled_activity("WEATHER NO WIFI");
        return;
    }

    update_oled_activity("WEATHER CHECK");

    typedef struct {
        size_t index;
        double lat;
        double lon;
    } visible_airport_t;

    visible_airport_t visible[MAX_AIRPORT_WEATHER] = {};
    size_t visible_count = 0;
    int range_mi = get_current_range_mi();
    double center_lat = settings.center_lat;
    double center_lon = settings.center_lon;
    get_radar_center(&center_lat, &center_lon);
    double lat_span = (double)range_mi / 69.0;
    double cos_lat = fabs(cos(deg_to_rad(center_lat)));
    double lon_span = (double)range_mi / (69.0 * fmax(cos_lat, 0.15));

    for (size_t i = 0; i < airport_record_count && visible_count < MAX_AIRPORT_WEATHER; ++i) {
        double lat = (double)airport_records[i].lat_e6 / 1000000.0;
        double lon = (double)airport_records[i].lon_e6 / 1000000.0;
        if (lat < center_lat - lat_span || lat > center_lat + lat_span ||
            lon < center_lon - lon_span || lon > center_lon + lon_span) {
            continue;
        }

        double distance = distance_miles(center_lat, center_lon, lat, lon);
        if (distance < 0.0 || distance > (double)range_mi) {
            continue;
        }

        visible[visible_count++] = {i, lat, lon};
    }

    airport_weather_t fetched[MAX_AIRPORT_WEATHER] = {};
    size_t fetched_count = 0;
    if (visible_count == 0) {
        if (airport_weather_mutex && xSemaphoreTake(airport_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            airport_weather_count = 0;
            airport_weather_last_fetch_ms = esp_timer_get_time() / 1000LL;
            xSemaphoreGive(airport_weather_mutex);
        }
        update_oled_activity("WEATHER NONE");
        return;
    }

    char url[3072];
    int used = snprintf(url, sizeof(url), "https://api.open-meteo.com/v1/forecast?latitude=");
    for (size_t i = 0; i < visible_count && used > 0 && used < (int)sizeof(url); ++i) {
        used += snprintf(url + used, sizeof(url) - (size_t)used,
                         "%s%.5f", i == 0 ? "" : ",", visible[i].lat);
    }
    if (used > 0 && used < (int)sizeof(url)) {
        used += snprintf(url + used, sizeof(url) - (size_t)used, "&longitude=");
    }
    for (size_t i = 0; i < visible_count && used > 0 && used < (int)sizeof(url); ++i) {
        used += snprintf(url + used, sizeof(url) - (size_t)used,
                         "%s%.5f", i == 0 ? "" : ",", visible[i].lon);
    }
    if (used <= 0 || used >= (int)sizeof(url)) {
        ESP_LOGW(TAG, "Airport weather request too large for %u airports", (unsigned)visible_count);
        update_oled_activity("WEATHER TOO BIG");
        return;
    }
    snprintf(url + used, sizeof(url) - (size_t)used,
             "&current=weather_code,cloud_cover,precipitation,rain");

    char *response = nullptr;
    int response_len = 0;
    update_oled_activity("WEATHER FETCH");
    esp_err_t err = http_client.fetchBuffer(url, "application/json", nullptr,
                                            4096, 64 * 1024, &response, &response_len,
                                            0, PHOTO_OPTIONAL_HTTP_TIMEOUT_MS);
    if (err != ESP_OK || !response) {
        update_oled_activity("WEATHER ERR");
        return;
    }

    cJSON *root = cJSON_ParseWithLength(response, response_len);
    heap_caps_free(response);
    if (!root) {
        update_oled_activity("WEATHER JSON ERR");
        return;
    }

    auto parse_weather_item = [&](cJSON *item, size_t visible_index) {
        if (!cJSON_IsObject(item) || visible_index >= visible_count ||
            fetched_count >= MAX_AIRPORT_WEATHER) {
            return;
        }

        cJSON *current = cJSON_GetObjectItemCaseSensitive(item, "current");
        cJSON *weather_code = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
        cJSON *cloud_cover = cJSON_GetObjectItemCaseSensitive(current, "cloud_cover");
        cJSON *precipitation = cJSON_GetObjectItemCaseSensitive(current, "precipitation");
        cJSON *rain = cJSON_GetObjectItemCaseSensitive(current, "rain");
        if (!cJSON_IsNumber(weather_code)) {
            return;
        }

        fetched[fetched_count].airport_index = visible[visible_index].index;
        fetched[fetched_count].weather_code = weather_code->valueint;
        fetched[fetched_count].cloud_cover = cJSON_IsNumber(cloud_cover) ? cloud_cover->valueint : -1;
        fetched[fetched_count].precipitation_mm =
            cJSON_IsNumber(precipitation) ? (float)precipitation->valuedouble : 0.0f;
        fetched[fetched_count].rain_mm = cJSON_IsNumber(rain) ? (float)rain->valuedouble : 0.0f;
        fetched[fetched_count].fetched_ms = esp_timer_get_time() / 1000LL;
        fetched[fetched_count].valid = true;
        ++fetched_count;
    };

    if (cJSON_IsArray(root)) {
        size_t count = cJSON_GetArraySize(root);
        if (count > visible_count) {
            count = visible_count;
        }
        for (size_t i = 0; i < count; ++i) {
            parse_weather_item(cJSON_GetArrayItem(root, (int)i), i);
        }
    } else {
        parse_weather_item(root, 0);
    }
    cJSON_Delete(root);

    if (airport_weather_mutex && xSemaphoreTake(airport_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(airport_weather, fetched, sizeof(fetched));
        airport_weather_count = fetched_count;
        airport_weather_last_fetch_ms = esp_timer_get_time() / 1000LL;
        xSemaphoreGive(airport_weather_mutex);
    }

    if (fetched_count > 0) {
        ++settings_generation;
        ESP_LOGI(TAG, "Updated airport weather for %u airports", (unsigned)fetched_count);
        update_oled_activity("WEATHER %u OK", (unsigned)fetched_count);
    } else {
        update_oled_activity("WEATHER NONE");
    }
}

/* Periodically refresh visible airport weather without blocking the UI task. */
void RadarApp::airport_weather_task(void *arg)
{
    (void)arg;
    while (true) {
        int interval_min = settings.airport_weather_refresh_min;
        if (interval_min < RADAR_SETTINGS_MIN_AIRPORT_WEATHER_REFRESH_MIN) {
            interval_min = RADAR_SETTINGS_DEFAULT_AIRPORT_WEATHER_REFRESH_MIN;
        }
        int64_t now_ms = esp_timer_get_time() / 1000LL;
        int64_t interval_ms = (int64_t)interval_min * 60LL * 1000LL;
        if (settings.show_airport_weather &&
            (airport_weather_last_fetch_ms == 0 ||
             now_ms - airport_weather_last_fetch_ms >= interval_ms)) {
            refresh_airport_weather();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* Initialise NVS, erasing once if the partition is incompatible. */
void RadarApp::init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

/* Create a styled wrapping label for a full-screen overlay. */
lv_obj_t *RadarApp::make_overlay_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                                    uint32_t color, int y, int width)
{
    lv_obj_t *label = make_label(parent, text, font, color);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

/* Handle the aircraft popup close button. */
void RadarApp::aircraft_popup_close_event(lv_event_t *event)
{
    popup_controller.closeEvent(event);
}

/* Create the aircraft details popup. */
void RadarApp::create_aircraft_popup(lv_obj_t *screen)
{
    popup_controller.create(screen);
}

/* Create the on-device instructions shown while the captive portal is active. */
void RadarApp::create_portal_overlay(lv_obj_t *screen)
{
    portal_overlay = lv_obj_create(screen);
    lv_obj_set_size(portal_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(portal_overlay, 0, 0);
    lv_obj_set_style_bg_color(portal_overlay, lv_color_hex(settings.colors.portal_bg), 0);
    lv_obj_set_style_bg_opa(portal_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(portal_overlay, 0, 0);
    lv_obj_set_style_pad_all(portal_overlay, 0, 0);
    lv_obj_clear_flag(portal_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(portal_overlay, LV_OBJ_FLAG_HIDDEN);

    make_overlay_label(portal_overlay, "WIFI SETUP", &lv_font_montserrat_26,
                       settings.colors.gps_lock, 120, 520);
    make_overlay_label(portal_overlay, "On your phone or computer, connect to this network:",
                       &lv_font_montserrat_16, settings.colors.sweep, 184, 540);
    portal_ap_label = make_overlay_label(portal_overlay, setup_ap_ssid, &lv_font_montserrat_22,
                                         settings.colors.button_text, 238, 520);
    make_overlay_label(portal_overlay, "Then open http://192.168.4.1 and save your WiFi details.",
                       &lv_font_montserrat_16, settings.colors.text_primary, 302, 560);
    portal_status_label = make_overlay_label(portal_overlay, "Starting setup network",
                                             &lv_font_montserrat_16, settings.colors.button_status, 386, 540);
}

/* Create one row in the WiFi menu with a label and click callback. */
lv_obj_t *RadarApp::create_wifi_menu_row(lv_obj_t *parent, int y, const char *text,
                                         lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 248, 38);
    lv_obj_set_pos(row, 12, y);
    lv_obj_set_style_bg_color(row, lv_color_hex(settings.colors.popup_border), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_10, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(settings.colors.button_pressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(row, lv_color_hex(settings.colors.popup_border), 0);
    lv_obj_set_style_border_opa(row, LV_OPA_40, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = make_label(row, text,
                                 configured_label_font(settings.label_styles.button),
                                 settings.colors.button_text);
    lv_obj_set_width(label, 226);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    if (label_out) {
        *label_out = label;
    }
    return row;
}

/* Create the WiFi menu containing IP, WiFi setup, reboot, and NVS clear actions. */
void RadarApp::create_wifi_menu(lv_obj_t *screen)
{
    wifi_menu = lv_obj_create(screen);
    lv_obj_set_size(wifi_menu, 272, 204);
    lv_obj_set_pos(wifi_menu, (SCREEN_W - 272) / 2, 448);
    lv_obj_set_style_bg_color(wifi_menu, lv_color_hex(settings.colors.popup_bg), 0);
    lv_obj_set_style_bg_opa(wifi_menu, LV_OPA_90, 0);
    lv_obj_set_style_border_color(wifi_menu, lv_color_hex(settings.colors.popup_border), 0);
    lv_obj_set_style_border_opa(wifi_menu, LV_OPA_70, 0);
    lv_obj_set_style_border_width(wifi_menu, 1, 0);
    lv_obj_set_style_radius(wifi_menu, 8, 0);
    lv_obj_set_style_pad_all(wifi_menu, 0, 0);
    lv_obj_clear_flag(wifi_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wifi_menu, LV_OBJ_FLAG_HIDDEN);

    wifi_menu_rows[0] = create_wifi_menu_row(wifi_menu, 10, "IP --",
                                             RadarApp::wifi_menu_ip_event_entry,
                                             &wifi_menu_ip_label);
    wifi_menu_rows[1] = create_wifi_menu_row(wifi_menu, 56, "CHANGE WIFI",
                                             RadarApp::wifi_menu_setup_event_entry,
                                             &wifi_menu_change_label);
    wifi_menu_rows[2] = create_wifi_menu_row(wifi_menu, 102, "REBOOT ESP32",
                                             RadarApp::wifi_menu_reboot_event_entry,
                                             &wifi_menu_reboot_label);
    wifi_menu_rows[3] = create_wifi_menu_row(wifi_menu, 148, "CLEAR NVS",
                                             RadarApp::wifi_menu_clear_nvs_event_entry,
                                             &wifi_menu_clear_nvs_label);
}

/* Create one row in the DATA menu and attach its action identifier. */
lv_obj_t *RadarApp::create_data_menu_row(lv_obj_t *parent, int y, const char *text,
                                         intptr_t action, lv_obj_t **label_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 320, 36);
    lv_obj_set_pos(row, 12, y);
    lv_obj_set_style_bg_color(row, lv_color_hex(settings.colors.popup_border), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_10, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(settings.colors.button_pressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(row, lv_color_hex(settings.colors.popup_border), 0);
    lv_obj_set_style_border_opa(row, LV_OPA_40, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, RadarApp::data_menu_event_entry,
                        LV_EVENT_CLICKED, (void *)action);

    lv_obj_t *label = make_label(row, text,
                                 configured_label_font(settings.label_styles.button, -2),
                                 settings.colors.button_text);
    lv_obj_set_width(label, 296);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    if (label_out) {
        *label_out = label;
    }
    return row;
}

/* Create the DATA menu containing filters and display toggles. */
void RadarApp::create_data_menu(lv_obj_t *screen)
{
    data_menu = lv_obj_create(screen);
    lv_obj_set_size(data_menu, 344, 374);
    lv_obj_set_pos(data_menu, (SCREEN_W - 344) / 2, 160);
    lv_obj_set_style_bg_color(data_menu, lv_color_hex(settings.colors.popup_bg), 0);
    lv_obj_set_style_bg_opa(data_menu, LV_OPA_90, 0);
    lv_obj_set_style_border_color(data_menu, lv_color_hex(settings.colors.popup_border), 0);
    lv_obj_set_style_border_opa(data_menu, LV_OPA_70, 0);
    lv_obj_set_style_border_width(data_menu, 1, 0);
    lv_obj_set_style_radius(data_menu, 8, 0);
    lv_obj_set_style_pad_all(data_menu, 0, 0);
    lv_obj_clear_flag(data_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(data_menu, LV_OBJ_FLAG_HIDDEN);

    data_menu_rows[DATA_MENU_FILTER_ALL] =
        create_data_menu_row(data_menu, 12, "ALL AIRCRAFT",
                             DATA_MENU_FILTER_ALL, &data_menu_labels[DATA_MENU_FILTER_ALL]);
    data_menu_rows[DATA_MENU_FILTER_MILITARY] =
        create_data_menu_row(data_menu, 58, "MILITARY AIRCRAFT",
                             DATA_MENU_FILTER_MILITARY, &data_menu_labels[DATA_MENU_FILTER_MILITARY]);
    data_menu_rows[DATA_MENU_FILTER_INTERESTING] =
        create_data_menu_row(data_menu, 104, "INTERESTING AIRCRAFT",
                             DATA_MENU_FILTER_INTERESTING, &data_menu_labels[DATA_MENU_FILTER_INTERESTING]);
    data_menu_rows[DATA_MENU_TOGGLE_HEADING] =
        create_data_menu_row(data_menu, 158, "HEADING",
                             DATA_MENU_TOGGLE_HEADING, &data_menu_labels[DATA_MENU_TOGGLE_HEADING]);
    data_menu_rows[DATA_MENU_TOGGLE_AIRPORTS] =
        create_data_menu_row(data_menu, 204, "AIRPORTS",
                             DATA_MENU_TOGGLE_AIRPORTS, &data_menu_labels[DATA_MENU_TOGGLE_AIRPORTS]);
    data_menu_rows[DATA_MENU_TOGGLE_COUNTRIES] =
        create_data_menu_row(data_menu, 250, "COUNTRIES",
                             DATA_MENU_TOGGLE_COUNTRIES, &data_menu_labels[DATA_MENU_TOGGLE_COUNTRIES]);
    data_menu_rows[DATA_MENU_TOGGLE_RUNWAYS] =
        create_data_menu_row(data_menu, 296, "RUNWAYS",
                             DATA_MENU_TOGGLE_RUNWAYS, &data_menu_labels[DATA_MENU_TOGGLE_RUNWAYS]);
    data_menu_rows[DATA_MENU_TOGGLE_GROUND_AIRCRAFT] =
        create_data_menu_row(data_menu, 338, "GROUND A/C",
                             DATA_MENU_TOGGLE_GROUND_AIRCRAFT,
                             &data_menu_labels[DATA_MENU_TOGGLE_GROUND_AIRCRAFT]);
    refresh_data_menu();
}

/* Build the full radar LVGL scene, controls, popups, overlays, and timers. */
void RadarApp::create_radar_ui(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    screen_root = screen;
    lv_obj_set_size(screen, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(screen, lv_color_hex(settings.colors.screen_bg), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(screen);

    lv_obj_t *radar = lv_obj_create(screen);
    lv_obj_set_size(radar, RADAR_SIZE, RADAR_SIZE);
    lv_obj_set_pos(radar, (SCREEN_W - RADAR_SIZE) / 2, RADAR_Y);
    lv_obj_set_style_bg_opa(radar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(radar, 0, 0);
    lv_obj_set_style_pad_all(radar, 0, 0);
    lv_obj_clear_flag(radar, LV_OBJ_FLAG_SCROLLABLE);

    radar_canvas = create_radar_canvas(radar);
    if (!radar_canvas) {
        return;
    }

    create_heading_labels(radar);
    create_range_ring_labels(radar);

    aircraft_canvas = create_aircraft_canvas(radar);
    create_airport_labels(radar);

    for (size_t i = 0; i < MAX_NOTIFICATION_SETTINGS; ++i) {
        notification_labels[i] = make_label(radar, "",
                                            configured_label_font(settings.label_styles.notification),
                                            settings.notifications[i].color);
        lv_obj_set_width(notification_labels[i], 520);
        lv_label_set_long_mode(notification_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(notification_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(notification_labels[i], LV_ALIGN_TOP_MID, 0, 42 + ((int)i * 20));
        lv_obj_add_flag(notification_labels[i], LV_OBJ_FLAG_HIDDEN);
    }

    gps_status_label = make_label(radar, "",
                                  configured_label_font(settings.label_styles.gps),
                                  settings.colors.gps_neutral);
    lv_obj_set_width(gps_status_label, 150);
    lv_label_set_long_mode(gps_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(gps_status_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(gps_status_label, 92, 42);
    lv_obj_add_flag(gps_status_label, LV_OBJ_FLAG_HIDDEN);

    sweep_overlay = lv_obj_create(radar);
    lv_obj_remove_style_all(sweep_overlay);
    lv_obj_set_size(sweep_overlay, RADAR_SIZE, RADAR_SIZE);
    lv_obj_set_pos(sweep_overlay, 0, 0);
    lv_obj_set_style_bg_opa(sweep_overlay, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(sweep_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(sweep_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sweep_overlay, RadarApp::sweep_draw_event_entry,
                        LV_EVENT_DRAW_MAIN, NULL);

    create_aircraft_markers(radar);
    create_radar_touch_layer(radar);

    int range_x = 42;
    int range_y = 592;
    make_curved_button(screen, &range_button, 212, 248,
                       range_x, range_y, CONTROL_BUTTON_W, CONTROL_BUTTON_H,
                       "RNG --", NULL, settings.colors.button_text);
    const int range_side_hitbox_size = 76;
    const int range_middle_hitbox_size = 86;
    auto position_range_arc_hitbox = [](lv_obj_t *obj, int degrees, int size) {
        float radians = ((float)degrees - 90.0f) * PI_F / 180.0f;
        int center_x = (SCREEN_W / 2) + (int)lroundf(cosf(radians) * (float)CONTROL_ARC_RADIUS);
        int center_y = (SCREEN_H / 2) + (int)lroundf(sinf(radians) * (float)CONTROL_ARC_RADIUS);
        lv_obj_set_size(obj, size, size);
        lv_obj_set_pos(obj, center_x - (size / 2), center_y - (size / 2));
    };
    range_button_left_hitbox = lv_obj_create(screen);
    position_range_arc_hitbox(range_button_left_hitbox, 254, range_side_hitbox_size);
    lv_obj_set_style_bg_opa(range_button_left_hitbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(range_button_left_hitbox, lv_color_hex(settings.colors.button_pressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(range_button_left_hitbox, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_radius(range_button_left_hitbox, range_side_hitbox_size / 2, 0);
    lv_obj_set_style_border_width(range_button_left_hitbox, 0, 0);
    lv_obj_set_style_pad_all(range_button_left_hitbox, 0, 0);
    lv_obj_clear_flag(range_button_left_hitbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(range_button_left_hitbox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(range_button_left_hitbox, RadarApp::range_button_event_entry,
                        LV_EVENT_CLICKED, (void *)(intptr_t)-1);

    range_button_middle_hitbox = lv_obj_create(screen);
    position_range_arc_hitbox(range_button_middle_hitbox, 230, range_middle_hitbox_size);
    lv_obj_set_style_bg_opa(range_button_middle_hitbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(range_button_middle_hitbox, lv_color_hex(settings.colors.button_pressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(range_button_middle_hitbox, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_radius(range_button_middle_hitbox, range_middle_hitbox_size / 2, 0);
    lv_obj_set_style_border_width(range_button_middle_hitbox, 0, 0);
    lv_obj_set_style_pad_all(range_button_middle_hitbox, 0, 0);
    lv_obj_clear_flag(range_button_middle_hitbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(range_button_middle_hitbox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(range_button_middle_hitbox, RadarApp::range_button_event_entry,
                        LV_EVENT_CLICKED, (void *)(intptr_t)0);

    range_button_right_hitbox = lv_obj_create(screen);
    position_range_arc_hitbox(range_button_right_hitbox, 206, range_side_hitbox_size);
    lv_obj_set_style_bg_opa(range_button_right_hitbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(range_button_right_hitbox, lv_color_hex(settings.colors.button_pressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(range_button_right_hitbox, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_radius(range_button_right_hitbox, range_side_hitbox_size / 2, 0);
    lv_obj_set_style_border_width(range_button_right_hitbox, 0, 0);
    lv_obj_set_style_pad_all(range_button_right_hitbox, 0, 0);
    lv_obj_clear_flag(range_button_right_hitbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(range_button_right_hitbox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(range_button_right_hitbox, RadarApp::range_button_event_entry,
                        LV_EVENT_CLICKED, (void *)(intptr_t)1);
    make_curved_button(screen, &wifi_button, 162, 198,
                       (SCREEN_W - CONTROL_BUTTON_W) / 2, 650,
                       CONTROL_BUTTON_W, CONTROL_BUTTON_H,
                       "WIFI WAIT", RadarApp::wifi_button_event_entry, settings.colors.button_text);
    make_curved_button(screen, &status_button, 112, 148,
                       SCREEN_W - 42 - CONTROL_BUTTON_W, 592,
                       CONTROL_BUTTON_W, CONTROL_BUTTON_H,
                       "DATA START", RadarApp::status_button_event_entry, settings.colors.button_status);

    create_aircraft_popup(screen);
    create_range_menu(screen);
    create_wifi_menu(screen);
    create_data_menu(screen);
    hardware_controls.createMenu(screen);
    create_portal_overlay(screen);

    refresh_range_label();
    bsp_display_brightness_set(DEFAULT_BRIGHTNESS);

    lv_timer_create(RadarApp::sweep_timer_entry, SWEEP_TIMER_MS, NULL);
    lv_timer_create(HardwareControlService::timerEntry, ROTARY_POLL_MS, &hardware_controls);
    lv_timer_create(RadarApp::update_aircraft_ui_entry, 1000, NULL);
    sweep_timer_cb(NULL);
}

/* Start persistent services, create the display, and launch worker tasks. */
void RadarApp::run()
{
    /*
     * Start persistent services before the display is built, then hand off to
     * LVGL timers and FreeRTOS worker tasks. app_main keeps the RadarApp static
     * so all callback forwarding through active_app remains valid.
     */
    active_app = this;
    settings_server.bind(this);
    captive_portal.bind(this);
    wifi_manager.bind(this);
    fetch_service.bind(this);
    photo_service.bind(this);
    route_service.bind(this);
    popup_controller.bind(this);
    curved_button_controller.bind(this);
    hardware_controls.bind(this);
    ESP_LOGI(TAG, "Starting Radar Console");

    aircraft_mutex = xSemaphoreCreateMutex();
    wifi_mutex = xSemaphoreCreateMutex();
    airport_weather_mutex = xSemaphoreCreateMutex();
    http_client.init();
    runway_service.init();
    wifi_event_group = xEventGroupCreate();
    init_nvs();
    init_json_allocator();
    settings.load();
    set_range_to_default();
    init_aircraft_storage();

    bsp_display_cfg_t cfg = {};
    cfg.lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    cfg.rotation = ESP_LV_ADAPTER_ROTATE_0;
    cfg.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;
    cfg.touch_flags.swap_xy = 0;
    cfg.touch_flags.mirror_x = 0;
    cfg.touch_flags.mirror_y = 0;

    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();
    oled_status.init((uint8_t)settings.hardware_oled_i2c_addr);
    hardware_controls.initInputs();

    bsp_display_lock((uint32_t)-1);
    create_radar_ui();
    bsp_display_unlock();

    gps.start();
    xTaskCreatePinnedToCoreWithCaps(RadarApp::aircraft_fetch_task_entry, "aircraft_fetch",
                                    FETCH_TASK_STACK, this, FETCH_TASK_PRIORITY, NULL,
                                    FETCH_TASK_CORE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    xTaskCreatePinnedToCoreWithCaps(RadarApp::airport_weather_task_entry, "airport_weather",
                                    AIRPORT_WEATHER_TASK_STACK, this,
                                    AIRPORT_WEATHER_TASK_PRIORITY, NULL,
                                    AIRPORT_WEATHER_TASK_CORE,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* ESP-IDF entry point; keep the application static so callbacks can reference it. */
extern "C" void app_main(void)
{
    static RadarApp app;
    app.run();
}
