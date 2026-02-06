/*
 * sigmoid.c - Sigmoid transition math
 *
 * Dusk is canonical: day -> night over DUSK_DURATION centered on sunset.
 * Dawn is its inverse: night -> day over DAWN_DURATION centered on sunrise.
 * Manual overrides use the same sigmoid over [0, duration].
 */

#include "sigmoid.h"
#include "abraxas.h"
#include "solar.h"

#include <math.h>

static double sigmoid_raw(double x, double steepness)
{
    return 1.0 / (1.0 + exp(-steepness * x));
}

double sigmoid_norm(double x, double steepness)
{
    double raw  = sigmoid_raw(x, steepness);
    double low  = sigmoid_raw(-1.0, steepness);
    double high = sigmoid_raw(1.0, steepness);
    return (raw - low) / (high - low);
}

int calculate_solar_temp(double minutes_from_sunrise, double minutes_to_sunset,
                         bool is_dark_mode)
{
    int day_temp   = is_dark_mode ? TEMP_DAY_DARK : TEMP_DAY_CLEAR;
    int night_temp = TEMP_NIGHT;

    double dawn_half = DAWN_DURATION / 2.0;
    double dusk_half = DUSK_DURATION / 2.0;

    /* Dawn: night -> day (inverse of dusk) */
    if (fabs(minutes_from_sunrise) < dawn_half) {
        double x = minutes_from_sunrise / dawn_half;     /* [-1, 1] */
        double factor = sigmoid_norm(x, SIGMOID_STEEPNESS);
        return (int)(night_temp + (day_temp - night_temp) * factor);
    }

    /* Dusk: day -> night (canonical) */
    if (fabs(minutes_to_sunset) < dusk_half) {
        double x = minutes_to_sunset / dusk_half;        /* [1, -1] */
        double factor = sigmoid_norm(x, SIGMOID_STEEPNESS);
        return (int)(night_temp + (day_temp - night_temp) * factor);
    }

    /* Daytime (between windows) */
    if (minutes_from_sunrise >= dawn_half && minutes_to_sunset >= dusk_half)
        return day_temp;

    /* Night */
    return night_temp;
}

int calculate_manual_temp(int start_temp, int target_temp,
                          time_t start_time, int duration_min, time_t now)
{
    if (duration_min <= 0)
        return target_temp;

    double elapsed_min = difftime(now, start_time) / 60.0;

    if (elapsed_min >= (double)duration_min)
        return target_temp;

    /* Map [0, duration] -> [-1, 1] */
    double x = 2.0 * (elapsed_min / (double)duration_min) - 1.0;
    double factor = sigmoid_norm(x, SIGMOID_STEEPNESS);
    return (int)(start_temp + (target_temp - start_temp) * factor);
}

time_t next_transition_resume(time_t now, double lat, double lon)
{
    sun_times_t st = solar_sunrise_sunset(now, lat, lon);
    if (!st.valid) return now + 86400; /* polar fallback: 24h */

    time_t dawn_window_start = st.sunrise - (time_t)(DAWN_DURATION / 2) * 60;
    time_t dusk_window_start = st.sunset  - (time_t)(DUSK_DURATION / 2) * 60;

    time_t resume_dawn = dawn_window_start - 15 * 60;
    time_t resume_dusk = dusk_window_start - 15 * 60;

    /* Find earliest future candidate */
    time_t best = 0;
    if (resume_dawn > now) best = resume_dawn;
    if (resume_dusk > now && (best == 0 || resume_dusk < best)) best = resume_dusk;

    if (best > 0) return best;

    /* Both today's transitions passed -- use tomorrow's dawn */
    time_t tomorrow = now + 86400;
    sun_times_t st2 = solar_sunrise_sunset(tomorrow, lat, lon);
    if (!st2.valid) return now + 86400;
    return st2.sunrise - (time_t)(DAWN_DURATION / 2 + 15) * 60;
}
