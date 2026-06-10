#include "AircraftFetchService.hpp"

#include <math.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "RadarApp.hpp"
#include "RadarTypes.hpp"

static const char *TAG = "aircraft_fetch";

static const char *AIRCRAFT_API_HOSTS[] = {
    "api.airplanes.live",
    "api.adsb.lol",
    "opendata.adsb.fi",
};

/* Return the remote host configured by the data-source setting. */
static const char *aircraft_source_host(int source)
{
    switch (source) {
    case AIRCRAFT_DATA_SOURCE_ADSB_LOL:
        return AIRCRAFT_API_HOSTS[1];
    case AIRCRAFT_DATA_SOURCE_ADSB_FI:
        return AIRCRAFT_API_HOSTS[2];
    case AIRCRAFT_DATA_SOURCE_AIRPLANES_LIVE:
    default:
        return AIRCRAFT_API_HOSTS[0];
    }
}

/*
 * The fetch task is the application's long-running network loop. It waits for
 * WiFi, fetches the selected range at the range-specific interval, and keeps
 * the UI status meaningful when data is stale or WiFi needs recovery.
 */

/* Convert a display range in statute miles to the API radius in nautical miles. */
int AircraftFetchService::milesToNauticalRequest(int range_mi)
{
    int range_nmi = (int)ceilf((float)range_mi / MILES_PER_NAUTICAL_MILE);
    return range_nmi > 0 ? range_nmi : 1;
}

/* Build the aircraft point URL from the current radar centre, range, and source. */
void AircraftFetchService::buildAircraftUrl(char *url, size_t url_size, int range_mi,
                                            int source) const
{
    if (!owner || !url || url_size == 0) {
        return;
    }

    double center_lat = owner->settings.center_lat;
    double center_lon = owner->settings.center_lon;
    owner->get_radar_center(&center_lat, &center_lon);
    int radius_nm = milesToNauticalRequest(range_mi);
    if (source == AIRCRAFT_DATA_SOURCE_ADSB_FI) {
        snprintf(url, url_size, "https://%s/api/v3/lat/%.6f/lon/%.6f/dist/%d",
                 aircraft_source_host(source), center_lat, center_lon, radius_nm);
    } else {
        snprintf(url, url_size, "https://%s/v2/point/%.6f/%.6f/%d",
                 aircraft_source_host(source), center_lat, center_lon, radius_nm);
    }
}

/* Fetch one aircraft JSON payload using the shared HTTP client. */
esp_err_t AircraftFetchService::fetchAircraftJson(esp_http_client_handle_t client,
                                                  char **response_out,
                                                  int *response_len_out,
                                                  int range_mi,
                                                  int source)
{
    if (!owner) {
        return ESP_FAIL;
    }

    char url[256];
    buildAircraftUrl(url, sizeof(url), range_mi, source);
    return owner->http_client.fetchAircraftJson(client, url, response_out, response_len_out);
}

/* Run the WiFi-aware aircraft fetch loop until the device is rebooted. */
void AircraftFetchService::task(void *arg)
{
    (void)arg;
    if (!owner || !owner->wifi_manager_init()) {
        vTaskDelete(NULL);
        return;
    }

    if (owner->load_wifi_credentials()) {
        owner->start_wifi_station();
    } else {
        owner->start_wifi_portal();
    }

    bool had_successful_fetch = false;
    int consecutive_http_failures = 0;
    char last_success_status[32] = "";
    esp_http_client_handle_t aircraft_client = NULL;
    int active_remote_source = -1;

    for (;;) {
        /*
         * Captive portal work is handled here rather than in the UI callback so
         * WiFi start/stop operations happen on one task, away from LVGL.
         */
        if (owner->consume_wifi_portal_request()) {
            owner->start_wifi_portal();
        }

        if (owner->wifi_portal_active) {
            if (aircraft_client) {
                esp_http_client_cleanup(aircraft_client);
                aircraft_client = NULL;
            }
            EventBits_t portal_bits = xEventGroupWaitBits(owner->wifi_event_group,
                                                          WIFI_CREDENTIALS_CHANGED_BIT |
                                                          WIFI_PORTAL_REQUEST_BIT,
                                                          pdTRUE, pdFALSE,
                                                          pdMS_TO_TICKS(1000));
            if (portal_bits & WIFI_CREDENTIALS_CHANGED_BIT) {
                owner->stop_wifi_portal();
                if (!owner->start_wifi_station()) {
                    owner->start_wifi_portal();
                }
            }
            continue;
        }

        EventBits_t bits = xEventGroupWaitBits(owner->wifi_event_group,
                                               WIFI_CONNECTED_BIT |
                                               WIFI_PORTAL_REQUEST_BIT |
                                               WIFI_CREDENTIALS_CHANGED_BIT,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
        if (bits & WIFI_CREDENTIALS_CHANGED_BIT) {
            xEventGroupClearBits(owner->wifi_event_group, WIFI_CREDENTIALS_CHANGED_BIT);
            if (aircraft_client) {
                esp_http_client_cleanup(aircraft_client);
                aircraft_client = NULL;
            }
            owner->stop_settings_http_server();
            if (!owner->start_wifi_station()) {
                owner->start_wifi_portal();
            }
            continue;
        }
        if (bits & WIFI_PORTAL_REQUEST_BIT) {
            continue;
        }
        if (!(bits & WIFI_CONNECTED_BIT)) {
            if (aircraft_client) {
                esp_http_client_cleanup(aircraft_client);
                aircraft_client = NULL;
            }
            owner->set_data_status("NO WIFI");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        owner->ensure_airport_runways_cached();

        int range_mi = owner->get_current_range_mi();
        int data_source = owner->settings.aircraft_data_source;
        char *response = NULL;
        int response_len = 0;
        esp_err_t err = ESP_FAIL;

        if (!had_successful_fetch) {
            owner->set_data_status("FETCH");
        }

        if (data_source == AIRCRAFT_DATA_SOURCE_LOCAL) {
            if (aircraft_client) {
                esp_http_client_cleanup(aircraft_client);
                aircraft_client = NULL;
                active_remote_source = -1;
            }
            if (owner->settings.aircraft_local_url[0] == '\0') {
                err = ESP_ERR_INVALID_ARG;
            } else {
                err = owner->fetch_http_buffer(owner->settings.aircraft_local_url,
                                               "application/json",
                                               HTTP_RESPONSE_INITIAL_BYTES,
                                               HTTP_RESPONSE_MAX_BYTES,
                                               &response, &response_len);
            }
        } else {
            const char *host = aircraft_source_host(data_source);
            if (aircraft_client && active_remote_source != data_source) {
                esp_http_client_cleanup(aircraft_client);
                aircraft_client = NULL;
            }
            if (!aircraft_client) {
                aircraft_client = owner->http_client.createAircraftClient();
                active_remote_source = data_source;
            }
            if (!aircraft_client) {
                err = ESP_ERR_NO_MEM;
            } else {
                err = fetchAircraftJson(aircraft_client, &response, &response_len, range_mi, data_source);
            }

            if (err != ESP_OK && err != ESP_ERR_NO_MEM && err != ESP_ERR_INVALID_SIZE) {
                ESP_LOGW(TAG, "Aircraft fetch failed from %s: %s",
                         host, esp_err_to_name(err));
                esp_http_client_cleanup(aircraft_client);
                aircraft_client = NULL;
                active_remote_source = -1;
            }
        }

        if (err == ESP_OK) {
            bool parsed = owner->parse_aircraft_json(response, response_len, range_mi);
            heap_caps_free(response);
            if (parsed) {
                had_successful_fetch = true;
                consecutive_http_failures = 0;
                if (owner->aircraft_mutex &&
                    xSemaphoreTake(owner->aircraft_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    snprintf(last_success_status, sizeof(last_success_status), "%s",
                             owner->latest_status);
                    xSemaphoreGive(owner->aircraft_mutex);
                } else {
                    snprintf(last_success_status, sizeof(last_success_status), "%s",
                             owner->latest_status);
                }
            }
        } else if (err == ESP_ERR_NO_MEM) {
            owner->set_data_status("NO MEM");
        } else if (err == ESP_ERR_INVALID_SIZE) {
            owner->set_data_status("TOO BIG");
        } else {
            ++consecutive_http_failures;
            ESP_LOGW(TAG, "Aircraft fetch cycle failed (%d consecutive): %s",
                     consecutive_http_failures, esp_err_to_name(err));
            if (!had_successful_fetch) {
                owner->set_data_status("NO DATA");
            } else if (consecutive_http_failures >= 3) {
                owner->set_data_status("STALE");
            } else if (last_success_status[0] != '\0') {
                owner->set_data_status("%s", last_success_status);
            } else {
                owner->set_data_status("OK");
            }
        }

        bits = xEventGroupWaitBits(owner->wifi_event_group,
                                   FETCH_NOW_BIT |
                                   WIFI_PORTAL_REQUEST_BIT |
                                   WIFI_CREDENTIALS_CHANGED_BIT,
                                   pdFALSE, pdFALSE,
                                   pdMS_TO_TICKS(owner->get_current_refresh_interval_ms()));
        if (bits & FETCH_NOW_BIT) {
            xEventGroupClearBits(owner->wifi_event_group, FETCH_NOW_BIT);
        }
    }
}
