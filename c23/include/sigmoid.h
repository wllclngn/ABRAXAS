/*
 * sigmoid.h - Sigmoid transition math
 *
 * Normalized sigmoid maps [-1,1] to exact [0,1].
 * Dusk is the canonical transition (day->night). Dawn is its inverse.
 */

#ifndef SIGMOID_H
#define SIGMOID_H

#include <time.h>

/* Sigmoid normalized to exactly [0,1] over [-1,1]. */
double sigmoid_norm(double x, double steepness);

/* Calculate solar-based color temperature (Kelvin). */
int calculate_solar_temp(double minutes_from_sunrise, double minutes_to_sunset,
                         bool is_dark_mode);

/* Calculate manual override temperature during sigmoid transition. */
int calculate_manual_temp(int start_temp, int target_temp,
                          time_t start_time, int duration_min, time_t now);

/* Calculate when manual override should auto-resume solar control.
   Returns epoch time 15 minutes before next transition window. */
time_t next_transition_resume(time_t now, double lat, double lon);

#endif /* SIGMOID_H */
