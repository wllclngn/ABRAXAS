/*
 * weather.h - NOAA weather API client
 *
 * HTTP via exec'ing curl(1) -- no libcurl linkage. When NOAA_DISABLED
 * is defined (non-US builds), init/cleanup are no-ops and fetch returns
 * has_error=true.
 */

#ifndef WEATHER_H
#define WEATHER_H

#include "abraxas.h"

/* No-op (kept for API compatibility). */
void weather_init(void);

/* No-op (kept for API compatibility). */
void weather_cleanup(void);

/* Fetch current weather from NOAA api.weather.gov.
   Fills weather_data_t with cloud_cover, forecast, temperature, is_day.
   Sets has_error=true on network/parse failure (or when NOAA_DISABLED). */
weather_data_t weather_fetch(double lat, double lon);

#endif /* WEATHER_H */
