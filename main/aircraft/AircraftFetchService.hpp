#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_client.h"

#include "RadarSettings.hpp"

class RadarApp;

/*
 * Background aircraft downloader.
 *
 * The service is deliberately thin: RadarApp owns the WiFi state, settings,
 * shared aircraft buffer, and HTTP client. This class manages the fetch loop,
 * builds the selected aircraft feed URL for the current centre/range, and hands
 * successful JSON to AircraftDataService for parsing.
 */
class AircraftFetchService {
public:
    /* Attach the owning application used for WiFi, settings, and HTTP access. */
    void bind(RadarApp *app) { owner = app; }

    /*
     * Convert display miles to the nautical-mile radius expected by
     * internet ADS-B feeds. The request is rounded up so the display range is never
     * under-fetched.
     */
    static int milesToNauticalRequest(int range_mi);

    /* Build the point query URL for the current radar centre and source. */
    void buildAircraftUrl(char *url, size_t url_size, int range_mi,
                          int source = AIRCRAFT_DATA_SOURCE_AIRPLANES_LIVE) const;

    /* Fetch one aircraft JSON document using the shared TLS-serialised client. */
    esp_err_t fetchAircraftJson(esp_http_client_handle_t client, char **response_out,
                                int *response_len_out, int range_mi,
                                int source = AIRCRAFT_DATA_SOURCE_AIRPLANES_LIVE);

    /* FreeRTOS task entry body. Runs until the app is stopped or rebooted. */
    void task(void *arg);

private:
    RadarApp *owner = nullptr;
};
