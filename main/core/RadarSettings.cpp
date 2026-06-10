#include "RadarSettings.hpp"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "nvs.h"
#include "sdkconfig.h"

/*
 * Settings are saved as a plain settings blob in NVS. During active development
 * an old blob size is ignored and defaults are used until the current schema is
 * saved again from the browser.
 */

namespace {

/* Clamp a settings integer to an inclusive range. */
int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

/* Read a boolean JSON field, accepting numeric legacy values as well. */
bool parse_bool_item(cJSON *object, const char *name, bool fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    if (cJSON_IsNumber(item)) {
        return item->valueint != 0;
    }
    return fallback;
}

/* Read an integer JSON field with a fallback for missing or malformed values. */
int parse_int_item(cJSON *object, const char *name, int fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

/* Read a floating-point JSON field with a fallback for missing values. */
double parse_double_item(cJSON *object, const char *name, double fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

/* Parse a colour from either a number or a #rrggbb string. */
uint32_t parse_color_value(cJSON *item, uint32_t fallback)
{
    if (cJSON_IsNumber(item)) {
        int value = item->valueint;
        return value < 0 ? fallback : ((uint32_t)value & 0xffffff);
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        return fallback;
    }

    const char *text = item->valuestring;
    if (*text == '#') {
        ++text;
    }
    char *end = nullptr;
    unsigned long value = strtoul(text, &end, 16);
    if (end == text || value > 0xffffff) {
        return fallback;
    }
    return (uint32_t)value;
}

/* Copy and trim a JSON string field into a fixed-size settings buffer. */
void copy_json_string(char *dst, size_t dst_size, cJSON *object, const char *name)
{
    if (!dst || dst_size == 0) {
        return;
    }
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

/* Store a short validation error message for the settings API. */
void set_error(char *error, size_t error_size, const char *text)
{
    if (error && error_size > 0) {
        snprintf(error, error_size, "%s", text);
    }
}

/* Validate that an API token buffer contains printable text and is terminated. */
bool valid_api_token(const char *token, size_t token_size)
{
    if (!token || token_size == 0) {
        return false;
    }

    size_t len = strnlen(token, token_size);
    if (len >= token_size) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)token[i];
        if (ch < 33 || ch > 126) {
            return false;
        }
    }
    return true;
}

/* Validate a stored aircraft feed URL for local receiver mode. */
bool valid_aircraft_url(const char *url, size_t url_size, bool required)
{
    if (!url || url_size == 0) {
        return false;
    }

    size_t len = strnlen(url, url_size);
    if (len >= url_size) {
        return false;
    }
    if (len == 0) {
        return !required;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)url[i];
        if (ch < 33 || ch > 126) {
            return false;
        }
    }
    return true;
}

#define RADAR_COLOR_FIELDS(X) \
    X(screen_bg, 0x010503) \
    X(radar_bg, 0x03160a) \
    X(radar_glow, 0x0d4d1e) \
    X(radar_bright, 0x54ff74) \
    X(radar_grid, 0x0b3f1d) \
    X(radar_radial, 0x136b2d) \
    X(radar_tick_medium, 0x2bd857) \
    X(radar_tick_minor, 0x157234) \
    X(radar_axis, 0x35db5f) \
    X(control_arc, 0x124c45) \
    X(range_label, 0x8cff9e) \
    X(heading_label, 0x54ff74) \
    X(sweep, 0xcffcf3) \
    X(country_boundary, 0x7dd3fc) \
    X(airport, 0x9ee6a6) \
    X(runway, 0xcffcf3) \
    X(aircraft_normal, 0x34d399) \
    X(aircraft_stale, 0x2dd4bf) \
    X(aircraft_low, 0xfacc15) \
    X(aircraft_emergency, 0xff3030) \
    X(aircraft_hit, 0xfff08a) \
    X(climb_triangle, 0x86efac) \
    X(descent_triangle, 0xf97316) \
    X(text_primary, 0xa7f3d0) \
    X(text_secondary, 0x7dd3bf) \
    X(button_text, 0xf8fafc) \
    X(button_status, 0x5eead4) \
    X(button_pressed, 0x123d3a) \
    X(popup_bg, 0x03160f) \
    X(popup_border, 0x5eead4) \
    X(portal_bg, 0x020806) \
    X(gps_neutral, 0x94a3b8) \
    X(gps_lock, 0x86efac) \
    X(gps_wait, 0xfacc15)

#define RADAR_WIDTH_FIELDS(X) \
    X(radar_bright, 2) \
    X(radar_grid, 1) \
    X(radar_radial, 1) \
    X(radar_tick_medium, 1) \
    X(radar_tick_minor, 1) \
    X(radar_axis, 1) \
    X(control_arc, 20) \
    X(sweep, 3) \
    X(country_boundary, 1) \
    X(airport, 1) \
    X(runway, 2) \
    X(aircraft_heading, 1)

#define RADAR_VISIBLE_FIELDS(X) \
    X(radar_bright, true) \
    X(radar_grid, true) \
    X(radar_radial, true) \
    X(radar_tick_medium, true) \
    X(radar_tick_minor, true) \
    X(radar_axis, true) \
    X(control_arc, true) \
    X(sweep, true) \
    X(country_boundary, true) \
    X(airport, true) \
    X(runway, true) \
    X(aircraft_heading, true)

#define RADAR_UI_FIELDS(X) \
    X(major_tick_length, 30, 1, RADAR_SETTINGS_MAX_TICK_LENGTH) \
    X(medium_tick_length, 18, 1, RADAR_SETTINGS_MAX_TICK_LENGTH) \
    X(minor_tick_length, 10, 1, RADAR_SETTINGS_MAX_TICK_LENGTH) \
    X(major_tick_degrees, 30, 1, RADAR_SETTINGS_MAX_ANGLE_STEP) \
    X(medium_tick_degrees, 10, 1, RADAR_SETTINGS_MAX_ANGLE_STEP) \
    X(minor_tick_degrees, 5, 1, RADAR_SETTINGS_MAX_ANGLE_STEP) \
    X(radial_degrees, 30, 1, RADAR_SETTINGS_MAX_ANGLE_STEP)

#define RADAR_ALTITUDE_COLOR_FIELDS(X) \
    X(ground, 0x9ca3af) \
    X(below_2000, 0xff2d2d) \
    X(below_10000, 0xffa22d) \
    X(below_20000, 0xffe15c) \
    X(below_30000, 0x4ade80) \
    X(below_40000, 0x38bdf8) \
    X(above_40000, 0xa78bfa)

#define RADAR_LABEL_STYLE_FIELDS(X) \
    X(aircraft, RADAR_SETTINGS_LABEL_FONT_MONTSERRAT, 12) \
    X(aircraft_notification, RADAR_SETTINGS_LABEL_FONT_MONTSERRAT, 14) \
    X(range_label, RADAR_SETTINGS_LABEL_FONT_MONTSERRAT, 12) \
    X(heading_label, RADAR_SETTINGS_LABEL_FONT_MONTSERRAT, 12) \
    X(airport, RADAR_SETTINGS_LABEL_FONT_MONTSERRAT, 12) \
    X(button, RADAR_SETTINGS_LABEL_FONT_MONTSERRAT, 16) \
    X(notification, RADAR_SETTINGS_LABEL_FONT_MONTSERRAT, 16) \
    X(gps, RADAR_SETTINGS_LABEL_FONT_MONTSERRAT, 12)

/* Populate the default colour palette. */
void set_default_colors(color_setting_t *colors)
{
    if (!colors) {
        return;
    }
    /* Keep visual defaults grouped here so the UI reset controls match drawing code. */
#define SET_DEFAULT_COLOR(name, value) colors->name = value;
    RADAR_COLOR_FIELDS(SET_DEFAULT_COLOR)
#undef SET_DEFAULT_COLOR
}

/* Populate the default ADSB-style altitude colour bands. */
void set_default_altitude_colors(altitude_color_setting_t *colors)
{
    if (!colors) {
        return;
    }
#define SET_DEFAULT_ALTITUDE_COLOR(name, value) colors->name = value;
    RADAR_ALTITUDE_COLOR_FIELDS(SET_DEFAULT_ALTITUDE_COLOR)
#undef SET_DEFAULT_ALTITUDE_COLOR
}

/* Populate the default font family and size for radar text labels. */
void set_default_label_styles(label_style_setting_t *styles)
{
    if (!styles) {
        return;
    }
#define SET_DEFAULT_LABEL_STYLE(name, font_value, size_value) \
    styles->name.font = font_value; \
    styles->name.size = size_value;
    RADAR_LABEL_STYLE_FIELDS(SET_DEFAULT_LABEL_STYLE)
#undef SET_DEFAULT_LABEL_STYLE
}

/* Populate the default widths for every line-drawn UI element. */
void set_default_widths(line_width_setting_t *widths)
{
    if (!widths) {
        return;
    }
#define SET_DEFAULT_WIDTH(name, value) widths->name = value;
    RADAR_WIDTH_FIELDS(SET_DEFAULT_WIDTH)
#undef SET_DEFAULT_WIDTH
}

/* Populate the default visibility flags for optional line layers. */
void set_default_visible(line_visibility_setting_t *visible)
{
    if (!visible) {
        return;
    }
#define SET_DEFAULT_VISIBLE(name, value) visible->name = value;
    RADAR_VISIBLE_FIELDS(SET_DEFAULT_VISIBLE)
#undef SET_DEFAULT_VISIBLE
}

/* Populate default geometry controls for ticks and radial lines. */
void set_default_ui(ui_geometry_setting_t *ui)
{
    if (!ui) {
        return;
    }
#define SET_DEFAULT_UI(name, value, min_value, max_value) ui->name = value;
    RADAR_UI_FIELDS(SET_DEFAULT_UI)
#undef SET_DEFAULT_UI
}

/* Overlay colour values from the settings JSON onto an existing palette. */
void parse_color_settings(cJSON *object, color_setting_t *colors)
{
    if (!cJSON_IsObject(object) || !colors) {
        return;
    }
#define PARSE_COLOR(name, value) \
    colors->name = parse_color_value(cJSON_GetObjectItemCaseSensitive(object, #name), colors->name);
    RADAR_COLOR_FIELDS(PARSE_COLOR)
#undef PARSE_COLOR
}

/* Overlay altitude marker colour values from the settings JSON. */
void parse_altitude_color_settings(cJSON *object, altitude_color_setting_t *colors)
{
    if (!cJSON_IsObject(object) || !colors) {
        return;
    }
#define PARSE_ALTITUDE_COLOR(name, value) \
    colors->name = parse_color_value(cJSON_GetObjectItemCaseSensitive(object, #name), colors->name);
    RADAR_ALTITUDE_COLOR_FIELDS(PARSE_ALTITUDE_COLOR)
#undef PARSE_ALTITUDE_COLOR
}

/* Parse one label style object from settings JSON. */
void parse_one_label_style(cJSON *object, const char *name, label_font_setting_t *style)
{
    if (!cJSON_IsObject(object) || !style) {
        return;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsObject(item)) {
        return;
    }
    cJSON *font = cJSON_GetObjectItemCaseSensitive(item, "font");
    if (cJSON_IsString(font) && font->valuestring) {
        style->font = strcmp(font->valuestring, "default") == 0 ?
                      RADAR_SETTINGS_LABEL_FONT_DEFAULT : RADAR_SETTINGS_LABEL_FONT_MONTSERRAT;
    }
    style->size = (uint8_t)clamp_int(parse_int_item(item, "size", style->size),
                                     RADAR_SETTINGS_MIN_LABEL_SIZE,
                                     RADAR_SETTINGS_MAX_LABEL_SIZE);
}

/* Overlay label font settings from the settings JSON. */
void parse_label_style_settings(cJSON *object, label_style_setting_t *styles)
{
    if (!cJSON_IsObject(object) || !styles) {
        return;
    }
#define PARSE_LABEL_STYLE(name, font_value, size_value) parse_one_label_style(object, #name, &styles->name);
    RADAR_LABEL_STYLE_FIELDS(PARSE_LABEL_STYLE)
#undef PARSE_LABEL_STYLE
}

/* Overlay line width values from settings JSON, clamping unsafe input. */
void parse_width_settings(cJSON *object, line_width_setting_t *widths)
{
    if (!cJSON_IsObject(object) || !widths) {
        return;
    }
#define PARSE_WIDTH(name, value) \
    widths->name = (uint8_t)clamp_int(parse_int_item(object, #name, widths->name), 1, RADAR_SETTINGS_MAX_LINE_WIDTH);
    RADAR_WIDTH_FIELDS(PARSE_WIDTH)
#undef PARSE_WIDTH
}

/* Overlay line visibility flags from settings JSON. */
void parse_visible_settings(cJSON *object, line_visibility_setting_t *visible)
{
    if (!cJSON_IsObject(object) || !visible) {
        return;
    }
#define PARSE_VISIBLE(name, value) \
    visible->name = parse_bool_item(object, #name, visible->name);
    RADAR_VISIBLE_FIELDS(PARSE_VISIBLE)
#undef PARSE_VISIBLE
}

/* Overlay tick and radial geometry settings from JSON. */
void parse_ui_settings(cJSON *object, ui_geometry_setting_t *ui)
{
    if (!cJSON_IsObject(object) || !ui) {
        return;
    }
#define PARSE_UI(name, value, min_value, max_value) \
    ui->name = (uint8_t)clamp_int(parse_int_item(object, #name, ui->name), min_value, max_value);
    RADAR_UI_FIELDS(PARSE_UI)
#undef PARSE_UI
}

/* Add the colour palette to the settings JSON response. */
void add_color_settings_json(cJSON *root, const color_setting_t *colors)
{
    cJSON *color_object = cJSON_AddObjectToObject(root, "colors");
    if (!color_object || !colors) {
        return;
    }

    char text[8];
#define ADD_COLOR_JSON(name, value) \
    RadarSettings::colorToHex(colors->name, text, sizeof(text)); \
    cJSON_AddStringToObject(color_object, #name, text);
    RADAR_COLOR_FIELDS(ADD_COLOR_JSON)
#undef ADD_COLOR_JSON
}

/* Add the altitude marker palette to the settings JSON response. */
void add_altitude_color_settings_json(cJSON *root, const altitude_color_setting_t *colors)
{
    cJSON *color_object = cJSON_AddObjectToObject(root, "altitudeColors");
    if (!color_object || !colors) {
        return;
    }

    char text[8];
#define ADD_ALTITUDE_COLOR_JSON(name, value) \
    RadarSettings::colorToHex(colors->name, text, sizeof(text)); \
    cJSON_AddStringToObject(color_object, #name, text);
    RADAR_ALTITUDE_COLOR_FIELDS(ADD_ALTITUDE_COLOR_JSON)
#undef ADD_ALTITUDE_COLOR_JSON
}

/* Add one label style object to the settings JSON response. */
void add_one_label_style_json(cJSON *parent, const char *name, const label_font_setting_t *style)
{
    cJSON *item = cJSON_AddObjectToObject(parent, name);
    if (!item || !style) {
        return;
    }
    cJSON_AddStringToObject(item, "font",
                            style->font == RADAR_SETTINGS_LABEL_FONT_DEFAULT ?
                            "default" : "montserrat");
    cJSON_AddNumberToObject(item, "size", style->size);
}

/* Add label font settings to the settings JSON response. */
void add_label_style_settings_json(cJSON *root, const label_style_setting_t *styles)
{
    cJSON *style_object = cJSON_AddObjectToObject(root, "labelStyles");
    if (!style_object || !styles) {
        return;
    }
#define ADD_LABEL_STYLE_JSON(name, font_value, size_value) \
    add_one_label_style_json(style_object, #name, &styles->name);
    RADAR_LABEL_STYLE_FIELDS(ADD_LABEL_STYLE_JSON)
#undef ADD_LABEL_STYLE_JSON
}

/* Add line widths to the settings JSON response. */
void add_width_settings_json(cJSON *root, const line_width_setting_t *widths)
{
    cJSON *width_object = cJSON_AddObjectToObject(root, "widths");
    if (!width_object || !widths) {
        return;
    }
#define ADD_WIDTH_JSON(name, value) cJSON_AddNumberToObject(width_object, #name, widths->name);
    RADAR_WIDTH_FIELDS(ADD_WIDTH_JSON)
#undef ADD_WIDTH_JSON
}

/* Add line visibility flags to the settings JSON response. */
void add_visible_settings_json(cJSON *root, const line_visibility_setting_t *visible)
{
    cJSON *visible_object = cJSON_AddObjectToObject(root, "visible");
    if (!visible_object || !visible) {
        return;
    }
#define ADD_VISIBLE_JSON(name, value) cJSON_AddBoolToObject(visible_object, #name, visible->name);
    RADAR_VISIBLE_FIELDS(ADD_VISIBLE_JSON)
#undef ADD_VISIBLE_JSON
}

/* Add tick and radial geometry settings to the settings JSON response. */
void add_ui_settings_json(cJSON *root, const ui_geometry_setting_t *ui)
{
    cJSON *ui_object = cJSON_AddObjectToObject(root, "ui");
    if (!ui_object || !ui) {
        return;
    }
#define ADD_UI_JSON(name, value, min_value, max_value) cJSON_AddNumberToObject(ui_object, #name, ui->name);
    RADAR_UI_FIELDS(ADD_UI_JSON)
#undef ADD_UI_JSON
}

/* Validate every line width is within the supported drawing range. */
bool valid_width_settings(const line_width_setting_t *widths)
{
    if (!widths) {
        return false;
    }
#define CHECK_WIDTH(name, value) \
    if (widths->name < 1 || widths->name > RADAR_SETTINGS_MAX_LINE_WIDTH) { return false; }
    RADAR_WIDTH_FIELDS(CHECK_WIDTH)
#undef CHECK_WIDTH
    return true;
}

/* Validate every tick and radial geometry control is within its safe range. */
bool valid_ui_settings(const ui_geometry_setting_t *ui)
{
    if (!ui) {
        return false;
    }
#define CHECK_UI(name, value, min_value, max_value) \
    if (ui->name < min_value || ui->name > max_value) { return false; }
    RADAR_UI_FIELDS(CHECK_UI)
#undef CHECK_UI
    return true;
}

/* Validate one label font/size pair. */
bool valid_label_style(const label_font_setting_t *style)
{
    if (!style) {
        return false;
    }
    return (style->font == RADAR_SETTINGS_LABEL_FONT_MONTSERRAT ||
            style->font == RADAR_SETTINGS_LABEL_FONT_DEFAULT) &&
           style->size >= RADAR_SETTINGS_MIN_LABEL_SIZE &&
           style->size <= RADAR_SETTINGS_MAX_LABEL_SIZE;
}

/* Validate all label typography settings. */
bool valid_label_style_settings(const label_style_setting_t *styles)
{
    if (!styles) {
        return false;
    }
#define CHECK_LABEL_STYLE(name, font_value, size_value) \
    if (!valid_label_style(&styles->name)) { return false; }
    RADAR_LABEL_STYLE_FIELDS(CHECK_LABEL_STYLE)
#undef CHECK_LABEL_STYLE
    return true;
}

} // namespace

/* Construct settings with defaults so every field starts valid. */
RadarSettings::RadarSettings()
{
    setDefaults();
}

/* Reset all persisted settings fields to their factory defaults. */
void RadarSettings::setDefaults()
{
    memset(static_cast<radar_settings_t *>(this), 0, sizeof(radar_settings_t));
    center_lat = atof(CONFIG_RADAR_CENTER_LAT);
    center_lon = atof(CONFIG_RADAR_CENTER_LON);
    center_source = RADAR_CENTER_SOURCE_MANUAL;
    center_airport_lat = center_lat;
    center_airport_lon = center_lon;
    center_location_lat = center_lat;
    center_location_lon = center_lon;
    default_range_mi = 50;
    show_sweep = true;
    sweep_step_deg = RADAR_SETTINGS_DEFAULT_SWEEP_STEP_DEG;
    sweep_draw_interval_ms = RADAR_SETTINGS_DEFAULT_SWEEP_DRAW_INTERVAL_MS;
    show_sweep_trail = true;
    sweep_trail_count = RADAR_SETTINGS_DEFAULT_SWEEP_TRAIL_COUNT;
    sweep_trail_width = RADAR_SETTINGS_DEFAULT_SWEEP_TRAIL_WIDTH;
    sweep_trail_step_deg = RADAR_SETTINGS_DEFAULT_SWEEP_TRAIL_STEP_DEG;
    show_airports = true;
    emergency_squawks_red = true;
    show_aircraft_heading = true;
    aircraft_heading_style = RADAR_HEADING_STYLE_ARROW;
    show_climb_descent = false;
    show_aircraft_routes = true;
    aircraft_route_style = AIRCRAFT_ROUTE_STYLE_SHORT;
    show_ground_aircraft = false;
    ground_speed_kt = RADAR_SETTINGS_DEFAULT_GROUND_SPEED_KT;
    show_countries = true;
    show_airport_runways = false;
    aircraft_data_source = AIRCRAFT_DATA_SOURCE_AIRPLANES_LIVE;
    snprintf(aircraft_local_url, sizeof(aircraft_local_url), "%s", "");
    set_default_colors(&colors);
    set_default_altitude_colors(&altitude_colors);
    set_default_widths(&widths);
    set_default_visible(&visible);
    set_default_ui(&ui);
    set_default_label_styles(&label_styles);

    const int default_ranges[] = {5, 10, 20, 30, 40, 50, 75, 100, 150, 250};
    const size_t default_range_count = sizeof(default_ranges) / sizeof(default_ranges[0]);
    for (size_t i = 0; i < MAX_RANGE_SETTINGS; ++i) {
        ranges[i].miles = i < default_range_count ? default_ranges[i] : 0;
        ranges[i].refresh_sec = CONFIG_RADAR_FETCH_INTERVAL_SEC;
        ranges[i].label_count = RADAR_SETTINGS_DEFAULT_LABELS;
    }

    for (size_t i = 0; i < MAX_NOTIFICATION_SETTINGS; ++i) {
        notifications[i].enabled = true;
        notifications[i].bold_text = false;
        notifications[i].dim_others = false;
        notifications[i].color = 0xffb020;
    }
}

/* Load settings from NVS, falling back to defaults on missing or invalid data. */
void RadarSettings::load()
{
    setDefaults();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(RADAR_SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return;
    }

    size_t size = 0;
    err = nvs_get_blob(handle, RADAR_SETTINGS_KEY, nullptr, &size);
    if (err != ESP_OK || size != sizeof(radar_settings_t)) {
        nvs_close(handle);
        return;
    }

    radar_settings_t stored = *static_cast<radar_settings_t *>(this);
    err = nvs_get_blob(handle, RADAR_SETTINGS_KEY, &stored, &size);
    nvs_close(handle);
    if (err == ESP_OK && isValid(stored)) {
        if (stored.colors.country_boundary == 0x1d6c4c) {
            stored.colors.country_boundary = colors.country_boundary;
        }
        apply(stored);
    }
}

/* Save the current settings blob to NVS. */
esp_err_t RadarSettings::save() const
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(RADAR_SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    const radar_settings_t *data = static_cast<const radar_settings_t *>(this);
    err = nvs_set_blob(handle, RADAR_SETTINGS_KEY, data, sizeof(*data));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/* Validate the current settings object. */
bool RadarSettings::isValid() const
{
    return isValid(*static_cast<const radar_settings_t *>(this));
}

/* Validate an arbitrary candidate settings object before it is applied. */
bool RadarSettings::isValid(const radar_settings_t &candidate)
{
    if (candidate.center_lat < -90.0 || candidate.center_lat > 90.0 ||
        candidate.center_lon < -180.0 || candidate.center_lon > 180.0 ||
        (candidate.center_source != RADAR_CENTER_SOURCE_MANUAL &&
         candidate.center_source != RADAR_CENTER_SOURCE_GPS &&
         candidate.center_source != RADAR_CENTER_SOURCE_AIRPORT &&
         candidate.center_source != RADAR_CENTER_SOURCE_LOCATION) ||
        candidate.center_airport_lat < -90.0 || candidate.center_airport_lat > 90.0 ||
        candidate.center_airport_lon < -180.0 || candidate.center_airport_lon > 180.0 ||
        candidate.center_location_lat < -90.0 || candidate.center_location_lat > 90.0 ||
        candidate.center_location_lon < -180.0 || candidate.center_location_lon > 180.0 ||
        candidate.default_range_mi < 1 || candidate.default_range_mi > 250 ||
        candidate.sweep_step_deg < RADAR_SETTINGS_MIN_SWEEP_STEP_DEG ||
        candidate.sweep_step_deg > RADAR_SETTINGS_MAX_SWEEP_STEP_DEG ||
        candidate.sweep_draw_interval_ms < RADAR_SETTINGS_MIN_SWEEP_DRAW_INTERVAL_MS ||
        candidate.sweep_draw_interval_ms > RADAR_SETTINGS_MAX_SWEEP_DRAW_INTERVAL_MS ||
        candidate.sweep_trail_count < 0 ||
        candidate.sweep_trail_count > RADAR_SETTINGS_MAX_SWEEP_TRAIL_COUNT ||
        candidate.sweep_trail_width < 1 ||
        candidate.sweep_trail_width > RADAR_SETTINGS_MAX_LINE_WIDTH ||
        candidate.sweep_trail_step_deg < RADAR_SETTINGS_MIN_SWEEP_TRAIL_STEP_DEG ||
        candidate.sweep_trail_step_deg > RADAR_SETTINGS_MAX_SWEEP_TRAIL_STEP_DEG ||
        candidate.aircraft_heading_style < RADAR_HEADING_STYLE_NONE ||
        candidate.aircraft_heading_style > RADAR_HEADING_STYLE_LINE ||
        candidate.ground_speed_kt < 0 ||
        candidate.ground_speed_kt > RADAR_SETTINGS_MAX_GROUND_SPEED_KT) {
        return false;
    }

    if (!valid_api_token(candidate.airportdb_api_token, sizeof(candidate.airportdb_api_token))) {
        return false;
    }
    if (!valid_api_token(candidate.openweather_api_key, sizeof(candidate.openweather_api_key))) {
        return false;
    }
    if (candidate.aircraft_data_source != AIRCRAFT_DATA_SOURCE_AIRPLANES_LIVE &&
        candidate.aircraft_data_source != AIRCRAFT_DATA_SOURCE_ADSB_LOL &&
        candidate.aircraft_data_source != AIRCRAFT_DATA_SOURCE_ADSB_FI &&
        candidate.aircraft_data_source != AIRCRAFT_DATA_SOURCE_LOCAL) {
        return false;
    }
    if (!valid_aircraft_url(candidate.aircraft_local_url, sizeof(candidate.aircraft_local_url),
                            candidate.aircraft_data_source == AIRCRAFT_DATA_SOURCE_LOCAL)) {
        return false;
    }

    if (!valid_width_settings(&candidate.widths)) {
        return false;
    }

    if (!valid_ui_settings(&candidate.ui)) {
        return false;
    }

    if (!valid_label_style_settings(&candidate.label_styles)) {
        return false;
    }

    bool has_range = false;
    bool default_in_ranges = false;
    for (size_t i = 0; i < MAX_RANGE_SETTINGS; ++i) {
        const range_setting_t *range = &candidate.ranges[i];
        if (range->miles == 0) {
            continue;
        }
        if (range->miles < 1 || range->miles > 250 ||
            range->refresh_sec < 2 || range->refresh_sec > 300 ||
            range->label_count < 0 || range->label_count > RADAR_SETTINGS_MAX_LABELS) {
            return false;
        }
        if (range->miles == candidate.default_range_mi) {
            default_in_ranges = true;
        }
        has_range = true;
    }
    return has_range && default_in_ranges;
}

/* Replace the current settings with a pre-validated candidate. */
void RadarSettings::apply(const radar_settings_t &candidate)
{
    *static_cast<radar_settings_t *>(this) = candidate;
}

/* Restore only the range table and default range to factory values. */
void RadarSettings::resetRangesToDefaults()
{
    RadarSettings defaults;
    memcpy(ranges, defaults.ranges, sizeof(ranges));
    default_range_mi = defaults.default_range_mi;
}

/* Count active range entries in the current settings. */
size_t RadarSettings::rangeCount() const
{
    return rangeCount(*static_cast<const radar_settings_t *>(this));
}

/* Count active range entries in an arbitrary settings object. */
size_t RadarSettings::rangeCount(const radar_settings_t &candidate)
{
    size_t count = 0;
    for (size_t i = 0; i < MAX_RANGE_SETTINGS; ++i) {
        if (candidate.ranges[i].miles > 0) {
            ++count;
        }
    }
    return count;
}

/* Parse a browser settings JSON payload into a validated candidate. */
bool RadarSettings::parseJson(const char *json, radar_settings_t *candidate,
                              char *error, size_t error_size) const
{
    if (!json || !candidate) {
        set_error(error, error_size, "Invalid request");
        return false;
    }

    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        set_error(error, error_size, "Invalid JSON");
        return false;
    }

    *candidate = *static_cast<const radar_settings_t *>(this);
    cJSON *general = cJSON_GetObjectItemCaseSensitive(root, "general");
    if (cJSON_IsObject(general)) {
        candidate->center_lat = parse_double_item(general, "lat", candidate->center_lat);
        candidate->center_lon = parse_double_item(general, "lon", candidate->center_lon);
        candidate->default_range_mi = parse_int_item(general, "defaultRange", candidate->default_range_mi);
        cJSON *source = cJSON_GetObjectItemCaseSensitive(general, "centerSource");
        if (cJSON_IsString(source) && source->valuestring) {
            if (strcmp(source->valuestring, "gps") == 0) {
                candidate->center_source = RADAR_CENTER_SOURCE_GPS;
            } else if (strcmp(source->valuestring, "airport") == 0) {
                candidate->center_source = RADAR_CENTER_SOURCE_AIRPORT;
            } else if (strcmp(source->valuestring, "location") == 0) {
                candidate->center_source = RADAR_CENTER_SOURCE_LOCATION;
            } else {
                candidate->center_source = RADAR_CENTER_SOURCE_MANUAL;
            }
        } else if (cJSON_IsBool(source)) {
            candidate->center_source = cJSON_IsTrue(source) ?
                                       RADAR_CENTER_SOURCE_GPS : RADAR_CENTER_SOURCE_MANUAL;
        }

        cJSON *airport = cJSON_GetObjectItemCaseSensitive(general, "airport");
        if (cJSON_IsObject(airport)) {
            candidate->center_airport_lat = parse_double_item(airport, "lat", candidate->center_airport_lat);
            candidate->center_airport_lon = parse_double_item(airport, "lon", candidate->center_airport_lon);
            copy_json_string(candidate->center_airport_name, sizeof(candidate->center_airport_name),
                             airport, "name");
            copy_json_string(candidate->center_airport_code, sizeof(candidate->center_airport_code),
                             airport, "code");
        }

        cJSON *location = cJSON_GetObjectItemCaseSensitive(general, "location");
        if (cJSON_IsObject(location)) {
            candidate->center_location_lat = parse_double_item(location, "lat", candidate->center_location_lat);
            candidate->center_location_lon = parse_double_item(location, "lon", candidate->center_location_lon);
            copy_json_string(candidate->center_location_name, sizeof(candidate->center_location_name),
                             location, "name");
            copy_json_string(candidate->center_location_state, sizeof(candidate->center_location_state),
                             location, "state");
            copy_json_string(candidate->center_location_country, sizeof(candidate->center_location_country),
                             location, "country");
        }

        cJSON *data_source = cJSON_GetObjectItemCaseSensitive(general, "dataSource");
        if (cJSON_IsString(data_source) && data_source->valuestring) {
            if (strcmp(data_source->valuestring, "adsb_lol") == 0) {
                candidate->aircraft_data_source = AIRCRAFT_DATA_SOURCE_ADSB_LOL;
            } else if (strcmp(data_source->valuestring, "adsb_fi") == 0) {
                candidate->aircraft_data_source = AIRCRAFT_DATA_SOURCE_ADSB_FI;
            } else if (strcmp(data_source->valuestring, "local") == 0) {
                candidate->aircraft_data_source = AIRCRAFT_DATA_SOURCE_LOCAL;
            } else {
                candidate->aircraft_data_source = AIRCRAFT_DATA_SOURCE_AIRPLANES_LIVE;
            }
        }
        copy_json_string(candidate->aircraft_local_url, sizeof(candidate->aircraft_local_url),
                         general, "localAircraftUrl");

    }

    cJSON *api_keys = cJSON_GetObjectItemCaseSensitive(root, "apiKeys");
    if (cJSON_IsObject(api_keys)) {
        copy_json_string(candidate->airportdb_api_token, sizeof(candidate->airportdb_api_token),
                         api_keys, "airportDbToken");
        copy_json_string(candidate->openweather_api_key, sizeof(candidate->openweather_api_key),
                         api_keys, "openWeatherApiKey");
    }

    cJSON *interface = cJSON_GetObjectItemCaseSensitive(root, "interface");
    if (cJSON_IsObject(interface)) {
        candidate->show_sweep = parse_bool_item(interface, "showSweep", candidate->show_sweep);
        candidate->sweep_step_deg =
            clamp_int(parse_int_item(interface, "sweepStepDeg", candidate->sweep_step_deg),
                      RADAR_SETTINGS_MIN_SWEEP_STEP_DEG,
                      RADAR_SETTINGS_MAX_SWEEP_STEP_DEG);
        candidate->sweep_draw_interval_ms =
            clamp_int(parse_int_item(interface, "sweepDrawIntervalMs", candidate->sweep_draw_interval_ms),
                      RADAR_SETTINGS_MIN_SWEEP_DRAW_INTERVAL_MS,
                      RADAR_SETTINGS_MAX_SWEEP_DRAW_INTERVAL_MS);
        candidate->show_sweep_trail =
            parse_bool_item(interface, "showSweepTrail", candidate->show_sweep_trail);
        candidate->sweep_trail_count =
            clamp_int(parse_int_item(interface, "sweepTrailCount", candidate->sweep_trail_count),
                      0, RADAR_SETTINGS_MAX_SWEEP_TRAIL_COUNT);
        candidate->sweep_trail_width =
            clamp_int(parse_int_item(interface, "sweepTrailWidth", candidate->sweep_trail_width),
                      1, RADAR_SETTINGS_MAX_LINE_WIDTH);
        candidate->sweep_trail_step_deg =
            parse_double_item(interface, "sweepTrailStepDeg", candidate->sweep_trail_step_deg);
        if (candidate->sweep_trail_step_deg < RADAR_SETTINGS_MIN_SWEEP_TRAIL_STEP_DEG) {
            candidate->sweep_trail_step_deg = RADAR_SETTINGS_MIN_SWEEP_TRAIL_STEP_DEG;
        }
        if (candidate->sweep_trail_step_deg > RADAR_SETTINGS_MAX_SWEEP_TRAIL_STEP_DEG) {
            candidate->sweep_trail_step_deg = RADAR_SETTINGS_MAX_SWEEP_TRAIL_STEP_DEG;
        }
        candidate->show_airports = parse_bool_item(interface, "showAirports", candidate->show_airports);
        candidate->show_countries = parse_bool_item(interface, "showCountries", candidate->show_countries);
        candidate->show_airport_runways = parse_bool_item(interface, "showAirportRunways", candidate->show_airport_runways);
        candidate->emergency_squawks_red = parse_bool_item(interface, "emergencyRed", candidate->emergency_squawks_red);
        candidate->show_aircraft_routes = parse_bool_item(interface, "showAircraftRoutes", candidate->show_aircraft_routes);
        cJSON *route_style = cJSON_GetObjectItemCaseSensitive(interface, "routeStyle");
        if (cJSON_IsString(route_style) && route_style->valuestring) {
            candidate->aircraft_route_style =
                strcmp(route_style->valuestring, "long") == 0 ?
                AIRCRAFT_ROUTE_STYLE_LONG : AIRCRAFT_ROUTE_STYLE_SHORT;
        }
        candidate->show_ground_aircraft = parse_bool_item(interface, "showGroundAircraft", candidate->show_ground_aircraft);
        candidate->ground_speed_kt =
            clamp_int(parse_int_item(interface, "groundSpeedKt", candidate->ground_speed_kt),
                      0, RADAR_SETTINGS_MAX_GROUND_SPEED_KT);
        cJSON *heading_mode = cJSON_GetObjectItemCaseSensitive(interface, "headingMode");
        if (cJSON_IsString(heading_mode) && heading_mode->valuestring) {
            if (strcmp(heading_mode->valuestring, "none") == 0) {
                candidate->show_aircraft_heading = false;
                candidate->aircraft_heading_style = RADAR_HEADING_STYLE_NONE;
                candidate->show_climb_descent = false;
            } else if (strcmp(heading_mode->valuestring, "line") == 0) {
                candidate->show_aircraft_heading = true;
                candidate->aircraft_heading_style = RADAR_HEADING_STYLE_LINE;
            } else if (strcmp(heading_mode->valuestring, "arrow") == 0) {
                candidate->show_aircraft_heading = true;
                candidate->aircraft_heading_style = RADAR_HEADING_STYLE_ARROW;
            } else if (strcmp(heading_mode->valuestring, "vertical") == 0) {
                candidate->show_aircraft_heading = true;
                candidate->aircraft_heading_style = RADAR_HEADING_STYLE_ARROW;
                candidate->show_climb_descent = true;
            } else {
                candidate->show_aircraft_heading = true;
                candidate->aircraft_heading_style = RADAR_HEADING_STYLE_ARROW;
                candidate->show_climb_descent = false;
            }
        } else {
            candidate->show_aircraft_heading = parse_bool_item(interface, "showHeading", candidate->show_aircraft_heading);
            candidate->show_climb_descent = parse_bool_item(interface, "showClimbDescent", candidate->show_climb_descent);
            if (!candidate->show_aircraft_heading) {
                candidate->aircraft_heading_style = RADAR_HEADING_STYLE_NONE;
                candidate->show_climb_descent = false;
            } else if (candidate->aircraft_heading_style == RADAR_HEADING_STYLE_NONE) {
                candidate->aircraft_heading_style = RADAR_HEADING_STYLE_ARROW;
            }
        }
        candidate->show_climb_descent = parse_bool_item(interface, "showClimbDescent", candidate->show_climb_descent);
        if (!candidate->show_aircraft_heading ||
            candidate->aircraft_heading_style == RADAR_HEADING_STYLE_NONE) {
            candidate->show_aircraft_heading = false;
            candidate->aircraft_heading_style = RADAR_HEADING_STYLE_NONE;
            candidate->show_climb_descent = false;
        }
    }

    if (candidate->center_source != RADAR_CENTER_SOURCE_AIRPORT) {
        candidate->show_airport_runways = false;
    }

    parse_color_settings(cJSON_GetObjectItemCaseSensitive(root, "colors"), &candidate->colors);
    parse_altitude_color_settings(cJSON_GetObjectItemCaseSensitive(root, "altitudeColors"),
                                  &candidate->altitude_colors);
    parse_width_settings(cJSON_GetObjectItemCaseSensitive(root, "widths"), &candidate->widths);
    parse_visible_settings(cJSON_GetObjectItemCaseSensitive(root, "visible"), &candidate->visible);
    parse_ui_settings(cJSON_GetObjectItemCaseSensitive(root, "ui"), &candidate->ui);
    parse_label_style_settings(cJSON_GetObjectItemCaseSensitive(root, "labelStyles"),
                               &candidate->label_styles);

    cJSON *range_array = cJSON_GetObjectItemCaseSensitive(root, "ranges");
    if (cJSON_IsArray(range_array)) {
        memset(candidate->ranges, 0, sizeof(candidate->ranges));
        size_t out = 0;
        cJSON *range;
        cJSON_ArrayForEach(range, range_array) {
            if (out >= MAX_RANGE_SETTINGS || !cJSON_IsObject(range)) {
                continue;
            }
            int miles = parse_int_item(range, "miles", 0);
            if (miles <= 0) {
                continue;
            }
            candidate->ranges[out].miles = clamp_int(miles, 1, 250);
            candidate->ranges[out].refresh_sec = clamp_int(parse_int_item(range, "refresh", CONFIG_RADAR_FETCH_INTERVAL_SEC), 2, 300);
            candidate->ranges[out].label_count = clamp_int(parse_int_item(range, "labels", RADAR_SETTINGS_DEFAULT_LABELS),
                                                           0, RADAR_SETTINGS_MAX_LABELS);
            ++out;
        }
    }

    cJSON *notification_array = cJSON_GetObjectItemCaseSensitive(root, "notifications");
    if (cJSON_IsArray(notification_array)) {
        memset(candidate->notifications, 0, sizeof(candidate->notifications));
        for (size_t i = 0; i < MAX_NOTIFICATION_SETTINGS; ++i) {
            candidate->notifications[i].enabled = true;
            candidate->notifications[i].bold_text = false;
            candidate->notifications[i].dim_others = false;
            candidate->notifications[i].color = 0xffb020;
        }
        size_t out = 0;
        cJSON *notification;
        cJSON_ArrayForEach(notification, notification_array) {
            if (out >= MAX_NOTIFICATION_SETTINGS || !cJSON_IsObject(notification)) {
                continue;
            }
            copy_json_string(candidate->notifications[out].type_match,
                             sizeof(candidate->notifications[out].type_match), notification, "type");
            copy_json_string(candidate->notifications[out].text,
                             sizeof(candidate->notifications[out].text), notification, "text");
            candidate->notifications[out].enabled =
                parse_bool_item(notification, "enabled", true);
            candidate->notifications[out].bold_text =
                parse_bool_item(notification, "boldText", false);
            candidate->notifications[out].dim_others =
                parse_bool_item(notification, "dimOthers", false);
            candidate->notifications[out].color =
                parse_color_value(cJSON_GetObjectItemCaseSensitive(notification, "color"), 0xffb020);
            ++out;
        }
    }

    cJSON_Delete(root);

    if (candidate->default_range_mi < 1 || candidate->default_range_mi > 250) {
        candidate->default_range_mi = candidate->ranges[0].miles;
    }

    bool default_in_ranges = false;
    for (size_t i = 0; i < MAX_RANGE_SETTINGS; ++i) {
        if (candidate->ranges[i].miles == candidate->default_range_mi) {
            default_in_ranges = true;
            break;
        }
    }
    if (!default_in_ranges) {
        candidate->default_range_mi = candidate->ranges[0].miles;
    }

    if (candidate->aircraft_data_source != AIRCRAFT_DATA_SOURCE_AIRPLANES_LIVE &&
        candidate->aircraft_data_source != AIRCRAFT_DATA_SOURCE_ADSB_LOL &&
        candidate->aircraft_data_source != AIRCRAFT_DATA_SOURCE_ADSB_FI &&
        candidate->aircraft_data_source != AIRCRAFT_DATA_SOURCE_LOCAL) {
        set_error(error, error_size, "Invalid aircraft data source");
        return false;
    }
    if (!valid_aircraft_url(candidate->aircraft_local_url, sizeof(candidate->aircraft_local_url),
                            candidate->aircraft_data_source == AIRCRAFT_DATA_SOURCE_LOCAL)) {
        set_error(error, error_size,
                  candidate->aircraft_data_source == AIRCRAFT_DATA_SOURCE_LOCAL ?
                  "Local aircraft URL must start with http:// or https://" :
                  "Invalid local aircraft URL");
        return false;
    }

    if (!isValid(*candidate)) {
        set_error(error, error_size, "Invalid settings");
        return false;
    }

    return true;
}

/* Serialise the current settings and WiFi context for the browser UI. */
char *RadarSettings::toJson(const char *wifi_ssid) const
{
    cJSON *root = cJSON_CreateObject();
    cJSON *general = cJSON_AddObjectToObject(root, "general");
    cJSON_AddNumberToObject(general, "lat", center_lat);
    cJSON_AddNumberToObject(general, "lon", center_lon);
    cJSON_AddStringToObject(general, "centerSource",
                            center_source == RADAR_CENTER_SOURCE_GPS ? "gps" :
                            (center_source == RADAR_CENTER_SOURCE_AIRPORT ? "airport" :
                             (center_source == RADAR_CENTER_SOURCE_LOCATION ? "location" : "manual")));
    cJSON *airport = cJSON_AddObjectToObject(general, "airport");
    cJSON_AddStringToObject(airport, "name", center_airport_name);
    cJSON_AddStringToObject(airport, "code", center_airport_code);
    cJSON_AddNumberToObject(airport, "lat", center_airport_lat);
    cJSON_AddNumberToObject(airport, "lon", center_airport_lon);
    cJSON *location = cJSON_AddObjectToObject(general, "location");
    cJSON_AddStringToObject(location, "name", center_location_name);
    cJSON_AddStringToObject(location, "state", center_location_state);
    cJSON_AddStringToObject(location, "country", center_location_country);
    cJSON_AddNumberToObject(location, "lat", center_location_lat);
    cJSON_AddNumberToObject(location, "lon", center_location_lon);
    cJSON_AddNumberToObject(general, "defaultRange", default_range_mi);
    cJSON_AddStringToObject(general, "dataSource",
                            aircraft_data_source == AIRCRAFT_DATA_SOURCE_ADSB_LOL ? "adsb_lol" :
                            (aircraft_data_source == AIRCRAFT_DATA_SOURCE_ADSB_FI ? "adsb_fi" :
                            (aircraft_data_source == AIRCRAFT_DATA_SOURCE_LOCAL ? "local" :
                             "airplanes_live")));
    cJSON_AddStringToObject(general, "localAircraftUrl", aircraft_local_url);

    cJSON *api_keys = cJSON_AddObjectToObject(root, "apiKeys");
    cJSON_AddStringToObject(api_keys, "airportDbToken", airportdb_api_token);
    cJSON_AddStringToObject(api_keys, "openWeatherApiKey", openweather_api_key);

    cJSON *interface = cJSON_AddObjectToObject(root, "interface");
    cJSON_AddBoolToObject(interface, "showSweep", show_sweep);
    cJSON_AddNumberToObject(interface, "sweepStepDeg", sweep_step_deg);
    cJSON_AddNumberToObject(interface, "sweepDrawIntervalMs", sweep_draw_interval_ms);
    cJSON_AddBoolToObject(interface, "showSweepTrail", show_sweep_trail);
    cJSON_AddNumberToObject(interface, "sweepTrailCount", sweep_trail_count);
    cJSON_AddNumberToObject(interface, "sweepTrailWidth", sweep_trail_width);
    cJSON_AddNumberToObject(interface, "sweepTrailStepDeg", sweep_trail_step_deg);
    cJSON_AddBoolToObject(interface, "showAirports", show_airports);
    cJSON_AddBoolToObject(interface, "showCountries", show_countries);
    cJSON_AddBoolToObject(interface, "showAirportRunways", show_airport_runways);
    cJSON_AddBoolToObject(interface, "emergencyRed", emergency_squawks_red);
    cJSON_AddBoolToObject(interface, "showAircraftRoutes", show_aircraft_routes);
    cJSON_AddStringToObject(interface, "routeStyle",
                            aircraft_route_style == AIRCRAFT_ROUTE_STYLE_LONG ? "long" : "short");
    cJSON_AddBoolToObject(interface, "showGroundAircraft", show_ground_aircraft);
    cJSON_AddNumberToObject(interface, "groundSpeedKt", ground_speed_kt);
    cJSON_AddBoolToObject(interface, "showHeading", show_aircraft_heading);
    cJSON_AddBoolToObject(interface, "showClimbDescent", show_climb_descent);
    cJSON_AddStringToObject(interface, "headingMode",
                            !show_aircraft_heading ||
                            aircraft_heading_style == RADAR_HEADING_STYLE_NONE ? "none" :
                            (aircraft_heading_style == RADAR_HEADING_STYLE_LINE ? "line" : "arrow"));

    add_color_settings_json(root, &colors);
    add_altitude_color_settings_json(root, &altitude_colors);
    add_width_settings_json(root, &widths);
    add_visible_settings_json(root, &visible);
    add_ui_settings_json(root, &ui);
    add_label_style_settings_json(root, &label_styles);

    cJSON *range_array = cJSON_AddArrayToObject(root, "ranges");
    for (size_t i = 0; i < MAX_RANGE_SETTINGS; ++i) {
        cJSON *range = cJSON_CreateObject();
        cJSON_AddNumberToObject(range, "miles", ranges[i].miles);
        cJSON_AddNumberToObject(range, "refresh", ranges[i].refresh_sec);
        cJSON_AddNumberToObject(range, "labels", ranges[i].label_count);
        cJSON_AddItemToArray(range_array, range);
    }

    cJSON *notification_array = cJSON_AddArrayToObject(root, "notifications");
    for (size_t i = 0; i < MAX_NOTIFICATION_SETTINGS; ++i) {
        char color[8];
        colorToHex(notifications[i].color, color, sizeof(color));
        cJSON *notification = cJSON_CreateObject();
        cJSON_AddStringToObject(notification, "type", notifications[i].type_match);
        cJSON_AddBoolToObject(notification, "enabled", notifications[i].enabled);
        cJSON_AddBoolToObject(notification, "boldText", notifications[i].bold_text);
        cJSON_AddBoolToObject(notification, "dimOthers", notifications[i].dim_others);
        cJSON_AddStringToObject(notification, "color", color);
        cJSON_AddStringToObject(notification, "text", notifications[i].text);
        cJSON_AddItemToArray(notification_array, notification);
    }

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", wifi_ssid ? wifi_ssid : "");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* Format a packed 0xRRGGBB colour as a CSS-style #rrggbb string. */
void RadarSettings::colorToHex(uint32_t color, char *dst, size_t dst_size)
{
    snprintf(dst, dst_size, "#%06x", (unsigned)(color & 0xffffff));
}
