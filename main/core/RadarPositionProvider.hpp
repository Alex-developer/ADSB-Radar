#pragma once

#include "GpsReceiver.hpp"
#include "RadarSettings.hpp"

/*
 * Chooses the active radar centre from settings and GPS state.
 *
 * Manual, airport, and location centres come directly from persisted settings.
 * GPS is used only when selected and a fresh fix is available; otherwise the
 * resolver falls back to the manual centre so aircraft fetching can continue.
 */
class RadarPositionProvider {
public:
    /*
     * Resolve the active latitude/longitude.
     *
     * using_gps is set when the returned position came from a current GPS fix.
     */
    static bool resolve(const RadarSettings &settings, const GpsReceiver &gps,
                        double *lat, double *lon, bool *using_gps = nullptr);
};
