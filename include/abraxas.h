/*
 * abraxas.h - Central header for ABRAXAS daemon
 *
 * Constants, path resolution, and shared types used by every module.
 */

#ifndef ABRAXAS_H
#define ABRAXAS_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Temperature bounds (Kelvin, match libmeridian) */
constexpr int TEMP_MIN       = 1000;
constexpr int TEMP_MAX       = 25000;

/* Temperature targets */
constexpr int TEMP_DAY_CLEAR = 6500;
constexpr int TEMP_DAY_DARK  = 4500;
constexpr int TEMP_NIGHT     = 2900;

/* Cloud threshold -- % cloud cover that triggers dark mode */
constexpr int CLOUD_THRESHOLD = 75;

/* Timing */
constexpr int WEATHER_REFRESH_SEC = 900;   /* 15 minutes */
constexpr int TEMP_UPDATE_SEC     = 60;    /* 1 minute   */

/* Transition windows (minutes) */
constexpr int DAWN_DURATION = 90;
constexpr int DUSK_DURATION = 120;

/* Sigmoid steepness for transitions */
constexpr double SIGMOID_STEEPNESS = 6.0;

/* Path buffer size */
#define ABRAXAS_PATH_MAX 512

/* Resolved filesystem paths */
typedef struct {
    char config_dir[ABRAXAS_PATH_MAX];     /* ~/.config/abraxas         */
    char config_file[ABRAXAS_PATH_MAX];    /* ~/.config/abraxas/config.ini */
    char cache_file[ABRAXAS_PATH_MAX];     /* ~/.config/abraxas/weather_cache.json */
    char override_file[ABRAXAS_PATH_MAX];  /* ~/.config/abraxas/override.json */
    char zipdb_file[ABRAXAS_PATH_MAX];     /* ~/.config/abraxas/us_zipcodes.bin */
} abraxas_paths_t;

/* Geographic location */
typedef struct {
    double lat;
    double lon;
    bool   valid;
} location_t;

/* Cached weather data */
typedef struct {
    int     cloud_cover;           /* 0-100 % */
    char    forecast[128];         /* short description */
    double  temperature;           /* F from NOAA */
    bool    is_day;
    time_t  fetched_at;            /* epoch seconds */
    bool    has_error;
} weather_data_t;

/* Manual override state (persisted to override.json) */
typedef struct {
    bool    active;
    int     target_temp;           /* Kelvin */
    int     duration_minutes;
    time_t  issued_at;             /* epoch seconds */
    int     start_temp;            /* Kelvin at moment of override */
} override_state_t;

/* Full daemon runtime state */
typedef struct {
    location_t       location;
    weather_data_t   weather;
    override_state_t override;
    abraxas_paths_t  paths;

    /* Manual mode tracking */
    bool    manual_mode;
    int     manual_start_temp;
    int     manual_target_temp;
    time_t  manual_start_time;
    int     manual_duration_min;
    time_t  manual_issued_at;
    time_t  manual_resume_time;

    /* Last applied temperature */
    int     last_temp;
    bool    last_temp_valid;
} daemon_state_t;

#endif /* ABRAXAS_H */
