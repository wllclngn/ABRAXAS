/*
 * weather.h - NOAA weather API client
 *
 * Two-step API: points/{lat},{lon} -> forecastHourly URL -> parse first period.
 * Cloud cover derived from forecast keyword heuristic.
 * Uses libcurl for HTTP and json.h for response parsing.
 */

#ifndef WEATHER_H
#define WEATHER_H

#include "abraxas.h"

/* Fetch current weather from NOAA api.weather.gov.
   Fills weather_data_t with cloud_cover, forecast, temperature, is_day.
   Sets has_error=true on network/parse failure. */
weather_data_t weather_fetch(double lat, double lon);

#endif /* WEATHER_H */
