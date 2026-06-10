#include "RadarPositionProvider.hpp"

/* Resolve the active radar centre from settings, falling back safely when GPS is stale. */
bool RadarPositionProvider::resolve(const RadarSettings &settings, const GpsReceiver &gps,
                                    double *lat, double *lon, bool *using_gps)
{
    if (lat) {
        *lat = settings.center_lat;
    }
    if (lon) {
        *lon = settings.center_lon;
    }
    if (using_gps) {
        *using_gps = false;
    }

    if (settings.center_source == RADAR_CENTER_SOURCE_AIRPORT) {
        if (lat) {
            *lat = settings.center_airport_lat;
        }
        if (lon) {
            *lon = settings.center_airport_lon;
        }
        return true;
    }

    if (settings.center_source == RADAR_CENTER_SOURCE_LOCATION) {
        if (lat) {
            *lat = settings.center_location_lat;
        }
        if (lon) {
            *lon = settings.center_location_lon;
        }
        return true;
    }

    if (settings.center_source != RADAR_CENTER_SOURCE_GPS) {
        return false;
    }

    double gps_lat = 0.0;
    double gps_lon = 0.0;
    if (!gps.getFix(&gps_lat, &gps_lon)) {
        return false;
    }

    if (lat) {
        *lat = gps_lat;
    }
    if (lon) {
        *lon = gps_lon;
    }
    if (using_gps) {
        *using_gps = true;
    }
    return true;
}
