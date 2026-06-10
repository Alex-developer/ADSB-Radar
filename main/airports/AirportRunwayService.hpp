#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "RadarHttpClient.hpp"
#include "RadarSettings.hpp"
#include "RadarTypes.hpp"

/*
 * AirportDB runway cache and fetch service.
 *
 * Runway data is only meaningful when the radar centre is an airport. AirportDB
 * has a monthly request limit, so the service keeps an in-memory active cache,
 * stores successful responses in NVS by ICAO code, and rate-limits failed
 * attempts to avoid repeatedly burning API calls.
 */
class AirportRunwayService {
public:
    /* Create the mutex used to guard the active runway cache. */
    void init();

    /*
     * Ensure the active cache matches the selected airport centre.
     *
     * This may load from NVS or perform one HTTPS request if there is no cached
     * data and enough time has passed since the last failed attempt.
     */
    void ensureCached(const RadarSettings &settings, RadarHttpClient &http_client,
                      const char *origin, uint32_t *settings_generation);

    /* Replace the active cache and mark the UI as needing a redraw. */
    void publishCache(const airport_runway_cache_t *cache, uint32_t *settings_generation);

    /* Copy the current active cache for drawing. */
    bool getActiveCache(airport_runway_cache_t *cache);

    /* Drop the active cache and mark the UI as needing a redraw. */
    void clearActiveCache(uint32_t *settings_generation);

private:
    SemaphoreHandle_t runway_mutex = nullptr;
    airport_runway_cache_t runway_cache = {};
    char runway_last_attempt_icao[5] = {};
    int64_t runway_last_attempt_ms = 0;

    /* Build a short NVS key from an ICAO code. */
    static void buildCacheKey(char *key, size_t key_size, const char *icao);

    /* Load cached runway endpoints from NVS. */
    bool loadCache(const char *icao, airport_runway_cache_t *cache);

    /* Save a successful AirportDB response in compact binary form. */
    esp_err_t saveCache(const airport_runway_cache_t *cache);

    /* Parse the runway subset needed by the radar from AirportDB JSON. */
    bool parseRunways(const char *json, int json_len, const char *icao, airport_runway_cache_t *cache);

    /* Fetch and parse runway data from AirportDB. */
    esp_err_t fetchRunways(const RadarSettings &settings, RadarHttpClient &http_client,
                           const char *origin, const char *icao, airport_runway_cache_t *cache);
};
