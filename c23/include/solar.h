/*
 * solar.h - NOAA sun position and sunrise/sunset calculations
 *
 * All computations are offline -- no network access.
 * Based on NOAA solar equations (Jean Meeus, Astronomical Algorithms).
 */

#ifndef SOLAR_H
#define SOLAR_H

#include <stdbool.h>
#include <time.h>

/* Sun elevation at a given time and location */
typedef struct {
    double elevation;   /* degrees above horizon (negative = below) */
} sun_position_t;

/* Sunrise and sunset times */
typedef struct {
    time_t sunrise;     /* epoch seconds */
    time_t sunset;      /* epoch seconds */
    bool   valid;       /* false if polar (no rise/set) */
} sun_times_t;

/* Calculate sun elevation angle. */
sun_position_t solar_position(time_t when, double lat, double lon);

/* Calculate sunrise and sunset for the date containing 'when'. */
sun_times_t solar_sunrise_sunset(time_t when, double lat, double lon);

#endif /* SOLAR_H */
