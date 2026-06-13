#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "zlib.h"

#include "bsp/esp-bsp.h"
#include "AircraftDataService.hpp"
#include "AircraftFetchService.hpp"
#include "AircraftPhotoService.hpp"
#include "AircraftRouteService.hpp"
#include "AirportRunwayService.hpp"
#include "CaptivePortal.hpp"
#include "GpsReceiver.hpp"
#include "RadarGeometry.hpp"
#include "RadarHttpClient.hpp"
#include "RadarPositionProvider.hpp"
#include "RadarSettings.hpp"
#include "RadarTypes.hpp"
#include "SettingsServer.hpp"
#include "AircraftPopupController.hpp"
#include "CurvedButtonController.hpp"
#include "Ssd1306StatusDisplay.hpp"
#include "WifiManager.hpp"

/*
 * Main application coordinator.
 *
 * RadarApp owns the long-lived LVGL objects, shared aircraft buffers, settings,
 * WiFi state, and service instances. The smaller service classes are friends so
 * they can update this embedded application's tightly shared state without a
 * large accessor layer. UI changes still run under the LVGL display lock, and
 * aircraft data crosses from the fetch task to the UI through generation-counted
 * snapshots.
 */
class RadarApp {
public:
    /* Construct the application shell; real initialisation happens in run(). */
    RadarApp() = default;

    /*
     * Initialise NVS, display, WiFi, GPS, settings, and worker tasks.
     *
     * This method does not return under normal operation.
     */
    void run();

private:
    friend class SettingsServer;
    friend class CaptivePortal;
    friend class WifiManager;
    friend class AircraftFetchService;
    friend class AircraftPhotoService;
    friend class AircraftRouteService;
    friend class AircraftPopupController;
    friend class CurvedButtonController;

    static RadarApp *active_app;
    static constexpr size_t DATA_MENU_ROW_COUNT = 8;
    static constexpr size_t SWEEP_TRAIL_COUNT = 50;

    /* Cached weather for one visible airport. */
    typedef struct {
        size_t airport_index;
        int weather_code;
        int cloud_cover;
        float precipitation_mm;
        float rain_mm;
        int64_t fetched_ms;
        bool valid;
    } airport_weather_t;

    /* Main radar canvases and root UI objects. */
    lv_obj_t *radar_canvas = nullptr;
    lv_obj_t *aircraft_canvas = nullptr;
    lv_obj_t *screen_root = nullptr;
    void *radar_canvas_buf = nullptr;
    void *aircraft_canvas_buf = nullptr;
    lv_obj_t *sweep_overlay = nullptr;
    int sweep_overlay_left = 0;
    int sweep_overlay_top = 0;

    /* Curved control buttons and their popup menus. */
    curved_button_t range_button = {};
    lv_obj_t *range_button_left_hitbox = nullptr;
    lv_obj_t *range_button_middle_hitbox = nullptr;
    lv_obj_t *range_button_right_hitbox = nullptr;
    lv_obj_t *range_menu = nullptr;
    lv_obj_t *range_menu_rows[MAX_RANGE_SETTINGS] = {};
    lv_obj_t *range_menu_labels[MAX_RANGE_SETTINGS] = {};
    curved_button_t status_button = {};
    curved_button_t wifi_button = {};
    lv_obj_t *wifi_menu = nullptr;
    lv_obj_t *wifi_menu_rows[4] = {};
    lv_obj_t *wifi_menu_ip_label = nullptr;
    lv_obj_t *wifi_menu_change_label = nullptr;
    lv_obj_t *wifi_menu_reboot_label = nullptr;
    lv_obj_t *wifi_menu_clear_nvs_label = nullptr;
    lv_obj_t *data_menu = nullptr;
    lv_obj_t *data_menu_rows[DATA_MENU_ROW_COUNT] = {};
    lv_obj_t *data_menu_labels[DATA_MENU_ROW_COUNT] = {};
    lv_obj_t *hardware_menu = nullptr;
    lv_obj_t *hardware_menu_title = nullptr;
    lv_obj_t *hardware_menu_rows[10] = {};
    lv_obj_t *hardware_menu_labels[10] = {};
    lv_obj_t *hardware_menu_values[10] = {};
    lv_obj_t *hardware_menu_help = nullptr;

    /* Captive portal overlay shown on the device while WiFi setup is active. */
    lv_obj_t *portal_overlay = nullptr;
    lv_obj_t *portal_ap_label = nullptr;
    lv_obj_t *portal_status_label = nullptr;

    /* Aircraft detail popup and photo widgets. */
    lv_obj_t *aircraft_popup = nullptr;
    lv_obj_t *aircraft_popup_title = nullptr;
    lv_obj_t *aircraft_popup_body = nullptr;
    lv_obj_t *aircraft_photo_image = nullptr;
    lv_obj_t *aircraft_photo_status = nullptr;
    char aircraft_popup_base_body[320] = {};

    /* Static and dynamic labels placed around the radar. */
    lv_obj_t *notification_labels[MAX_NOTIFICATION_SETTINGS] = {};
    lv_obj_t *gps_status_label = nullptr;
    lv_obj_t *heading_labels[HEADING_LABEL_COUNT] = {};
    lv_obj_t *range_ring_labels[RANGE_RING_COUNT] = {};
    lv_obj_t *airport_labels[MAX_AIRPORT_LABELS] = {};

    /* Pre-allocated drawing state, kept stable to avoid heap churn during sweep updates. */
    radar_marker_t markers[MAX_AIRCRAFT_LABELS] = {};
    aircraft_track_history_t *aircraft_tracks = nullptr;
    lv_image_dsc_t aircraft_photo_dsc = {};

    /* Cross-task coordination and aircraft snapshots. */
    SemaphoreHandle_t aircraft_mutex = nullptr;
    SemaphoreHandle_t wifi_mutex = nullptr;
    SemaphoreHandle_t airport_weather_mutex = nullptr;
    EventGroupHandle_t wifi_event_group = nullptr;
    aircraft_data_t *latest_aircraft = nullptr;
    aircraft_data_t *ui_aircraft_snapshot = nullptr;
    size_t latest_aircraft_count = 0;
    size_t ui_aircraft_count = 0;
    uint32_t latest_aircraft_generation = 0;
    int ui_aircraft_range_mi = 50;
    char latest_status[32] = "STARTING";
    char portal_status[80] = "Starting setup network";
    char oled_wifi_line[32] = "WIFI --";

    /* WiFi credentials and AP identity. Password is kept only in RAM after loading. */
    char wifi_ssid[33] = {};
    char wifi_password[65] = {};
    char setup_ap_ssid[33] = "RadarSetup";

    /* Pixel data owned by the aircraft popup while a photo is visible. */
    uint8_t *aircraft_photo_pixels = nullptr;
    size_t aircraft_photo_pixel_size = 0;

    /* Runtime UI state. */
    int sweep_angle_deg = 0;
    size_t range_index = 2;
    void *rotary_knob_handle = nullptr;
    volatile int encoder_pending_delta = 0;
    int encoder_event_accum = 0;
    int confirm_last_level = 1;
    int back_last_level = 1;
    int push_last_level = 1;
    int64_t confirm_last_change_us = 0;
    int64_t back_last_change_us = 0;
    int64_t push_last_change_us = 0;
    int hardware_menu_selected = 0;
    int64_t hardware_menu_last_use_us = 0;
    aircraft_filter_t aircraft_filter = AIRCRAFT_FILTER_ALL;
    uint32_t aircraft_photo_request_id = 0;
    uint32_t aircraft_route_request_id = 0;
    int wifi_retry_count = 0;
    airport_weather_t airport_weather[MAX_AIRPORT_WEATHER] = {};
    size_t airport_weather_count = 0;
    int64_t airport_weather_last_fetch_ms = 0;

    /* Volatile flags touched by worker tasks or event callbacks. */
    volatile bool aircraft_photo_fetch_running = false;
    volatile bool aircraft_route_fetch_running = false;
    volatile bool wifi_recovering = false;
    volatile bool wifi_started = false;
    volatile bool wifi_portal_active = false;
    volatile bool portal_dns_running = false;

    /* Network server handles. */
    bool settings_http_active = false;
    httpd_handle_t portal_httpd = nullptr;
    TaskHandle_t portal_dns_task_handle = nullptr;
    esp_netif_t *wifi_sta_netif = nullptr;
    esp_netif_t *wifi_ap_netif = nullptr;

    /* Generation counters decide when the static canvas or aircraft overlay must redraw. */
    uint32_t displayed_generation = UINT32_MAX;
    int displayed_range_mi = -1;
    uint32_t displayed_settings_generation = UINT32_MAX;
    uint32_t settings_generation = 0;

    /* Owned services. They are bound back to this RadarApp during start-up. */
    RadarSettings settings = {};
    GpsReceiver gps = {};
    RadarHttpClient http_client = {};
    AircraftDataService aircraft_service = {};
    AircraftFetchService fetch_service = {};
    AircraftPhotoService photo_service = {};
    AircraftRouteService route_service = {};
    AirportRunwayService runway_service = {};
    SettingsServer settings_server = {};
    AircraftPopupController popup_controller = {};
    CurvedButtonController curved_button_controller = {};
    CaptivePortal captive_portal = {};
    WifiManager wifi_manager = {};
    Ssd1306StatusDisplay oled_status = {};

    /* Forward the range button LVGL event to the active app instance. */
    static void range_button_event_entry(lv_event_t *event);

    /* Forward the DATA button LVGL event to the active app instance. */
    static void status_button_event_entry(lv_event_t *event);

    /* Forward a DATA menu row click to the active app instance. */
    static void data_menu_event_entry(lv_event_t *event);

    /* Forward the WiFi button LVGL event to the active app instance. */
    static void wifi_button_event_entry(lv_event_t *event);

    /* Forward the WiFi menu IP row click to the active app instance. */
    static void wifi_menu_ip_event_entry(lv_event_t *event);

    /* Forward the WiFi menu setup row click to the active app instance. */
    static void wifi_menu_setup_event_entry(lv_event_t *event);

    /* Forward the WiFi menu reboot row click to the active app instance. */
    static void wifi_menu_reboot_event_entry(lv_event_t *event);

    /* Forward the WiFi menu clear-NVS row click to the active app instance. */
    static void wifi_menu_clear_nvs_event_entry(lv_event_t *event);

    /* Forward radar touch events from the transparent hit layer. */
    static void radar_touch_event_entry(lv_event_t *event);

    /* Forward the fast sweep timer callback. */
    static void sweep_timer_entry(lv_timer_t *timer);

    /* Draw the custom sweep overlay in one LVGL draw event. */
    static void sweep_draw_event_entry(lv_event_t *event);

    /* Forward the slower aircraft/UI refresh timer callback. */
    static void update_aircraft_ui_entry(lv_timer_t *timer);

    /* Poll the physical rotary encoder from the LVGL timer thread. */
    static void rotary_timer_entry(lv_timer_t *timer);

    /* Queue left/right events from the Espressif knob driver. */
    static void knob_left_entry(void *knob, void *user_data);
    static void knob_right_entry(void *knob, void *user_data);

    /* FreeRTOS entry point for the aircraft photo worker task. */
    static void aircraft_photo_fetch_task_entry(void *arg);

    /* FreeRTOS entry point for the aircraft route worker task. */
    static void aircraft_route_fetch_task_entry(void *arg);

    /* Forward the captive portal page request to the active app instance. */
    static esp_err_t portal_get_handler_entry(httpd_req_t *req);

    /* Forward the captive portal form submit to the active app instance. */
    static esp_err_t portal_save_handler_entry(httpd_req_t *req);

    /* Forward the settings page request to the active app instance. */
    static esp_err_t settings_page_handler_entry(httpd_req_t *req);

    /* Forward the settings JSON GET request to the active app instance. */
    static esp_err_t settings_api_get_handler_entry(httpd_req_t *req);

    /* Forward the settings JSON save request to the active app instance. */
    static esp_err_t settings_api_save_handler_entry(httpd_req_t *req);

    /* Forward the diagnostics/status request to the active app instance. */
    static esp_err_t settings_status_handler_entry(httpd_req_t *req);

    /* Forward the LVGL screenshot request to the active app instance. */
    static esp_err_t settings_screenshot_handler_entry(httpd_req_t *req);

    /* Forward the default settings request to the active app instance. */
    static esp_err_t settings_defaults_handler_entry(httpd_req_t *req);

    /* Forward the range reset request to the active app instance. */
    static esp_err_t settings_ranges_reset_handler_entry(httpd_req_t *req);

    /* Forward the airport search request to the active app instance. */
    static esp_err_t settings_airport_search_handler_entry(httpd_req_t *req);

    /* Forward the OpenWeather location search request to the active app instance. */
    static esp_err_t settings_location_search_handler_entry(httpd_req_t *req);

    /* Forward the settings WiFi scan request to the active app instance. */
    static esp_err_t settings_wifi_scan_handler_entry(httpd_req_t *req);

    /* Forward the settings WiFi save request to the active app instance. */
    static esp_err_t settings_wifi_save_handler_entry(httpd_req_t *req);

    /* Forward a range menu row event to the active app instance. */
    static void range_menu_event_entry(lv_event_t *event);

    /* FreeRTOS entry point for the captive portal DNS responder. */
    static void dns_server_task_entry(void *arg);

    /* ESP event-loop entry point for WiFi and IP events. */
    static void wifi_event_handler_entry(void *arg, esp_event_base_t event_base,
                                         int32_t event_id, void *event_data);

    /* FreeRTOS entry point for the aircraft fetch loop. */
    static void aircraft_fetch_task_entry(void *arg);

    /* FreeRTOS entry point for the airport weather refresh loop. */
    static void airport_weather_task_entry(void *arg);

    /* Forward the popup close button event to the active app instance. */
    static void aircraft_popup_close_event_entry(lv_event_t *event);

    /* Allocate a PSRAM-first aircraft snapshot buffer. */
    static aircraft_data_t *alloc_aircraft_buffer();

    /* Allocate and bind the shared aircraft storage used by fetch and UI tasks. */
    void init_aircraft_storage();

    /* Return the currently selected range in statute miles. */
    int get_current_range_mi() const;

    /* Return the current range's aircraft refresh interval in milliseconds. */
    int get_current_refresh_interval_ms() const;

    /* Return the current range's aircraft label limit. */
    size_t get_current_label_limit() const;

    /* Return the number of configured user ranges. */
    size_t get_range_count() const;

    /* Select the configured startup/default range. */
    void set_range_to_default();

    /* Configure physical buttons and the trim rotary encoder GPIOs. */
    void init_rotary_inputs();

    /* Return the smallest signed difference between two headings. */
    static int angle_delta(int a, int b);

    /* Convert degrees to radians. */
    static double deg_to_rad(double degrees);

    /* Convert radians to degrees. */
    static double rad_to_deg(double radians);

    /* Calculate great-circle distance in statute miles. */
    static double distance_miles(double lat1, double lon1, double lat2, double lon2);

    /* Calculate initial bearing in degrees clockwise from north. */
    static int bearing_degrees(double lat1, double lon1, double lat2, double lon2);

    /* Resolve a configured radar label font. */
    static const lv_font_t *configured_label_font(const label_font_setting_t &style, int size_delta = 0);

    /* Apply a configured label font to an existing object. */
    static void apply_label_font(lv_obj_t *obj, const label_font_setting_t &style, int size_delta = 0);

    /* Resolve the configured aircraft label font for callsign or detail text. */
    const lv_font_t *aircraft_label_font(bool detail, bool emphasised) const;

    /* Reapply settings-driven fonts to all static radar labels. */
    void refresh_static_ui_fonts();

    /* Update the short DATA button status text from any task. */
    void set_data_status(const char *fmt, ...);

    /* Update the captive portal overlay status text from any task. */
    void set_portal_status(const char *fmt, ...);

    /* Resolve the active radar centre from settings and GPS state. */
    bool get_radar_center(double *lat, double *lon, bool *using_gps = nullptr) const;

    /* Apply a font to an LVGL object without repeating style boilerplate. */
    static void set_obj_font(lv_obj_t *obj, const lv_font_t *font);

    /* Create a label with the app's common transparent, non-scrollable styling. */
    static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color);

    /* Normalise an angle into the 0..359 degree range. */
    static int normalize_degrees(int degrees);

    /* Reposition all character labels that form a curved button caption. */
    static void position_curved_button_text(curved_button_t *button);

    /* Set the visible caption on a curved button. */
    static void set_curved_button_text(curved_button_t *button, const char *text);

    /* Create a curved visual button and its rectangular touch hitbox. */
    void make_curved_button(lv_obj_t *parent, curved_button_t *button, int start_deg, int end_deg,
                            int x, int y, int w, int h, const char *text, lv_event_cb_t cb,
                            uint32_t text_color, bool top_text = false);

    /* Recalculate the text beside each range ring after the range changes. */
    void update_range_ring_labels();

    /* Refresh the range button caption. */
    void refresh_range_label();

    /* Change the selected range by one or more preset slots. */
    void change_range_by_delta(int delta);

    /* Create one selectable row in the range popup menu. */
    lv_obj_t *create_range_menu_row(lv_obj_t *parent, int y, size_t index, lv_obj_t **label_out);

    /* Create the range popup menu and its rows. */
    void create_range_menu(lv_obj_t *screen);

    /* Refresh range popup labels from configured range presets. */
    void refresh_range_menu();

    /* Show the range selection popup menu. */
    void show_range_menu();

    /* Hide the range selection popup menu. */
    void hide_range_menu();

    /* Toggle the range selection popup menu. */
    void toggle_range_menu();

    /* Format the current station IP for the WiFi menu. */
    void format_wifi_ip_text(char *dst, size_t dst_size);

    /* Mirror a short network status message onto the optional SSD1306 display. */
    void update_oled_status(const char *status);

    /* Mirror the active station or setup AP IP address onto the optional SSD1306 display. */
    void update_oled_ip(const char *ip, bool setup_ap);

    /* Redraw the optional SSD1306 with WiFi and radar status lines. */
    void refresh_oled_dashboard();

    /* Create one row in the WiFi popup menu. */
    lv_obj_t *create_wifi_menu_row(lv_obj_t *parent, int y, const char *text,
                                   lv_event_cb_t cb, lv_obj_t **label_out);

    /* Create the WiFi popup menu and its rows. */
    void create_wifi_menu(lv_obj_t *screen);

    /* Refresh WiFi popup labels from the current connection state. */
    void refresh_wifi_menu();

    /* Show the WiFi popup menu. */
    void show_wifi_menu();

    /* Hide the WiFi popup menu. */
    void hide_wifi_menu();

    /* Toggle the WiFi popup menu. */
    void toggle_wifi_menu();

    /* Create one row in the DATA popup menu. */
    lv_obj_t *create_data_menu_row(lv_obj_t *parent, int y, const char *text,
                                   intptr_t action, lv_obj_t **label_out);

    /* Create the DATA popup menu and its rows. */
    void create_data_menu(lv_obj_t *screen);

    /* Refresh DATA menu labels from filter and visibility settings. */
    void refresh_data_menu();

    /* Show the DATA popup menu. */
    void show_data_menu();

    /* Hide the DATA popup menu. */
    void hide_data_menu();

    /* Toggle the DATA popup menu. */
    void toggle_data_menu();

    /* Create the hardware-control menu overlay. */
    void create_hardware_menu(lv_obj_t *screen);

    /* Refresh the hardware menu row text and selection state. */
    void refresh_hardware_menu();

    /* Show, hide, or move within the hardware menu. */
    void show_hardware_menu();
    void hide_hardware_menu();
    void move_hardware_menu_selection(int delta);

    /* Execute the selected hardware menu row. */
    void select_hardware_menu_item();

    /* Run one configured hardware button action. */
    void apply_hardware_button_action(int action);

    /* Check one active-low button for a debounced press. */
    bool consume_button_press(int gpio, int *last_level, int64_t *last_change_us);

    /* Handle left, middle, and right range button taps. */
    void range_button_event(lv_event_t *event);

    /* Poll the physical rotary encoder and apply completed detents. */
    void rotary_timer_cb();

    /* Handle a range popup row tap. */
    void range_menu_event(lv_event_t *event);

    /* Handle the DATA button tap. */
    void status_button_event(lv_event_t *event);

    /* Handle a DATA menu row action. */
    void data_menu_event(lv_event_t *event);

    /* Handle the WiFi button tap. */
    void wifi_button_event(lv_event_t *event);

    /* Handle the WiFi menu IP row tap. */
    void wifi_menu_ip_event(lv_event_t *event);

    /* Handle the WiFi menu setup row tap. */
    void wifi_menu_setup_event(lv_event_t *event);

    /* Handle the WiFi menu reboot row tap. */
    void wifi_menu_reboot_event(lv_event_t *event);

    /* Handle the WiFi menu clear-NVS row tap. */
    void wifi_menu_clear_nvs_event(lv_event_t *event);

    /* Choose the marker colour for a displayed aircraft. */
    uint32_t marker_color(const aircraft_data_t *aircraft, bool hit, bool dimmed = false) const;

    /* Return the first configured notification that matches an aircraft type. */
    const notification_setting_t *matching_notification(const aircraft_data_t *aircraft) const;

    /* Return whether an aircraft matches a notification that focuses the radar. */
    bool matches_focus_notification(const aircraft_data_t *aircraft) const;

    /* Return whether any visible aircraft has requested notification focus. */
    bool notification_focus_active(const aircraft_data_t *snapshot, size_t count, int range_mi) const;

    /* Check whether an aircraft passes the current DATA menu filter. */
    bool aircraft_matches_filter(const aircraft_data_t *aircraft) const;

    /* Force the next UI refresh to redraw the aircraft overlay. */
    void invalidate_aircraft_display();

    /* Clamp an integer value to an inclusive range. */
    static int clamp_int(int value, int min_value, int max_value);

    /* Darken a plotted aircraft colour when notification focus is active. */
    static uint32_t dim_colour(uint32_t color);

    /* Return a stable key used for aircraft history tracking. */
    static const char *aircraft_track_key(const aircraft_data_t *aircraft);

    /* Find or create the history cache slot for one aircraft. */
    aircraft_track_history_t *track_history_for_aircraft(const aircraft_data_t *aircraft);

    /* Update the history cache after a fresh aircraft snapshot is displayed. */
    void update_aircraft_track_history(const aircraft_data_t *snapshot, size_t count,
                                       int range_mi, uint32_t generation);

    /* Project an aircraft's bearing and distance into radar canvas coordinates. */
    static bool project_aircraft_to_radar(const aircraft_data_t *aircraft, int range_mi, int *x, int *y);

    /* Find the aircraft nearest to a touch point. */
    int find_aircraft_at_point(int x, int y);

    /* Return "-" for empty popup text fields. */
    static const char *popup_value_or_dash(const char *value);

    /* Format an altitude for popup display. */
    static void format_altitude(char *dst, size_t dst_size, int altitude_ft);

    /* Set the popup photo status label. */
    void set_aircraft_photo_status_text(const char *text);

    /* Release the popup's current photo buffer and descriptor. */
    void release_aircraft_photo_image();

    /* Hide the aircraft popup and cancel outstanding photo work. */
    void hide_aircraft_popup();

    /* Show the aircraft popup for one aircraft. */
    void show_aircraft_popup(const aircraft_data_t *aircraft, int screen_x, int screen_y);

    /* Handle touches on the radar hit layer. */
    void radar_touch_event(lv_event_t *event);

    /* Create the transparent radar touch layer above the canvases. */
    void create_radar_touch_layer(lv_obj_t *radar);

    /* Highlight aircraft that are under the sweep line or selected. */
    void update_target_highlights();

    /* Advance and redraw the sweep line. */
    void sweep_timer_cb(lv_timer_t *timer);

    /* Set one canvas pixel if it lies inside the canvas bounds. */
    static void canvas_set_px_safe(lv_obj_t *canvas, int x, int y, uint32_t color);

    /* Mix two 8-bit colour channels by amount, where 0 keeps a and 1 keeps b. */
    static uint8_t mix_channel(uint8_t a, uint8_t b, float amount);

    /* Mix two 0xRRGGBB colours by amount, where 0 keeps from and 1 keeps to. */
    static uint32_t mix_color_hex(uint32_t from, uint32_t to, float amount);

    /* Calculate soft alpha for a ring edge at a given distance from centre. */
    static float ring_alpha(float distance, float radius, float width, float feather);

    /* Test whether an angle lies inside a clockwise arc that may wrap past 360. */
    static bool angle_in_range(float degrees, int start_deg, int end_deg);

    /* Convert a polar arc endpoint into canvas-relative x/y coordinates. */
    static void arc_endpoint(int degrees, int radius, float *x, float *y);

    /* Expand integer drawing bounds to include a point plus margin. */
    static void expand_bounds(float x, float y, float margin, int *min_x, int *min_y, int *max_x, int *max_y);

    /* Draw a thick curved band between two bearings. */
    static void draw_control_arc_band(lv_obj_t *canvas, int start_deg, int end_deg, int radius,
                                      float width, uint32_t color);

    /* Draw one configured control-arc segment. */
    void draw_control_arc_segment(lv_obj_t *canvas, int start_deg, int end_deg);

    /* Draw all static control arcs around the lower bezel. */
    void draw_control_arcs(lv_obj_t *canvas);

    /* Draw a square brush centred at one pixel. */
    static void canvas_draw_brush(lv_obj_t *canvas, int x, int y, int width, uint32_t color);

    /* Draw a clipped line with a square brush. */
    static void canvas_draw_line(lv_obj_t *canvas, int x0, int y0, int x1, int y1, int width, uint32_t color);

    /* Draw a square brush with alpha. */
    static void canvas_draw_brush_opa(lv_obj_t *canvas, int x, int y, int width, uint32_t color, lv_opa_t opa);

    /* Draw a clipped line with alpha. */
    static void canvas_draw_line_opa(lv_obj_t *canvas, int x0, int y0, int x1, int y1,
                                     int width, uint32_t color, lv_opa_t opa);

    /* Draw a radial line from r0 to r1 at the supplied bearing. */
    static void canvas_draw_radial_line(lv_obj_t *canvas, int degrees, int r0, int r1, int width, uint32_t color);

    /* Draw the square background grid clipped to the round radar face. */
    void draw_square_grid(lv_obj_t *canvas);

    /* Draw compass tick marks and radial bearing lines. */
    void draw_compass_scale(lv_obj_t *canvas);

    /* Draw the static radar background canvas. */
    void draw_radar_canvas(lv_obj_t *canvas);

    /* Allocate and initialise the static radar background canvas. */
    lv_obj_t *create_radar_canvas(lv_obj_t *parent);

    /* Allocate and initialise the transparent aircraft overlay canvas. */
    lv_obj_t *create_aircraft_canvas(lv_obj_t *parent);

    /* Create compass heading labels around the radar. */
    void create_heading_labels(lv_obj_t *radar);

    /* Create range-ring labels that are updated when range changes. */
    void create_range_ring_labels(lv_obj_t *radar);

    /* Create the fixed pool of aircraft label widgets. */
    void create_aircraft_markers(lv_obj_t *radar);

    /* Create the fixed pool of airport label widgets. */
    void create_airport_labels(lv_obj_t *radar);

    /* Draw an aircraft dot marker on the overlay canvas. */
    static void draw_aircraft_dot(lv_obj_t *canvas, int x, int y, uint32_t color);

    /* Draw an ATC-style square head symbol. */
    static void draw_aircraft_head_symbol(lv_obj_t *canvas, int x, int y, uint32_t color);

    /* Draw one small aircraft history point. */
    static void draw_aircraft_history_dot(lv_obj_t *canvas, int x, int y, uint32_t color, lv_opa_t opa);

    /* Draw a short heading indicator without implying future position. */
    static void draw_aircraft_heading_indicator(lv_obj_t *canvas, int x, int y, int heading_deg,
                                                uint32_t color, int width, bool arrow_head);

    /* Set one canvas pixel with alpha if it lies inside the canvas bounds. */
    static void canvas_set_px_opa_safe(lv_obj_t *canvas, int x, int y, uint32_t color, lv_opa_t opa);

    /* Draw a subtle airport cross marker. */
    void draw_airport_marker(lv_obj_t *canvas, int x, int y);

    /* Draw a compact weather icon beside an airport marker. */
    void draw_airport_weather_icon(lv_obj_t *canvas, int x, int y, int weather_code);

    /* Read cached weather for one airport record index. */
    bool get_airport_weather(size_t airport_index, airport_weather_t *weather);

    /* Fetch and publish current weather for airports visible at the active range. */
    void refresh_airport_weather();

    /* Draw country boundary segments that intersect the current radar range. */
    size_t draw_country_boundaries(lv_obj_t *canvas, int range_mi);

    /* Draw airport markers that lie inside the current radar range. */
    size_t draw_airports(lv_obj_t *canvas, int range_mi);

    /* Position and show airport ICAO labels for visible airports. */
    void update_airport_labels(int range_mi);

    /* Redraw aircraft dots and headings on the fast-changing overlay. */
    void update_aircraft_plot(const aircraft_data_t *snapshot, size_t count, int range_mi);

    /* Position and update visible aircraft text labels. */
    void update_aircraft_labels(const aircraft_data_t *snapshot, size_t count, int range_mi);

    /* Update the top notification banner from matching aircraft. */
    void update_notification_banner(const aircraft_data_t *snapshot, size_t count, int range_mi);

    /* Refresh the small GPS status label on the radar. */
    void update_gps_status_label();

    /* Apply a text colour to a curved button. */
    static void set_curved_button_color(curved_button_t *button, uint32_t color);

    /* Reapply settings-driven colours and redraw the static canvas if needed. */
    void refresh_static_ui_colors();

    /* Show, hide, and refresh the on-device captive portal instructions. */
    void update_portal_overlay();

    /* Copy aircraft data from the fetch task and refresh all dynamic radar UI. */
    void update_aircraft_ui(lv_timer_t *timer);

    /* Install the PSRAM-first cJSON allocator hooks. */
    static void init_json_allocator();

    /* Return whether two strings match after trimming, ignoring case. */
    static bool string_equals_trimmed_ci(const char *left, const char *right);

    /* Convert a display range in statute miles to the API's nautical-mile request range. */
    static int miles_to_nautical_request(int range_mi);

    /* Build the airplanes.live point query URL for the current radar centre. */
    void build_aircraft_url(char *url, size_t url_size, int range_mi);

    /* Read a required numeric JSON field. */
    static bool json_get_number(cJSON *object, const char *name, double *value);

    /* Read a numeric JSON field that may be stored as a number or string. */
    static bool json_get_double(cJSON *object, const char *name, double *value);

    /* Copy a JSON string field after trimming surrounding whitespace. */
    static void copy_trimmed_field(char *dst, size_t dst_size, cJSON *object, const char *name);

    /* Normalise an ICAO airport code to its four-character uppercase form. */
    static bool normalize_icao_code(char *dst, size_t dst_size, const char *src);

    /* Publish freshly fetched runway geometry to the active cache. */
    void publish_runway_cache(const airport_runway_cache_t *cache);

    /* Copy the active runway cache if it is valid for the current centre. */
    bool get_active_runway_cache(airport_runway_cache_t *cache);

    /* Clear any active runway geometry after centre or settings changes. */
    void clear_active_runway_cache();

    /* Start a runway fetch when the airport-centred radar needs one. */
    void ensure_airport_runways_cached();

    /* Draw cached runway centre lines that fit within the current range. */
    size_t draw_airport_runways(lv_obj_t *canvas, int range_mi);

    /* Parse an aircraft altitude field, including the special ground value. */
    static int parse_altitude_ft(cJSON *aircraft);

    /* Sort aircraft by distance from the radar centre. */
    static int aircraft_compare_distance(const void *left, const void *right);

    /* Publish parsed aircraft data for the UI task to consume. */
    void publish_aircraft_data(aircraft_data_t *items, size_t count, int total);

    /* Parse an airplanes.live JSON payload into display-ready aircraft records. */
    bool parse_aircraft_json(const char *json, int json_len, int range_mi);

    /* Create a configured HTTP client for aircraft JSON requests. */
    static esp_http_client_handle_t create_aircraft_http_client();

    /* Fetch the current aircraft JSON payload through an existing HTTP client. */
    esp_err_t fetch_aircraft_json(esp_http_client_handle_t client, char **response_out,
                                  int *response_len_out, int range_mi);

    /* Build the Origin/Referer base used for photo API requests. */
    void build_photo_origin(char *origin, size_t origin_size);

    /* Apply browser-like headers required by the aircraft photo API. */
    void set_photo_http_headers(esp_http_client_handle_t client, const char *accept);

    /* Fetch a bounded HTTP response into a heap buffer. */
    esp_err_t fetch_http_buffer(const char *url, const char *accept, size_t initial_capacity,
                                size_t max_bytes, char **response_out, int *response_len_out,
                                int mutex_timeout_ms = HTTP_BUFFER_TLS_MUTEX_TIMEOUT_MS,
                                int http_timeout_ms = PHOTO_HTTP_TIMEOUT_MS);

    /* Extract the first thumbnail URL from a PlaneSpotters metadata response. */
    static bool parse_photo_thumbnail_url(const char *json, int json_len, char *url, size_t url_size);

    /* Normalise an aircraft ICAO hex string for photo lookup. */
    static bool normalize_icao_hex(char *dst, size_t dst_size, const char *src);

    /* Check that an asynchronous photo request still matches the open popup. */
    bool photo_request_is_current(uint32_t request_id);

    /* Update popup photo status text from the photo worker task. */
    void update_aircraft_photo_status_from_task(uint32_t request_id, const char *status);

    /* Install a decoded popup photo from the photo worker task. */
    void install_aircraft_photo_from_task(uint32_t request_id, const lv_image_dsc_t *decoded_image,
                                          uint8_t *pixels, size_t pixels_size);

    /* Worker task that fetches metadata, downloads, and decodes an aircraft photo. */
    void aircraft_photo_fetch_task(void *arg);

    /* Start a photo fetch for the supplied aircraft ICAO hex value. */
    void start_aircraft_photo_fetch(const char *icao);

    /* Check that an asynchronous route result still matches the open popup. */
    bool route_request_is_current(uint32_t request_id);

    /* Update popup route text from the route worker task. */
    void update_aircraft_route_from_task(uint32_t request_id, const char *route_text);

    /* Worker task that fetches route details for the popup. */
    void aircraft_route_fetch_task(void *arg);

    /* Start a route fetch for the supplied flight callsign. */
    void start_aircraft_route_fetch(const char *callsign);

    /* Ask the WiFi manager task to start the captive portal. */
    void request_wifi_portal();

    /* Consume and clear a pending captive portal request bit. */
    bool consume_wifi_portal_request();

    /* Create the setup access-point SSID from the device identity. */
    void init_setup_ap_ssid();

    /* Load saved station WiFi credentials from NVS. */
    bool load_wifi_credentials();

    /* Store station WiFi credentials in NVS. */
    esp_err_t save_wifi_credentials(const char *ssid, const char *password);

    /* Apply validated settings to the live application state. */
    void apply_settings(const radar_settings_t *candidate);

    /* Read a small HTTP request body into a heap buffer. */
    static char *read_request_body(httpd_req_t *req, size_t max_len);

    /* Send the current settings as JSON. */
    esp_err_t send_settings_json(httpd_req_t *req);

    /* Send factory-default settings as JSON. */
    esp_err_t send_defaults_json(httpd_req_t *req);

    /* Send a compact JSON status response. */
    esp_err_t send_json_status(httpd_req_t *req, bool ok, const char *message);

    /* Serve the browser settings page. */
    esp_err_t settings_page_handler(httpd_req_t *req);

    /* Handle a settings API read request. */
    esp_err_t settings_api_get_handler(httpd_req_t *req);

    /* Handle a settings API save request. */
    esp_err_t settings_api_save_handler(httpd_req_t *req);

    /* Handle a live device-status API request. */
    esp_err_t settings_status_handler(httpd_req_t *req);

    /* Capture the current LVGL display and return it as a BMP image. */
    esp_err_t settings_screenshot_handler(httpd_req_t *req);

    /* Handle a factory-default settings API request. */
    esp_err_t settings_defaults_handler(httpd_req_t *req);

    /* Reset the configured range presets to defaults. */
    esp_err_t settings_ranges_reset_handler(httpd_req_t *req);

    /* Search the generated airport list for settings autocomplete. */
    esp_err_t settings_airport_search_handler(httpd_req_t *req);

    /* Search OpenWeather geocoding results for location-centred radar setup. */
    esp_err_t settings_location_search_handler(httpd_req_t *req);

    /* Scan nearby WiFi networks for the settings page. */
    esp_err_t settings_wifi_scan_handler(httpd_req_t *req);

    /* Save station WiFi credentials from the settings page. */
    esp_err_t settings_wifi_save_handler(httpd_req_t *req);

    /* Start the HTTP server that hosts the settings application. */
    bool start_settings_http_server();

    /* Stop the settings HTTP server if it is running. */
    void stop_settings_http_server();

    /* Convert a URL-encoded hexadecimal digit. */
    static int hex_value(char ch);

    /* Decode a URL-encoded string into a fixed buffer. */
    static void url_decode(char *dst, size_t dst_size, const char *src, size_t src_len);

    /* Extract and decode one field from a form-encoded body. */
    static bool form_get_value(const char *body, const char *key, char *out, size_t out_size);

    /* Send text escaped for safe inclusion in a simple HTML response. */
    static void http_send_escaped(httpd_req_t *req, const char *text);

    /* Run a blocking WiFi scan and return the number of visible access points. */
    uint16_t scan_wifi_networks(wifi_ap_record_t *aps, uint16_t max_count);

    /* Serve the captive portal WiFi selection page. */
    esp_err_t portal_get_handler(httpd_req_t *req);

    /* Save credentials submitted through the captive portal. */
    esp_err_t portal_save_handler(httpd_req_t *req);

    /* Start the captive portal HTTP server. */
    bool start_portal_http_server();

    /* DNS task that redirects all captive portal lookups to the device. */
    void dns_server_task(void *arg);

    /* Start the captive portal DNS responder. */
    bool start_portal_dns_server();

    /* Stop all captive portal services and release their handles. */
    void stop_portal_services();

    /* Bring up access-point mode and portal services. */
    bool start_wifi_portal();

    /* Tear down the captive portal and return to normal station operation. */
    void stop_wifi_portal();

    /* Connect station mode using the saved WiFi credentials. */
    bool start_wifi_station();

    /* Recover from HTTP-related WiFi failures without entering the portal. */
    bool recover_wifi_after_http_failure();

    /* Dispatch ESP-IDF WiFi and IP events into the app's event bits. */
    void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

    /* Initialise network interfaces, events, and WiFi service state. */
    bool wifi_manager_init();

    /* Worker task that maintains WiFi state and periodically fetches aircraft data. */
    void aircraft_fetch_task(void *arg);

    /* Worker task that refreshes weather for visible airports. */
    void airport_weather_task(void *arg);

    /* Initialise NVS storage, erasing it once if the partition layout changed. */
    static void init_nvs();

    /* Create a styled label used by the full-screen overlay views. */
    static lv_obj_t *make_overlay_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                                        uint32_t color, int y, int width);

    /* Handle the close button on the aircraft detail popup. */
    void aircraft_popup_close_event(lv_event_t *event);

    /* Create the aircraft detail popup and its child widgets. */
    void create_aircraft_popup(lv_obj_t *screen);

    /* Create the captive portal instruction overlay. */
    void create_portal_overlay(lv_obj_t *screen);

    /* Create all LVGL widgets and timers for the main radar view. */
    void create_radar_ui();
};
