#include "RadarGeometry.hpp"

#include <math.h>
#include <stdlib.h>

/* Return the smallest absolute angular difference between two bearings. */
int RadarGeometry::angleDelta(int a, int b)
{
    int d = abs(a - b) % 360;
    return d > 180 ? 360 - d : d;
}

/* Convert degrees to radians. */
double RadarGeometry::degToRad(double degrees)
{
    return degrees * PI_D / 180.0;
}

/* Convert radians to degrees. */
double RadarGeometry::radToDeg(double radians)
{
    return radians * 180.0 / PI_D;
}

/* Calculate great-circle distance between two coordinates in statute miles. */
double RadarGeometry::distanceMiles(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = degToRad(lat2 - lat1);
    double dlon = degToRad(lon2 - lon1);
    double rlat1 = degToRad(lat1);
    double rlat2 = degToRad(lat2);
    double a = sin(dlat / 2.0) * sin(dlat / 2.0) +
               cos(rlat1) * cos(rlat2) * sin(dlon / 2.0) * sin(dlon / 2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return 3958.8 * c;
}

/* Calculate the initial true bearing from one coordinate to another. */
int RadarGeometry::bearingDegrees(double lat1, double lon1, double lat2, double lon2)
{
    double rlat1 = degToRad(lat1);
    double rlat2 = degToRad(lat2);
    double dlon = degToRad(lon2 - lon1);
    double y = sin(dlon) * cos(rlat2);
    double x = cos(rlat1) * sin(rlat2) - sin(rlat1) * cos(rlat2) * cos(dlon);
    int bearing = (int)lround(fmod(radToDeg(atan2(y, x)) + 360.0, 360.0));
    return bearing == 360 ? 0 : bearing;
}

/* Clamp an integer to an inclusive range. */
int RadarGeometry::clampInt(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}
