#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "zlib.h"

#include "bsp/esp-bsp.h"

/*
 * Shared display, networking, and storage limits for the radar application.
 *
 * These values are deliberately centralised because the ESP32-P4 build is
 * sensitive to RAM pressure. Increasing a canvas size, HTTP buffer, label
 * count, or task stack here can change whether WiFi, TLS, USB GPS, and LVGL
 * can all run together. Prefer changing these constants with a monitor session
 * open so heap and watchdog behaviour can be checked on real hardware.
 */
extern int radar_screen_w;
extern int radar_screen_h;
extern int radar_size;
extern int radar_center;
extern int radar_radius;
extern int radar_bg_radius;
extern int radar_y;
extern int control_arc_radius;
extern int heading_label_radius;

#define SCREEN_W radar_screen_w
#define SCREEN_H radar_screen_h
#define RADAR_SIZE radar_size
#define RADAR_CENTER radar_center
#define RADAR_RADIUS radar_radius
#define RADAR_BG_RADIUS radar_bg_radius
#define RADAR_Y radar_y
#define CONTROL_ARC_RADIUS control_arc_radius
#define CONTROL_ARC_WIDTH 20
#define CONTROL_BUTTON_W 184
#define CONTROL_BUTTON_H 56
#define CONTROL_TEXT_MAX 18
#define AIRCRAFT_POPUP_W 320
#define AIRCRAFT_POPUP_H 380
#define AIRCRAFT_PHOTO_W 200
#define AIRCRAFT_PHOTO_H 112
#define DEFAULT_BRIGHTNESS 95
#define MAX_AIRCRAFT_TARGETS 512
#define MAX_AIRCRAFT_LABELS 50
#define MAX_AIRCRAFT_TRACKS 512
#define AIRCRAFT_HISTORY_POINTS 10
#define MAX_AIRPORT_LABELS 64
#define MAX_AIRPORT_WEATHER 64
#define MAX_PARSED_AIRCRAFT 512
#define RANGE_RING_COUNT 5
#define HEADING_LABEL_COUNT 12
#define RADAR_GRID_MARGIN 20
#define RADAR_GRID_SPACING 50
#define HEADING_LABEL_RADIUS heading_label_radius
#define SWEEP_TIMER_MS 10
#define HTTP_RESPONSE_INITIAL_BYTES (96 * 1024)
#define HTTP_RESPONSE_MAX_BYTES (512 * 1024)
#define HTTP_READ_CHUNK_BYTES 4096
#define HTTP_TIMEOUT_MS 20000
#define HTTP_TOTAL_TIMEOUT_MS 30000
#define HTTP_NO_DATA_TIMEOUT_MS 7000
#define HTTP_RETRY_COUNT 0
#define HTTP_RETRY_DELAY_MS 1500
#define PHOTO_JSON_INITIAL_BYTES 4096
#define PHOTO_JSON_MAX_BYTES (32 * 1024)
#define PHOTO_JPEG_INITIAL_BYTES (16 * 1024)
#define PHOTO_JPEG_MAX_BYTES (128 * 1024)
#define PHOTO_HTTP_TIMEOUT_MS 8000
#define PHOTO_OPTIONAL_HTTP_TIMEOUT_MS 3000
#define PHOTO_OPTIONAL_TLS_MUTEX_TIMEOUT_MS 0
#define AIRPORTDB_JSON_INITIAL_BYTES 8192
#define AIRPORTDB_JSON_MAX_BYTES (96 * 1024)
#define AIRPORTDB_FETCH_COOLDOWN_MS (60 * 60 * 1000)
#define AIRPORTDB_CACHE_NAMESPACE "runway_cache"
#define AIRPORTDB_CACHE_VERSION 1
#define MAX_AIRPORT_RUNWAYS 16
#define HTTP_TLS_MUTEX_TIMEOUT_MS 30000
#define HTTP_BUFFER_TLS_MUTEX_TIMEOUT_MS 2500
#define PHOTO_TASK_STACK 12288
#define PHOTO_TASK_PRIORITY 1
#define PHOTO_USER_AGENT "RadarConsole/1.0 (+http://192.168.4.1/)"
#define WIFI_HTTP_RESET_TIMEOUT_MS 15000
#define FETCH_TASK_STACK 16384
#define FETCH_TASK_PRIORITY 1
#define FETCH_TASK_CORE 0
#define AIRPORT_WEATHER_TASK_STACK 12288
#define AIRPORT_WEATHER_TASK_PRIORITY 1
#define AIRPORT_WEATHER_TASK_CORE 0
#define DNS_TASK_STACK 4096
#define DNS_TASK_PRIORITY 3
#define WIFI_CONNECTED_BIT BIT0
#define FETCH_NOW_BIT BIT1
#define WIFI_PORTAL_REQUEST_BIT BIT2
#define WIFI_PORTAL_ACTIVE_BIT BIT3
#define WIFI_CREDENTIALS_CHANGED_BIT BIT4
#define MILES_PER_NAUTICAL_MILE 1.15077945f
#define COUNTRY_BOUNDARY_COORD_SCALE 100000.0
#define PI_D 3.14159265358979323846
#define PI_F 3.14159265358979323846f
#define WIFI_CRED_NAMESPACE "wifi_cfg"
#define WIFI_CRED_SSID_KEY "ssid"
#define WIFI_CRED_PASS_KEY "pass"
#define WIFI_SETUP_AP_CHANNEL 6
#define WIFI_SETUP_AP_MAX_CONN 4
#define WIFI_SETUP_AP_IP "192.168.4.1"
#define MAX_WIFI_SCAN_RESULTS 20
#define DHCPS_OFFER_DNS 0x02

/*
 * A normalised aircraft record used by the drawing layer.
 *
 * Values are already projected into the app's preferred units: miles for
 * distance, feet for altitude, knots for speed, and degrees clockwise from
 * north for bearings/headings. Text fields are fixed-size so the fetch task can
 * publish snapshots without heap ownership questions crossing into the UI task.
 */
typedef struct {
    char callsign[12];
    char detail[16];
    char icao[8];
    char registration[12];
    char type[12];
    char squawk[8];
    uint8_t db_flags;
    int bearing_deg;
    int heading_deg;
    float distance_mi;
    int altitude_ft;
    int speed_kt;
    int vertical_rate_fpm;
    float seen_s;
    bool has_db_flags;
} aircraft_data_t;

/* Runtime aircraft filter selected from the DATA menu. */
typedef enum {
    AIRCRAFT_FILTER_ALL = 0,
    AIRCRAFT_FILTER_MILITARY,
    AIRCRAFT_FILTER_INTERESTING,
} aircraft_filter_t;

/*
 * LVGL objects backing one visible aircraft label.
 *
 * The marker owns no heap memory. It only caches the last aircraft assigned to
 * the pre-created labels, which keeps redraws predictable while the sweep line
 * is animating.
 */
typedef struct {
    lv_obj_t *callsign_label;
    lv_obj_t *detail_label;
    lv_obj_t *trend_icon;
    lv_point_precise_t trend_points[3];
    aircraft_data_t data;
    bool visible;
    bool highlighted;
} radar_marker_t;

/* One remembered aircraft position used for optional range-specific history trails. */
typedef struct {
    float distance_mi;
    int bearing_deg;
} aircraft_history_point_t;

/* Fixed-size aircraft history cache keyed by ICAO hex when available. */
typedef struct {
    char key[12];
    aircraft_history_point_t points[AIRCRAFT_HISTORY_POINTS];
    uint8_t count;
    uint32_t last_generation;
} aircraft_track_history_t;

/*
 * A curved text button placed around the lower radar arc.
 *
 * Longer labels are split into individual characters so they can follow the
 * circular bezel. The plain label field is kept for short or fallback text.
 */
typedef struct {
    lv_obj_t *hitbox;
    lv_obj_t *label;
    lv_obj_t *chars[CONTROL_TEXT_MAX];
    int start_deg;
    int end_deg;
    int text_radius;
    uint32_t text_color;
    bool top_text;
    char text[CONTROL_TEXT_MAX + 1];
} curved_button_t;

/* One runway, stored as endpoint coordinates scaled by 1e6 to avoid doubles in NVS. */
typedef struct {
    int32_t le_lat_e6;
    int32_t le_lon_e6;
    int32_t he_lat_e6;
    int32_t he_lon_e6;
    char le_ident[8];
    char he_ident[8];
} airport_runway_t;

/* Cached runway data for the current airport centre. */
typedef struct {
    uint32_t version;
    char icao[5];
    uint8_t count;
    airport_runway_t runways[MAX_AIRPORT_RUNWAYS];
} airport_runway_cache_t;

/*
 * Streaming HTTP response context.
 *
 * The ESP HTTP client calls back with chunks. This object tracks the response
 * buffer, optional gzip state, and the first error raised by the callback.
 * Buffers are allocated by RadarHttpClient and must be freed by the caller once
 * a successful fetch has been consumed.
 */
typedef struct {
    bool gzip_encoded;
    bool gzip_started;
    bool gzip_finished;
    char *response;
    size_t capacity;
    size_t max_capacity;
    int length;
    int compressed_length;
    esp_err_t err;
    z_stream gzip_stream;
} http_fetch_context_t;

/* Message passed to the aircraft photo worker task. */
typedef struct {
    char icao[8];
    uint32_t request_id;
} photo_fetch_request_t;
