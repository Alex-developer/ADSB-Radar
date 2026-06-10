#pragma once

#include "RadarTypes.hpp"

/*
 * Small, allocation-free geographic helpers used by parsing and drawing code.
 *
 * All bearings are degrees clockwise from true north. Distances are statute
 * miles because the user-facing ranges are configured in miles.
 */
class RadarGeometry {
public:
    /* Smallest signed angular difference between two headings. */
    static int angleDelta(int a, int b);

    /* Convert degrees to radians. */
    static double degToRad(double degrees);

    /* Convert radians to degrees. */
    static double radToDeg(double radians);

    /* Great-circle distance in statute miles. */
    static double distanceMiles(double lat1, double lon1, double lat2, double lon2);

    /* Initial bearing from one coordinate to another, normalised to 0..359. */
    static int bearingDegrees(double lat1, double lon1, double lat2, double lon2);

    /* Clamp an integer without relying on C++ library helpers. */
    static int clampInt(int value, int min_value, int max_value);
};
