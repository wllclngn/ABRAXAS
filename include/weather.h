/*
 * weather.h - NOAA weather API client
 *
 * This module owns the entire NOAA/libcurl lifecycle. When NOAA_DISABLED
 * is defined (non-US builds), init/cleanup are no-ops and fetch returns
 * has_error=true. No other module touches libcurl.
 */

#ifndef WEATHER_H
#define WEATHER_H

#include "abraxas.h"

/* Initialize weather subsystem (curl). No-op when NOAA_DISABLED. */
void weather_init(void);

/* Shut down weather subsystem (curl). No-op when NOAA_DISABLED. */
void weather_cleanup(void);

/* Fetch current weather from NOAA api.weather.gov.
   Fills weather_data_t with cloud_cover, forecast, temperature, is_day.
   Sets has_error=true on network/parse failure (or when NOAA_DISABLED). */
weather_data_t weather_fetch(double lat, double lon);

#endif /* WEATHER_H */
