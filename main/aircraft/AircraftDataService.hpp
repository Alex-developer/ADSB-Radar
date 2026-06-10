#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#include "GpsReceiver.hpp"
#include "RadarSettings.hpp"
#include "RadarTypes.hpp"

/*
 * Converts raw Airplanes.live JSON into the compact aircraft snapshot used by
 * the UI.
 *
 * The fetch task owns temporary parsing buffers. Once parsing succeeds, this
 * service publishes a sorted, filtered snapshot into RadarApp's shared aircraft
 * buffer while holding aircraft_mutex. The UI only ever sees the normalised
 * aircraft_data_t records, not the source JSON.
 */
class AircraftDataService {
public:
    /*
     * Attach the shared storage owned by RadarApp.
     *
     * The pointers must remain valid for the life of the app. This keeps the
     * service small and avoids allocating another permanent aircraft store.
     */
    void bind(SemaphoreHandle_t mutex, aircraft_data_t *latest_items,
              size_t *latest_count, uint32_t *latest_generation,
              char *status_text, size_t status_size);

    /* Update the short status string shown by the DATA button. */
    void setStatus(const char *fmt, ...);

    /* Publish a parsed aircraft list and bump the generation counter. */
    void publish(aircraft_data_t *items, size_t count, int total);

    /*
     * Parse, filter, sort, and publish one Airplanes.live response.
     *
     * range_mi is the display range selected when the fetch was made, so stale
     * or out-of-range aircraft can be rejected before the UI sees them.
     */
    bool parseJson(const char *json, int json_len, int range_mi,
                   const RadarSettings &settings, const GpsReceiver &gps);

    /* Parse either numeric altitude fields or textual "ground" values. */
    static int parseAltitudeFt(cJSON *aircraft);

    /* qsort comparator: nearest aircraft first. */
    static int compareDistance(const void *left, const void *right);

private:
    SemaphoreHandle_t aircraft_mutex = nullptr;
    aircraft_data_t *latest_aircraft = nullptr;
    size_t *latest_aircraft_count = nullptr;
    uint32_t *latest_aircraft_generation = nullptr;
    char *latest_status = nullptr;
    size_t latest_status_size = 0;
};
