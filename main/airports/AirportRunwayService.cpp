#include "AirportRunwayService.hpp"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "runway_service";

/*
 * AirportDB data is cached aggressively. A runway fetch should happen only
 * after the user selects an airport centre, and the result is stored as compact
 * endpoint coordinates so subsequent boots do not consume the API quota.
 */

/* Create the mutex that protects the active runway cache. */
void AirportRunwayService::init()
{
    if (!runway_mutex) {
        runway_mutex = xSemaphoreCreateMutex();
    }
}

/* Build the NVS cache key for one normalised airport ICAO code. */
void AirportRunwayService::buildCacheKey(char *key, size_t key_size, const char *icao)
{
    char normalized[5];
    if (!key || key_size == 0 ||
        !RadarHttpClient::normalizeIcaoCode(normalized, sizeof(normalized), icao)) {
        if (key && key_size > 0) {
            key[0] = '\0';
        }
        return;
    }
    snprintf(key, key_size, "rw_%s", normalized);
}

/* Load a cached runway response for an airport from NVS. */
bool AirportRunwayService::loadCache(const char *icao, airport_runway_cache_t *cache)
{
    if (!cache) {
        return false;
    }
    memset(cache, 0, sizeof(*cache));

    char key[16];
    buildCacheKey(key, sizeof(key), icao);
    if (key[0] == '\0') {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(AIRPORTDB_CACHE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t size = sizeof(*cache);
    err = nvs_get_blob(handle, key, cache, &size);
    nvs_close(handle);
    if (err != ESP_OK || size != sizeof(*cache) ||
        cache->version != AIRPORTDB_CACHE_VERSION ||
        cache->count > MAX_AIRPORT_RUNWAYS) {
        memset(cache, 0, sizeof(*cache));
        return false;
    }

    char normalized[5];
    if (!RadarHttpClient::normalizeIcaoCode(normalized, sizeof(normalized), icao) ||
        strcmp(cache->icao, normalized) != 0) {
        memset(cache, 0, sizeof(*cache));
        return false;
    }
    return true;
}

/* Save a compact runway cache entry to NVS. */
esp_err_t AirportRunwayService::saveCache(const airport_runway_cache_t *cache)
{
    if (!cache || cache->version != AIRPORTDB_CACHE_VERSION ||
        cache->icao[0] == '\0' || cache->count > MAX_AIRPORT_RUNWAYS) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[16];
    buildCacheKey(key, sizeof(key), cache->icao);
    if (key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(AIRPORTDB_CACHE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, key, cache, sizeof(*cache));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/* Parse the runway endpoints needed by the radar from AirportDB JSON. */
bool AirportRunwayService::parseRunways(const char *json, int json_len, const char *icao,
                                        airport_runway_cache_t *cache)
{
    if (!json || json_len <= 0 || !cache) {
        return false;
    }

    char normalized[5];
    if (!RadarHttpClient::normalizeIcaoCode(normalized, sizeof(normalized), icao)) {
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return false;
    }

    /*
     * Keep only open runways with both end coordinates. Labels and surface
     * details are deliberately ignored on the radar so runways remain subtle.
     */
    cJSON *runways = cJSON_GetObjectItemCaseSensitive(root, "runways");
    if (!cJSON_IsArray(runways)) {
        cJSON_Delete(root);
        return false;
    }

    airport_runway_cache_t parsed = {};
    parsed.version = AIRPORTDB_CACHE_VERSION;
    snprintf(parsed.icao, sizeof(parsed.icao), "%s", normalized);

    cJSON *runway;
    cJSON_ArrayForEach(runway, runways) {
        if (!cJSON_IsObject(runway) || parsed.count >= MAX_AIRPORT_RUNWAYS) {
            continue;
        }

        cJSON *closed = cJSON_GetObjectItemCaseSensitive(runway, "closed");
        if ((cJSON_IsNumber(closed) && closed->valueint != 0) ||
            (cJSON_IsString(closed) && closed->valuestring && strcmp(closed->valuestring, "0") != 0)) {
            continue;
        }

        double le_lat = 0.0;
        double le_lon = 0.0;
        double he_lat = 0.0;
        double he_lon = 0.0;
        if (!RadarHttpClient::jsonGetDouble(runway, "le_latitude_deg", &le_lat) ||
            !RadarHttpClient::jsonGetDouble(runway, "le_longitude_deg", &le_lon) ||
            !RadarHttpClient::jsonGetDouble(runway, "he_latitude_deg", &he_lat) ||
            !RadarHttpClient::jsonGetDouble(runway, "he_longitude_deg", &he_lon)) {
            continue;
        }

        if (le_lat < -90.0 || le_lat > 90.0 || he_lat < -90.0 || he_lat > 90.0 ||
            le_lon < -180.0 || le_lon > 180.0 || he_lon < -180.0 || he_lon > 180.0) {
            continue;
        }

        airport_runway_t *out = &parsed.runways[parsed.count++];
        out->le_lat_e6 = (int32_t)lround(le_lat * 1000000.0);
        out->le_lon_e6 = (int32_t)lround(le_lon * 1000000.0);
        out->he_lat_e6 = (int32_t)lround(he_lat * 1000000.0);
        out->he_lon_e6 = (int32_t)lround(he_lon * 1000000.0);
        RadarHttpClient::copyTrimmedField(out->le_ident, sizeof(out->le_ident), runway, "le_ident");
        RadarHttpClient::copyTrimmedField(out->he_ident, sizeof(out->he_ident), runway, "he_ident");
    }

    cJSON_Delete(root);
    *cache = parsed;
    return true;
}

/* Fetch runway data from AirportDB and parse it into a compact cache structure. */
esp_err_t AirportRunwayService::fetchRunways(const RadarSettings &settings,
                                             RadarHttpClient &http_client,
                                             const char *origin,
                                             const char *icao,
                                             airport_runway_cache_t *cache)
{
    if (!cache || !icao || settings.airportdb_api_token[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char normalized[5];
    if (!RadarHttpClient::normalizeIcaoCode(normalized, sizeof(normalized), icao)) {
        return ESP_ERR_INVALID_ARG;
    }

    char encoded_token[sizeof(settings.airportdb_api_token) * 3];
    size_t out = 0;
    for (const char *p = settings.airportdb_api_token; *p && out + 4 < sizeof(encoded_token); ++p) {
        unsigned char ch = (unsigned char)*p;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded_token[out++] = (char)ch;
        } else {
            snprintf(encoded_token + out, sizeof(encoded_token) - out, "%%%02X", ch);
            out += 3;
        }
    }
    encoded_token[out] = '\0';

    char url[(AIRPORTDB_API_TOKEN_MAX * 3) + 96];
    snprintf(url, sizeof(url), "https://airportdb.io/api/v1/airport/%s?apiToken=%s",
             normalized, encoded_token);

    char *response = NULL;
    int response_len = 0;
    esp_err_t err = http_client.fetchBuffer(url, "application/json", origin,
                                            AIRPORTDB_JSON_INITIAL_BYTES,
                                            AIRPORTDB_JSON_MAX_BYTES,
                                            &response, &response_len);
    if (err != ESP_OK) {
        return err;
    }

    bool parsed = parseRunways(response, response_len, normalized, cache);
    heap_caps_free(response);
    return parsed ? ESP_OK : ESP_FAIL;
}

/* Replace the active runway cache and invalidate the radar overlay. */
void AirportRunwayService::publishCache(const airport_runway_cache_t *cache,
                                        uint32_t *settings_generation)
{
    if (!cache || cache->version != AIRPORTDB_CACHE_VERSION ||
        cache->count > MAX_AIRPORT_RUNWAYS) {
        return;
    }

    if (runway_mutex && xSemaphoreTake(runway_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        runway_cache = *cache;
        xSemaphoreGive(runway_mutex);
    } else {
        runway_cache = *cache;
    }
    if (settings_generation) {
        (*settings_generation)++;
    }
}

/* Copy the active runway cache if it is populated and structurally valid. */
bool AirportRunwayService::getActiveCache(airport_runway_cache_t *cache)
{
    if (!cache) {
        return false;
    }

    bool valid = false;
    if (runway_mutex && xSemaphoreTake(runway_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        valid = runway_cache.version == AIRPORTDB_CACHE_VERSION &&
                runway_cache.icao[0] != '\0' &&
                runway_cache.count <= MAX_AIRPORT_RUNWAYS;
        if (valid) {
            *cache = runway_cache;
        }
        xSemaphoreGive(runway_mutex);
    } else {
        valid = runway_cache.version == AIRPORTDB_CACHE_VERSION &&
                runway_cache.icao[0] != '\0' &&
                runway_cache.count <= MAX_AIRPORT_RUNWAYS;
        if (valid) {
            *cache = runway_cache;
        }
    }
    return valid;
}

/* Clear active runway data and invalidate the radar overlay if needed. */
void AirportRunwayService::clearActiveCache(uint32_t *settings_generation)
{
    bool had_cache = runway_cache.icao[0] != '\0';
    if (runway_mutex && xSemaphoreTake(runway_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        had_cache = runway_cache.icao[0] != '\0';
        memset(&runway_cache, 0, sizeof(runway_cache));
        xSemaphoreGive(runway_mutex);
    } else {
        memset(&runway_cache, 0, sizeof(runway_cache));
    }
    if (had_cache && settings_generation) {
        (*settings_generation)++;
    }
}

/* Ensure runway data is available for the selected airport without wasting API calls. */
void AirportRunwayService::ensureCached(const RadarSettings &settings,
                                        RadarHttpClient &http_client,
                                        const char *origin,
                                        uint32_t *settings_generation)
{
    char icao[5];
    if (!settings.show_airport_runways ||
        settings.center_source != RADAR_CENTER_SOURCE_AIRPORT ||
        !RadarHttpClient::normalizeIcaoCode(icao, sizeof(icao), settings.center_airport_code)) {
        clearActiveCache(settings_generation);
        return;
    }

    airport_runway_cache_t active = {};
    if (getActiveCache(&active) && strcmp(active.icao, icao) == 0) {
        return;
    }

    airport_runway_cache_t cached = {};
    if (loadCache(icao, &cached)) {
        ESP_LOGI(TAG, "Loaded cached runways for %s: %u", icao, cached.count);
        publishCache(&cached, settings_generation);
        return;
    }

    if (settings.airportdb_api_token[0] == '\0') {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (strcmp(runway_last_attempt_icao, icao) == 0 &&
        now_ms - runway_last_attempt_ms < AIRPORTDB_FETCH_COOLDOWN_MS) {
        return;
    }
    snprintf(runway_last_attempt_icao, sizeof(runway_last_attempt_icao), "%s", icao);
    runway_last_attempt_ms = now_ms;

    ESP_LOGI(TAG, "Fetching AirportDB runways for %s", icao);
    airport_runway_cache_t fetched = {};
    esp_err_t err = fetchRunways(settings, http_client, origin, icao, &fetched);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AirportDB runway fetch failed for %s: %s", icao, esp_err_to_name(err));
        return;
    }

    err = saveCache(&fetched);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saving runway cache failed for %s: %s", icao, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "Fetched AirportDB runways for %s: %u", icao, fetched.count);
    publishCache(&fetched, settings_generation);
}
