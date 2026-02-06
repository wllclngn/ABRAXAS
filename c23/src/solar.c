/*
 * solar.c - NOAA sun position and sunrise/sunset
 *
 * Port of the Python NOAA solar equations from the original ABRAXAS daemon.
 * Julian day -> Julian century -> geometric mean longitude/anomaly ->
 * equation of center -> apparent longitude -> declination -> hour angle.
 */

#define _GNU_SOURCE  /* tm_gmtoff */

#include "solar.h"

#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double deg2rad(double d) { return d * M_PI / 180.0; }
static double rad2deg(double r) { return r * 180.0 / M_PI; }

/* Timezone offset in hours from UTC (via kernel localtime). */
static double get_tz_offset_hours(void)
{
    time_t t = time(nullptr);
    struct tm local;
    localtime_r(&t, &local);
    return (double)local.tm_gmtoff / 3600.0;
}

/*
 * Julian Day from broken-down time.
 * hour_frac is fractional hours (h + m/60 + s/3600).
 */
static double julian_day(int year, int month, int day, double hour_frac)
{
    if (month <= 2) {
        year -= 1;
        month += 12;
    }

    int A = year / 100;
    int B = 2 - A + A / 4;

    double jd = (int)(365.25 * (year + 4716))
              + (int)(30.6001 * (month + 1))
              + day + B - 1524.5;
    jd += hour_frac / 24.0;
    return jd;
}

/*
 * Shared NOAA solar parameters from Julian century.
 * Factored out to avoid duplication between position and sunrise/sunset.
 */
typedef struct {
    double L0;              /* geometric mean longitude (deg)     */
    double M;               /* geometric mean anomaly (deg)       */
    double e;               /* eccentricity of orbit              */
    double sun_declin;      /* solar declination (deg)            */
    double eq_time;         /* equation of time (minutes)         */
    double obliq_corr;      /* corrected obliquity (deg)          */
} solar_params_t;

static solar_params_t compute_solar_params(double jc)
{
    solar_params_t sp;

    /* Geometric mean longitude of sun */
    sp.L0 = fmod(280.46646 + jc * (36000.76983 + 0.0003032 * jc), 360.0);

    /* Geometric mean anomaly */
    sp.M = 357.52911 + jc * (35999.05029 - 0.0001537 * jc);
    double M_rad = deg2rad(sp.M);

    /* Eccentricity of Earth's orbit */
    sp.e = 0.016708634 - jc * (0.000042037 + 0.0000001267 * jc);

    /* Sun's equation of center */
    double C = sin(M_rad) * (1.914602 - jc * (0.004817 + 0.000014 * jc))
             + sin(2.0 * M_rad) * (0.019993 - 0.000101 * jc)
             + sin(3.0 * M_rad) * 0.000289;

    /* Sun's true and apparent longitude */
    double sun_lon = sp.L0 + C;
    double omega = 125.04 - 1934.136 * jc;
    double sun_apparent_lon = sun_lon - 0.00569 - 0.00478 * sin(deg2rad(omega));

    /* Mean obliquity and correction */
    double obliq_mean = 23.0 + (26.0 + (21.448 - jc * (46.815 + jc * (0.00059 - jc * 0.001813))) / 60.0) / 60.0;
    sp.obliq_corr = obliq_mean + 0.00256 * cos(deg2rad(omega));
    double obliq_corr_rad = deg2rad(sp.obliq_corr);

    /* Solar declination */
    sp.sun_declin = rad2deg(asin(sin(obliq_corr_rad) * sin(deg2rad(sun_apparent_lon))));

    /* Equation of time */
    double var_y = tan(obliq_corr_rad / 2.0);
    var_y *= var_y;
    sp.eq_time = 4.0 * rad2deg(
        var_y * sin(2.0 * deg2rad(sp.L0))
        - 2.0 * sp.e * sin(M_rad)
        + 4.0 * sp.e * var_y * sin(M_rad) * cos(2.0 * deg2rad(sp.L0))
        - 0.5 * var_y * var_y * sin(4.0 * deg2rad(sp.L0))
        - 1.25 * sp.e * sp.e * sin(2.0 * M_rad)
    );

    return sp;
}

sun_position_t solar_position(time_t when, double lat, double lon)
{
    struct tm lt;
    localtime_r(&when, &lt);

    double hour_frac = lt.tm_hour + lt.tm_min / 60.0 + lt.tm_sec / 3600.0;
    double jd = julian_day(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, hour_frac);
    double jc = (jd - 2451545.0) / 36525.0;

    solar_params_t sp = compute_solar_params(jc);

    /* True solar time */
    double tz_offset = get_tz_offset_hours();
    double time_offset = sp.eq_time + 4.0 * lon - 60.0 * tz_offset;
    double tst = lt.tm_hour * 60.0 + lt.tm_min + lt.tm_sec / 60.0 + time_offset;

    /* Hour angle */
    double hour_angle = tst / 4.0 - 180.0;
    if (hour_angle < -180.0) hour_angle += 360.0;

    /* Zenith and elevation */
    double lat_rad = deg2rad(lat);
    double declin_rad = deg2rad(sp.sun_declin);
    double ha_rad = deg2rad(hour_angle);

    double cos_zenith = sin(lat_rad) * sin(declin_rad)
                      + cos(lat_rad) * cos(declin_rad) * cos(ha_rad);

    /* Clamp for acos domain */
    if (cos_zenith > 1.0)  cos_zenith = 1.0;
    if (cos_zenith < -1.0) cos_zenith = -1.0;

    double zenith = rad2deg(acos(cos_zenith));

    return (sun_position_t){ .elevation = 90.0 - zenith };
}

sun_times_t solar_sunrise_sunset(time_t when, double lat, double lon)
{
    struct tm lt;
    localtime_r(&when, &lt);

    /* Use noon of the given day */
    double jd = julian_day(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, 12.0);
    double jc = (jd - 2451545.0) / 36525.0;

    solar_params_t sp = compute_solar_params(jc);

    /* Hour angle for sunrise/sunset (zenith 90.833 degrees) */
    constexpr double zenith = 90.833;
    double lat_rad = deg2rad(lat);
    double declin_rad = deg2rad(sp.sun_declin);

    double cos_ha = cos(deg2rad(zenith)) / (cos(lat_rad) * cos(declin_rad))
                  - tan(lat_rad) * tan(declin_rad);

    /* Polar region check */
    if (cos_ha < -1.0 || cos_ha > 1.0) {
        return (sun_times_t){ .valid = false };
    }

    double ha = rad2deg(acos(cos_ha));
    double tz_offset = get_tz_offset_hours();

    double sunrise_min = 720.0 - 4.0 * (lon + ha) - sp.eq_time + tz_offset * 60.0;
    double sunset_min  = 720.0 - 4.0 * (lon - ha) - sp.eq_time + tz_offset * 60.0;

    /* Base midnight of the given day */
    struct tm base = {
        .tm_year = lt.tm_year, .tm_mon = lt.tm_mon, .tm_mday = lt.tm_mday,
        .tm_hour = 0, .tm_min = 0, .tm_sec = 0, .tm_isdst = -1
    };
    time_t midnight = mktime(&base);

    return (sun_times_t){
        .sunrise = midnight + (time_t)(sunrise_min * 60.0),
        .sunset  = midnight + (time_t)(sunset_min * 60.0),
        .valid   = true
    };
}
