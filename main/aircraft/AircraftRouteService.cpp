#include "AircraftRouteService.hpp"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "bsp/display.h"
#include "RadarApp.hpp"
#include "RadarTypes.hpp"

static const char *TAG = "aircraft_route";

#define ROUTE_JSON_INITIAL_BYTES 2048
#define ROUTE_JSON_MAX_BYTES (12 * 1024)
#define ROUTE_HTTP_TIMEOUT_MS 3500
#define ROUTE_TASK_STACK 8192
#define ROUTE_TASK_PRIORITY 1

typedef struct {
    uint32_t request_id;
    char callsign[16];
    bool long_route;
} route_fetch_request_t;

/* Return the best display code for an airport object. */
static const char *airportCode(cJSON *airport)
{
    if (!cJSON_IsObject(airport)) {
        return "";
    }

    cJSON *iata = cJSON_GetObjectItemCaseSensitive(airport, "iata_code");
    if (cJSON_IsString(iata) && iata->valuestring && iata->valuestring[0] != '\0') {
        return iata->valuestring;
    }

    cJSON *icao = cJSON_GetObjectItemCaseSensitive(airport, "icao_code");
    if (cJSON_IsString(icao) && icao->valuestring && icao->valuestring[0] != '\0') {
        return icao->valuestring;
    }

    return "";
}

/* Return the readable airport name for long route display. */
static const char *airportName(cJSON *airport)
{
    if (!cJSON_IsObject(airport)) {
        return "";
    }

    cJSON *name = cJSON_GetObjectItemCaseSensitive(airport, "name");
    if (cJSON_IsString(name) && name->valuestring && name->valuestring[0] != '\0') {
        return name->valuestring;
    }

    return airportCode(airport);
}

/* Convert a callsign into the compact ADSBDB path value. */
bool AircraftRouteService::normaliseCallsign(char *dst, size_t dst_size, const char *src)
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
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) {
            dst[out++] = (char)toupper(c);
        }
    }
    dst[out] = '\0';
    return out > 1;
}

/* Extract a route line from an ADSBDB callsign response. */
bool AircraftRouteService::parseRouteText(const char *json, int json_len, bool long_route,
                                          char *dst, size_t dst_size)
{
    if (!json || json_len <= 0 || !dst || dst_size == 0) {
        return false;
    }
    dst[0] = '\0';

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root) {
        return false;
    }

    bool parsed = false;
    cJSON *response = cJSON_GetObjectItemCaseSensitive(root, "response");
    cJSON *route = cJSON_IsObject(response) ?
                   cJSON_GetObjectItemCaseSensitive(response, "flightroute") : NULL;
    cJSON *origin = cJSON_IsObject(route) ?
                    cJSON_GetObjectItemCaseSensitive(route, "origin") : NULL;
    cJSON *destination = cJSON_IsObject(route) ?
                         cJSON_GetObjectItemCaseSensitive(route, "destination") : NULL;

    const char *origin_text = long_route ? airportName(origin) : airportCode(origin);
    const char *destination_text = long_route ? airportName(destination) : airportCode(destination);
    if (origin_text[0] != '\0' || destination_text[0] != '\0') {
        if (long_route) {
            snprintf(dst, dst_size, "FROM %s\nTO %s",
                     origin_text[0] != '\0' ? origin_text : "-",
                     destination_text[0] != '\0' ? destination_text : "-");
        } else {
            snprintf(dst, dst_size, "ROUTE %s > %s",
                     origin_text[0] != '\0' ? origin_text : "-",
                     destination_text[0] != '\0' ? destination_text : "-");
        }
        parsed = true;
    }

    cJSON_Delete(root);
    return parsed;
}

/* Fetch a route JSON buffer, stepping aside when another TLS operation is active. */
esp_err_t AircraftRouteService::fetchBufferWithRetry(const char *url,
                                                     char **response_out,
                                                     int *response_len_out)
{
    constexpr int max_attempts = 20;
    esp_err_t err = ESP_FAIL;

    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        err = owner->fetch_http_buffer(url, "application/json",
                                       ROUTE_JSON_INITIAL_BYTES, ROUTE_JSON_MAX_BYTES,
                                       response_out, response_len_out,
                                       PHOTO_OPTIONAL_TLS_MUTEX_TIMEOUT_MS,
                                       ROUTE_HTTP_TIMEOUT_MS);
        if (err != ESP_ERR_NO_MEM && err != ESP_ERR_TIMEOUT) {
            return err;
        }

        ESP_LOGW(TAG, "Route fetch deferred (%s), retry %d/%d",
                 esp_err_to_name(err), attempt, max_attempts);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return err;
}

/* Return whether a route worker result still belongs to the current popup. */
bool AircraftRouteService::requestIsCurrent(uint32_t request_id) const
{
    return owner &&
           request_id == owner->aircraft_route_request_id &&
           owner->aircraft_popup &&
           !lv_obj_has_flag(owner->aircraft_popup, LV_OBJ_FLAG_HIDDEN);
}

/* Update the popup body with route text under the LVGL display lock. */
void AircraftRouteService::updateRouteFromTask(uint32_t request_id, const char *route_text)
{
    if (!owner) {
        return;
    }
    owner->update_oled_activity("%s", route_text && route_text[0] ? route_text : "ROUTE NONE");
    if (bsp_display_lock(1000) != ESP_OK) {
        return;
    }

    if (requestIsCurrent(request_id) && owner->aircraft_popup_body) {
        char body[512];
        snprintf(body, sizeof(body), "%s\n%s",
                 owner->aircraft_popup_base_body,
                 route_text && route_text[0] != '\0' ? route_text : "ROUTE NONE");
        lv_label_set_text(owner->aircraft_popup_body, body);
    }

    bsp_display_unlock();
}

/* Fetch route metadata from ADSBDB and update the active popup. */
void AircraftRouteService::fetchTask(void *arg)
{
    route_fetch_request_t *request = (route_fetch_request_t *)arg;
    if (!owner || !request) {
        if (owner) {
            owner->aircraft_route_fetch_running = false;
        }
        vTaskDeleteWithCaps(NULL);
        return;
    }

    char url[96];
    snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", request->callsign);

    char *json = NULL;
    int json_len = 0;
    char route_text[192];
    esp_err_t err = fetchBufferWithRetry(url, &json, &json_len);
    if (err == ESP_OK && parseRouteText(json, json_len, request->long_route,
                                        route_text, sizeof(route_text))) {
        updateRouteFromTask(request->request_id, route_text);
    } else {
        ESP_LOGW(TAG, "Route fetch failed for %s: %s", request->callsign, esp_err_to_name(err));
        updateRouteFromTask(request->request_id,
                            err == ESP_ERR_NO_MEM ? "ROUTE MEM LOW" :
                            (err == ESP_ERR_TIMEOUT ? "ROUTE BUSY" : "ROUTE NONE"));
    }

    if (json) {
        heap_caps_free(json);
    }
    heap_caps_free(request);
    owner->aircraft_route_fetch_running = false;
    vTaskDeleteWithCaps(NULL);
}

/* Queue a new route fetch for one aircraft callsign. */
void AircraftRouteService::startFetch(const char *callsign)
{
    if (!owner) {
        return;
    }

    owner->aircraft_route_request_id++;
    if (!owner->settings.show_aircraft_routes) {
        return;
    }

    char normalised[16];
    if (!normaliseCallsign(normalised, sizeof(normalised), callsign)) {
        updateRouteFromTask(owner->aircraft_route_request_id, "ROUTE NO CALLSIGN");
        return;
    }
    if (owner->aircraft_route_fetch_running) {
        updateRouteFromTask(owner->aircraft_route_request_id, "ROUTE BUSY");
        return;
    }

    route_fetch_request_t *request = (route_fetch_request_t *)heap_caps_calloc(1, sizeof(*request),
                                                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!request) {
        request = (route_fetch_request_t *)heap_caps_calloc(1, sizeof(*request), MALLOC_CAP_8BIT);
    }
    if (!request) {
        updateRouteFromTask(owner->aircraft_route_request_id, "ROUTE NO MEM");
        return;
    }

    snprintf(request->callsign, sizeof(request->callsign), "%s", normalised);
    request->request_id = owner->aircraft_route_request_id;
    request->long_route = owner->settings.aircraft_route_style == AIRCRAFT_ROUTE_STYLE_LONG;
    owner->aircraft_route_fetch_running = true;
    updateRouteFromTask(request->request_id, "ROUTE FETCH");

    if (xTaskCreateWithCaps(RadarApp::aircraft_route_fetch_task_entry, "aircraft_route",
                            ROUTE_TASK_STACK, request, ROUTE_TASK_PRIORITY, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        owner->aircraft_route_fetch_running = false;
        heap_caps_free(request);
        updateRouteFromTask(owner->aircraft_route_request_id, "ROUTE ERR");
    }
}
