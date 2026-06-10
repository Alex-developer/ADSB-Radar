#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

class RadarApp;

/*
 * Browser settings API and page server.
 *
 * This server runs on the station interface once WiFi is connected. It serves
 * the Bootstrap settings UI, JSON configuration endpoints, WiFi scan/save
 * endpoints, airport/location search, and the status page used for diagnostics.
 */
class SettingsServer {
public:
    /* Attach the owning application that supplies settings and WiFi state. */
    void bind(RadarApp *app) { owner = app; }

    /* Send the current settings plus runtime WiFi context as JSON. */
    esp_err_t sendSettingsJson(httpd_req_t *req);

    /* Send built-in defaults without modifying live settings. */
    esp_err_t sendDefaultsJson(httpd_req_t *req);

    /* Send a small { ok, message } response used by AJAX handlers. */
    esp_err_t sendJsonStatus(httpd_req_t *req, bool ok, const char *message);

    /* Serve the Bootstrap settings web application. */
    esp_err_t pageHandler(httpd_req_t *req);

    /* Return the current settings JSON document. */
    esp_err_t apiGetHandler(httpd_req_t *req);

    /* Validate and save a submitted settings JSON document. */
    esp_err_t apiSaveHandler(httpd_req_t *req);

    /* Return live ESP32 memory, cache, WiFi, GPS, and fetch status. */
    esp_err_t statusHandler(httpd_req_t *req);

    /* Capture the current LVGL display and stream it as a BMP image. */
    esp_err_t screenshotHandler(httpd_req_t *req);

    /* Return the factory-default settings JSON document. */
    esp_err_t defaultsHandler(httpd_req_t *req);

    /* Reset only the configured range presets to defaults. */
    esp_err_t rangesResetHandler(httpd_req_t *req);

    /* Search the generated airport table for autocomplete. */
    esp_err_t airportSearchHandler(httpd_req_t *req);

    /* Search OpenWeather geocoding results for a named location. */
    esp_err_t locationSearchHandler(httpd_req_t *req);

    /* Return nearby WiFi networks for the WiFi settings tab. */
    esp_err_t wifiScanHandler(httpd_req_t *req);

    /* Save WiFi credentials submitted from the settings page. */
    esp_err_t wifiSaveHandler(httpd_req_t *req);

    /* Start the station-mode settings server. */
    bool start();

    /* Stop the station-mode settings server. */
    void stop();

private:
    RadarApp *owner = nullptr;
};
