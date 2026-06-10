#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

class RadarApp;

/*
 * Fetches route details for the aircraft popup.
 *
 * Route lookups are optional and run only after the user taps an aircraft. Late
 * worker results are discarded with a request id, matching the photo service
 * pattern used by the same popup.
 */
class AircraftRouteService {
public:
    /* Attach the owning application used for popup state and HTTP access. */
    void bind(RadarApp *app) { owner = app; }

    /* Convert a callsign into an ADSBDB-safe path segment. */
    static bool normaliseCallsign(char *dst, size_t dst_size, const char *src);

    /* Extract an origin-to-destination route string from ADSBDB JSON. */
    static bool parseRouteText(const char *json, int json_len, bool long_route,
                               char *dst, size_t dst_size);

    /* Check whether a worker result still belongs to the visible popup. */
    bool requestIsCurrent(uint32_t request_id) const;

    /* Safely update the popup route text from the worker task. */
    void updateRouteFromTask(uint32_t request_id, const char *route_text);

    /* Worker task body for route metadata fetch and popup update. */
    void fetchTask(void *arg);

    /* Start a route request for the supplied flight callsign. */
    void startFetch(const char *callsign);

private:
    /* Fetch a small JSON response, retrying transient TLS pressure. */
    esp_err_t fetchBufferWithRetry(const char *url, char **response_out, int *response_len_out);

    RadarApp *owner = nullptr;
};
