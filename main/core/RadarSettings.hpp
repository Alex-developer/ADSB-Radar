#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define RADAR_SETTINGS_NAMESPACE "radar_cfg"
#define RADAR_SETTINGS_KEY "settings"
#define AIRPORTDB_API_TOKEN_MAX 192
#define OPENWEATHER_API_KEY_MAX 96
#define AIRCRAFT_LOCAL_URL_MAX 192
#define MAX_RANGE_SETTINGS 10
#define MAX_NOTIFICATION_SETTINGS 10
#define RADAR_SETTINGS_MAX_LABELS 50
#define RADAR_SETTINGS_MAX_LINE_WIDTH 40
#define RADAR_SETTINGS_MAX_TICK_LENGTH 80
#define RADAR_SETTINGS_MAX_ANGLE_STEP 90
#define RADAR_SETTINGS_DEFAULT_LABELS 24
#define RADAR_SETTINGS_DEFAULT_GROUND_SPEED_KT 30
#define RADAR_SETTINGS_MAX_GROUND_SPEED_KT 250
#define RADAR_SETTINGS_DEFAULT_SWEEP_STEP_DEG 5
#define RADAR_SETTINGS_MIN_SWEEP_STEP_DEG 1
#define RADAR_SETTINGS_MAX_SWEEP_STEP_DEG 30
#define RADAR_SETTINGS_DEFAULT_SWEEP_DRAW_INTERVAL_MS 50
#define RADAR_SETTINGS_MIN_SWEEP_DRAW_INTERVAL_MS 10
#define RADAR_SETTINGS_MAX_SWEEP_DRAW_INTERVAL_MS 250
#define RADAR_SETTINGS_DEFAULT_SWEEP_TRAIL_COUNT 5
#define RADAR_SETTINGS_MAX_SWEEP_TRAIL_COUNT 50
#define RADAR_SETTINGS_DEFAULT_SWEEP_TRAIL_WIDTH 6
#define RADAR_SETTINGS_DEFAULT_SWEEP_TRAIL_STEP_DEG 1.0
#define RADAR_SETTINGS_MIN_SWEEP_TRAIL_STEP_DEG 0.1
#define RADAR_SETTINGS_MAX_SWEEP_TRAIL_STEP_DEG 10.0
#define RADAR_SETTINGS_DEFAULT_AIRPORT_WEATHER_REFRESH_MIN 15
#define RADAR_SETTINGS_MIN_AIRPORT_WEATHER_REFRESH_MIN 5
#define RADAR_SETTINGS_MAX_AIRPORT_WEATHER_REFRESH_MIN 180
#define RADAR_SETTINGS_DEFAULT_AIRPORT_WEATHER_ICON_SIZE 14
#define RADAR_SETTINGS_MIN_AIRPORT_WEATHER_ICON_SIZE 8
#define RADAR_SETTINGS_MAX_AIRPORT_WEATHER_ICON_SIZE 28
#define RADAR_SETTINGS_LABEL_FONT_MONTSERRAT 0
#define RADAR_SETTINGS_LABEL_FONT_DEFAULT 1
#define RADAR_SETTINGS_DEFAULT_AIRCRAFT_LABEL_FONT RADAR_SETTINGS_LABEL_FONT_MONTSERRAT
#define RADAR_SETTINGS_DEFAULT_AIRCRAFT_LABEL_SIZE 12
#define RADAR_SETTINGS_MIN_LABEL_SIZE 10
#define RADAR_SETTINGS_MAX_LABEL_SIZE 22
#define RADAR_CENTER_SOURCE_MANUAL 0
#define RADAR_CENTER_SOURCE_GPS 1
#define RADAR_CENTER_SOURCE_AIRPORT 2
#define RADAR_CENTER_SOURCE_LOCATION 3
#define RADAR_HEADING_STYLE_NONE 0
#define RADAR_HEADING_STYLE_ARROW 1
#define RADAR_HEADING_STYLE_LINE 2
#define AIRCRAFT_DATA_SOURCE_AIRPLANES_LIVE 0
#define AIRCRAFT_DATA_SOURCE_ADSB_LOL 1
#define AIRCRAFT_DATA_SOURCE_LOCAL 2
#define AIRCRAFT_DATA_SOURCE_ADSB_FI 3
#define AIRCRAFT_ROUTE_STYLE_SHORT 0
#define AIRCRAFT_ROUTE_STYLE_LONG 1
#define RADAR_DISPLAY_TYPE_720_720_4_INCH 0
#define RADAR_DISPLAY_TYPE_800_800_3_4_INCH 1
#define RADAR_HW_ROTARY_RANGE 0
#define RADAR_HW_ROTARY_MENU 1
#define RADAR_HW_BUTTON_NONE 0
#define RADAR_HW_BUTTON_MENU_SELECT 1
#define RADAR_HW_BUTTON_BACK_CLOSE 2
#define RADAR_HW_BUTTON_RANGE_UP 3
#define RADAR_HW_BUTTON_RANGE_DOWN 4
#define RADAR_HW_BUTTON_DATA_MENU 5
#define RADAR_HW_BUTTON_WIFI_MENU 6

/* One user-selectable radar range, including fetch cadence and label budget. */
typedef struct {
    int miles;
    int refresh_sec;
    int label_count;
    bool show_history_trail;
} range_setting_t;

/*
 * Notification rule shown at the top of the radar.
 *
 * type_match is matched exactly against aircraft type, ignoring case and
 * surrounding whitespace. Empty rules are ignored.
 * Empty rules are ignored.
 */
typedef struct {
    char type_match[16];
    bool enabled;
    bool bold_text;
    bool dim_others;
    uint32_t color;
    char text[40];
} notification_setting_t;

/*
 * All user-configurable colours, stored as 0xRRGGBB.
 *
 * Field names retain the existing code spelling, but the settings UI presents
 * them as colours. Keeping them in one struct makes it cheap to serialise to
 * JSON and to reset the UI groups back to defaults.
 */
typedef struct {
    uint32_t screen_bg;
    uint32_t radar_bg;
    uint32_t radar_glow;
    uint32_t radar_bright;
    uint32_t radar_grid;
    uint32_t radar_radial;
    uint32_t radar_tick_medium;
    uint32_t radar_tick_minor;
    uint32_t radar_axis;
    uint32_t control_arc;
    uint32_t range_label;
    uint32_t heading_label;
    uint32_t sweep;
    uint32_t country_boundary;
    uint32_t airport;
    uint32_t airport_weather;
    uint32_t runway;
    uint32_t aircraft_normal;
    uint32_t aircraft_stale;
    uint32_t aircraft_low;
    uint32_t aircraft_emergency;
    uint32_t aircraft_hit;
    uint32_t climb_triangle;
    uint32_t descent_triangle;
    uint32_t text_primary;
    uint32_t text_secondary;
    uint32_t button_text;
    uint32_t button_status;
    uint32_t button_pressed;
    uint32_t popup_bg;
    uint32_t popup_border;
    uint32_t portal_bg;
    uint32_t gps_neutral;
    uint32_t gps_lock;
    uint32_t gps_wait;
} color_setting_t;

/* Line widths for visual elements that are drawn by the canvas helpers. */
typedef struct {
    uint8_t radar_bright;
    uint8_t radar_grid;
    uint8_t radar_radial;
    uint8_t radar_tick_medium;
    uint8_t radar_tick_minor;
    uint8_t radar_axis;
    uint8_t control_arc;
    uint8_t sweep;
    uint8_t country_boundary;
    uint8_t airport;
    uint8_t runway;
    uint8_t aircraft_heading;
} line_width_setting_t;

/* Per-line visibility flags exposed by the UI settings page. */
typedef struct {
    bool radar_bright;
    bool radar_grid;
    bool radar_radial;
    bool radar_tick_medium;
    bool radar_tick_minor;
    bool radar_axis;
    bool control_arc;
    bool sweep;
    bool country_boundary;
    bool airport;
    bool runway;
    bool aircraft_heading;
} line_visibility_setting_t;

/* Geometry options for tick spacing and radial line density. */
typedef struct {
    uint8_t major_tick_length;
    uint8_t medium_tick_length;
    uint8_t minor_tick_length;
    uint8_t major_tick_degrees;
    uint8_t medium_tick_degrees;
    uint8_t minor_tick_degrees;
    uint8_t radial_degrees;
} ui_geometry_setting_t;

/* Aircraft marker colours selected by pressure altitude bands. */
typedef struct {
    uint32_t ground;
    uint32_t below_2000;
    uint32_t below_10000;
    uint32_t below_20000;
    uint32_t below_30000;
    uint32_t below_40000;
    uint32_t above_40000;
} altitude_color_setting_t;

/* Font family and size for text drawn on the radar face. */
typedef struct {
    uint8_t font;
    uint8_t size;
} label_font_setting_t;

/* User-configurable radar label typography. */
typedef struct {
    label_font_setting_t aircraft;
    label_font_setting_t aircraft_notification;
    label_font_setting_t range_label;
    label_font_setting_t heading_label;
    label_font_setting_t airport;
    label_font_setting_t button;
    label_font_setting_t notification;
    label_font_setting_t gps;
} label_style_setting_t;

/*
 * Complete persistent settings model.
 *
 * This struct is intentionally plain data because it is cloned, validated,
 * serialised to JSON, and saved to NVS. Runtime services should copy values out
 * under their own locks rather than keeping references to a candidate settings
 * object from a web request.
 */
typedef struct {
    double center_lat;
    double center_lon;
    int center_source;
    double center_airport_lat;
    double center_airport_lon;
    char center_airport_name[64];
    char center_airport_code[5];
    double center_location_lat;
    double center_location_lon;
    char center_location_name[64];
    char center_location_state[48];
    char center_location_country[4];
    int default_range_mi;
    int display_type;
    bool show_sweep;
    int sweep_step_deg;
    int sweep_draw_interval_ms;
    bool show_sweep_trail;
    int sweep_trail_count;
    int sweep_trail_width;
    double sweep_trail_step_deg;
    bool show_airports;
    bool show_airport_weather;
    int airport_weather_refresh_min;
    int airport_weather_icon_size;
    bool emergency_squawks_red;
    bool show_aircraft_heading;
    int aircraft_heading_style;
    bool show_climb_descent;
    bool show_aircraft_routes;
    int aircraft_route_style;
    bool show_countries;
    bool show_airport_runways;
    char airportdb_api_token[AIRPORTDB_API_TOKEN_MAX];
    char openweather_api_key[OPENWEATHER_API_KEY_MAX];
    int aircraft_data_source;
    char aircraft_local_url[AIRCRAFT_LOCAL_URL_MAX];
    color_setting_t colors;
    line_width_setting_t widths;
    line_visibility_setting_t visible;
    ui_geometry_setting_t ui;
    range_setting_t ranges[MAX_RANGE_SETTINGS];
    notification_setting_t notifications[MAX_NOTIFICATION_SETTINGS];
    bool show_ground_aircraft;
    int ground_speed_kt;
    altitude_color_setting_t altitude_colors;
    label_style_setting_t label_styles;
    bool hardware_controls_enabled;
    int hardware_rotary_action;
    int hardware_confirm_action;
    int hardware_back_action;
    int hardware_push_action;
    int hardware_menu_timeout_sec;
    bool hardware_show_hints;
    int hardware_oled_i2c_addr;
    int hardware_confirm_gpio;
    int hardware_back_gpio;
    int hardware_rotary_a_gpio;
    int hardware_rotary_b_gpio;
    int hardware_rotary_push_gpio;
} radar_settings_t;

/*
 * Persistent radar configuration.
 *
 * RadarSettings owns defaulting, validation, NVS persistence, and the JSON
 * contract used by the browser settings page. The class inherits the plain
 * struct so existing drawing and service code can read settings fields without
 * accessor noise on this small embedded application.
 */
class RadarSettings : public radar_settings_t {
public:
    RadarSettings();

    /* Reset every field to a known-good configuration. */
    void setDefaults();

    /* Load from NVS, falling back to defaults if the stored document is invalid. */
    void load();

    /* Save the current settings as JSON in NVS. */
    esp_err_t save() const;

    /* Validate the current object after loading or editing. */
    bool isValid() const;

    /* Validate a candidate before it replaces the live settings. */
    static bool isValid(const radar_settings_t &candidate);

    /* Replace the live settings with an already validated candidate. */
    void apply(const radar_settings_t &candidate);

    /* Restore only the range table to the built-in defaults. */
    void resetRangesToDefaults();

    /* Count non-empty ranges in the live settings. */
    size_t rangeCount() const;

    /* Count non-empty ranges in an arbitrary settings object. */
    static size_t rangeCount(const radar_settings_t &candidate);

    /*
     * Parse the settings API payload into candidate.
     *
     * The live settings are not modified. On failure, error receives a short
     * human-readable reason that can be returned by the web server.
     */
    bool parseJson(const char *json, radar_settings_t *candidate,
                   char *error, size_t error_size) const;

    /*
     * Serialise the current settings for the browser UI.
     *
     * The returned string is allocated with cJSON's allocator and must be freed
     * by the caller.
     */
    char *toJson(const char *wifi_ssid) const;

    /* Format a 0xRRGGBB value as #rrggbb. */
    static void colorToHex(uint32_t color, char *dst, size_t dst_size);
};
