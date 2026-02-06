/*
 * config.h - Configuration, override state, and path resolution
 *
 * INI config for location. JSON for override state and weather cache.
 * Path resolution from $HOME.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "abraxas.h"

/* Initialize all paths from $HOME. Creates config dir if needed.
   Returns false on failure ($HOME not set, mkdir failed). */
bool config_init_paths(abraxas_paths_t *paths);

/* Load location from config.ini. Returns location_t with valid=false if missing. */
location_t config_load_location(const abraxas_paths_t *paths);

/* Save location to config.ini. Returns false on write failure. */
bool config_save_location(const abraxas_paths_t *paths, double lat, double lon);

/* Load override state from override.json. Returns with active=false if missing/invalid. */
override_state_t config_load_override(const abraxas_paths_t *paths);

/* Save override state to override.json. */
bool config_save_override(const abraxas_paths_t *paths, const override_state_t *ovr);

/* Delete override.json. */
void config_clear_override(const abraxas_paths_t *paths);

/* Load cached weather data. Returns with has_error=true if missing/invalid. */
weather_data_t config_load_weather_cache(const abraxas_paths_t *paths);

/* Save weather cache. */
bool config_save_weather_cache(const abraxas_paths_t *paths, const weather_data_t *wd);

/* Check if weather cache needs refresh. */
bool config_weather_needs_refresh(const weather_data_t *wd);

/* Check if daemon process is alive via PID file. */
bool config_check_daemon_alive(const abraxas_paths_t *paths);

/* Write daemon PID to PID file. */
bool config_write_pid(const abraxas_paths_t *paths);

/* Remove daemon PID file. */
void config_remove_pid(const abraxas_paths_t *paths);

#endif /* CONFIG_H */
