#include "AircraftDataService.hpp"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "RadarGeometry.hpp"
#include "RadarHttpClient.hpp"
#include "RadarPositionProvider.hpp"

/*
 * Airplanes.live returns a broad ADS-B record. This implementation keeps only
 * the fields needed for the radar view, derives distance and bearing from the
 * active centre, and publishes a compact snapshot. The snapshot is copied under
 * a mutex so the LVGL task never parses JSON or waits on the network.
 */

/* Attach the shared aircraft snapshot storage owned by RadarApp. */
void AircraftDataService::bind(SemaphoreHandle_t mutex, aircraft_data_t *latest_items,
                               size_t *latest_count, uint32_t *latest_generation,
                               char *status_text, size_t status_size)
{
    aircraft_mutex = mutex;
    latest_aircraft = latest_items;
    latest_aircraft_count = latest_count;
    latest_aircraft_generation = latest_generation;
    latest_status = status_text;
    latest_status_size = status_size;
}

/* Update the short DATA status text under the aircraft mutex when available. */
void AircraftDataService::setStatus(const char *fmt, ...)
{
    if (!latest_status || latest_status_size == 0) {
        return;
    }

    char text[32];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    if (aircraft_mutex && xSemaphoreTake(aircraft_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        snprintf(latest_status, latest_status_size, "%s", text);
        xSemaphoreGive(aircraft_mutex);
    } else {
        snprintf(latest_status, latest_status_size, "%s", text);
    }
}

/* Parse the Airplanes.live barometric altitude field into feet. */
int AircraftDataService::parseAltitudeFt(cJSON *aircraft)
{
    cJSON *alt = cJSON_GetObjectItemCaseSensitive(aircraft, "alt_baro");
    if (cJSON_IsNumber(alt)) {
        return alt->valueint;
    }
    if (cJSON_IsString(alt) && alt->valuestring && strcmp(alt->valuestring, "ground") == 0) {
        return 0;
    }
    return INT_MIN;
}

/* Compare two aircraft by distance for nearest-first sorting. */
int AircraftDataService::compareDistance(const void *left, const void *right)
{
    const aircraft_data_t *a = (const aircraft_data_t *)left;
    const aircraft_data_t *b = (const aircraft_data_t *)right;
    if (a->distance_mi < b->distance_mi) {
        return -1;
    }
    if (a->distance_mi > b->distance_mi) {
        return 1;
    }
    return 0;
}

/* Publish a parsed aircraft list into the shared UI snapshot. */
void AircraftDataService::publish(aircraft_data_t *items, size_t count, int total)
{
    if (!latest_aircraft || !latest_aircraft_count || !latest_aircraft_generation ||
        !latest_status || latest_status_size == 0) {
        return;
    }

    if (count > MAX_AIRCRAFT_TARGETS) {
        count = MAX_AIRCRAFT_TARGETS;
    }

    if (aircraft_mutex && xSemaphoreTake(aircraft_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(latest_aircraft, items, count * sizeof(latest_aircraft[0]));
        *latest_aircraft_count = count;
        (*latest_aircraft_generation)++;
        if ((int)count < total) {
            snprintf(latest_status, latest_status_size, "%u/%d", (unsigned)count, total);
        } else {
            snprintf(latest_status, latest_status_size, "%d AC", total);
        }
        xSemaphoreGive(aircraft_mutex);
    }
}

/* Parse, filter, sort, and publish one aircraft JSON response. */
bool AircraftDataService::parseJson(const char *json, int json_len, int range_mi,
                                    const RadarSettings &settings, const GpsReceiver &gps)
{
    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root) {
        setStatus("BAD JSON");
        return false;
    }

    cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "ac");
    if (!cJSON_IsArray(array)) {
        array = cJSON_GetObjectItemCaseSensitive(root, "aircraft");
    }
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(root);
        setStatus("NO ARRAY");
        return false;
    }

    cJSON *total_item = cJSON_GetObjectItemCaseSensitive(root, "total");
    int api_total = cJSON_IsNumber(total_item) ? total_item->valueint : cJSON_GetArraySize(array);
    aircraft_data_t *parsed = (aircraft_data_t *)heap_caps_malloc(MAX_PARSED_AIRCRAFT * sizeof(parsed[0]),
                                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!parsed) {
        parsed = (aircraft_data_t *)heap_caps_malloc(MAX_PARSED_AIRCRAFT * sizeof(parsed[0]),
                                                     MALLOC_CAP_8BIT);
    }
    if (!parsed) {
        cJSON_Delete(root);
        setStatus("NO MEM");
        return false;
    }
    size_t parsed_count = 0;
    double center_lat = settings.center_lat;
    double center_lon = settings.center_lon;
    RadarPositionProvider::resolve(settings, gps, &center_lat, &center_lon);

    cJSON *aircraft;
    size_t scanned_count = 0;
    cJSON_ArrayForEach(aircraft, array) {
        if ((++scanned_count & 0x1f) == 0) {
            taskYIELD();
        }

        if (!cJSON_IsObject(aircraft)) {
            continue;
        }

        double distance = 0.0;
        double bearing = 0.0;
        bool has_position = RadarHttpClient::jsonGetNumber(aircraft, "dst", &distance) &&
                            RadarHttpClient::jsonGetNumber(aircraft, "dir", &bearing);
        if (!has_position) {
            double lat = 0.0;
            double lon = 0.0;
            if (!RadarHttpClient::jsonGetNumber(aircraft, "lat", &lat) ||
                !RadarHttpClient::jsonGetNumber(aircraft, "lon", &lon)) {
                continue;
            }
            distance = RadarGeometry::distanceMiles(center_lat, center_lon, lat, lon);
            bearing = RadarGeometry::bearingDegrees(center_lat, center_lon, lat, lon);
        } else {
            distance *= MILES_PER_NAUTICAL_MILE;
        }

        if (distance < 0.0 || distance > (double)range_mi) {
            continue;
        }

        char name[12];
        RadarHttpClient::copyTrimmedField(name, sizeof(name), aircraft, "flight");
        if (name[0] == '\0') {
            RadarHttpClient::copyTrimmedField(name, sizeof(name), aircraft, "r");
        }
        if (name[0] == '\0') {
            RadarHttpClient::copyTrimmedField(name, sizeof(name), aircraft, "hex");
        }
        if (name[0] == '\0') {
            snprintf(name, sizeof(name), "UNK");
        }

        double gs = 0.0;
        double seen = 0.0;
        double heading = -1.0;
        double vertical_rate = 0.0;
        bool has_vertical_rate = false;
        double db_flags = 0.0;
        bool has_db_flags = false;
        RadarHttpClient::jsonGetNumber(aircraft, "gs", &gs);
        RadarHttpClient::jsonGetNumber(aircraft, "seen", &seen);
        if (!RadarHttpClient::jsonGetNumber(aircraft, "track", &heading) &&
            !RadarHttpClient::jsonGetNumber(aircraft, "true_heading", &heading) &&
            !RadarHttpClient::jsonGetNumber(aircraft, "mag_heading", &heading) &&
            !RadarHttpClient::jsonGetNumber(aircraft, "nav_heading", &heading)) {
            heading = -1.0;
        }
        has_vertical_rate = RadarHttpClient::jsonGetNumber(aircraft, "baro_rate", &vertical_rate) ||
                            RadarHttpClient::jsonGetNumber(aircraft, "geom_rate", &vertical_rate);
        has_db_flags = RadarHttpClient::jsonGetNumber(aircraft, "dbFlags", &db_flags) ||
                       RadarHttpClient::jsonGetNumber(aircraft, "dbflags", &db_flags);

        aircraft_data_t item = {};
        item.bearing_deg = (int)lround(fmod(bearing + 360.0, 360.0));
        item.heading_deg = heading >= 0.0 ? (int)lround(fmod(heading + 360.0, 360.0)) : -1;
        item.distance_mi = (float)distance;
        item.altitude_ft = parseAltitudeFt(aircraft);
        item.speed_kt = (int)lround(gs);
        item.vertical_rate_fpm = has_vertical_rate ? (int)lround(vertical_rate) : INT_MIN;
        item.seen_s = (float)seen;
        item.has_db_flags = has_db_flags;
        if (has_db_flags) {
            int flags = (int)lround(db_flags);
            if (flags < 0) {
                flags = 0;
            } else if (flags > 255) {
                flags = 255;
            }
            item.db_flags = (uint8_t)flags;
        }
        if (item.bearing_deg == 360) {
            item.bearing_deg = 0;
        }
        if (item.heading_deg == 360) {
            item.heading_deg = 0;
        }

        snprintf(item.callsign, sizeof(item.callsign), "%s", name);
        RadarHttpClient::copyTrimmedField(item.icao, sizeof(item.icao), aircraft, "hex");
        RadarHttpClient::copyTrimmedField(item.registration, sizeof(item.registration), aircraft, "r");
        RadarHttpClient::copyTrimmedField(item.type, sizeof(item.type), aircraft, "t");
        RadarHttpClient::copyTrimmedField(item.squawk, sizeof(item.squawk), aircraft, "squawk");
        if (item.altitude_ft == INT_MIN) {
            snprintf(item.detail, sizeof(item.detail), "--- / %d", item.speed_kt);
        } else if (item.altitude_ft == 0) {
            snprintf(item.detail, sizeof(item.detail), "GND / %d", item.speed_kt);
        } else {
            snprintf(item.detail, sizeof(item.detail), "%03d / %d",
                     (item.altitude_ft + 50) / 100, item.speed_kt);
        }

        if (parsed_count < MAX_PARSED_AIRCRAFT) {
            parsed[parsed_count++] = item;
        }
    }

    qsort(parsed, parsed_count, sizeof(parsed[0]), compareDistance);
    publish(parsed, parsed_count, api_total);
    heap_caps_free(parsed);
    cJSON_Delete(root);
    return true;
}
