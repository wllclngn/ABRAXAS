/*
 * main.c - ABRAXAS CLI entry point
 *
 * Commands:
 *   --daemon         Run as daemon (default)
 *   --status         Show current status
 *   --set-location   Set location (ZIP or lat,lon)
 *   --refresh        Force weather refresh
 *   --set TEMP [MIN] Manual override to TEMP over MIN minutes
 *   --resume         Clear manual override
 *   --reset          Restore gamma and exit
 *   --help           Show usage
 */

#include "abraxas.h"
#include "config.h"
#include "daemon.h"
#include "sigmoid.h"
#include "solar.h"
#include "weather.h"
#include "zipdb.h"

#include <curl/curl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <meridian.h>

/* --- Status display --- */

static void cmd_status(double lat, double lon, const abraxas_paths_t *paths)
{
    printf("Location: %.4f, %.4f\n\n", lat, lon);

    time_t now = time(nullptr);
    sun_times_t st = solar_sunrise_sunset(now, lat, lon);
    sun_position_t sp = solar_position(now, lat, lon);

    struct tm nt, srt, sst;
    localtime_r(&now, &nt);
    localtime_r(&st.sunrise, &srt);
    localtime_r(&st.sunset, &sst);

    printf("Date: %04d-%02d-%02d %02d:%02d:%02d\n",
           nt.tm_year + 1900, nt.tm_mon + 1, nt.tm_mday,
           nt.tm_hour, nt.tm_min, nt.tm_sec);

    if (st.valid) {
        printf("Sunrise: %02d:%02d\n", srt.tm_hour, srt.tm_min);
        printf("Sunset: %02d:%02d\n", sst.tm_hour, sst.tm_min);
    } else {
        printf("Sunrise/Sunset: N/A (polar region)\n");
    }
    printf("Sun elevation: %.1f degrees\n\n", sp.elevation);

    /* Weather */
    weather_data_t weather = config_load_weather_cache(paths);
    if (!weather.has_error) {
        printf("Weather: %s\n", weather.forecast);
        printf("Cloud cover: %d%%\n", weather.cloud_cover);

        struct tm ft;
        localtime_r(&weather.fetched_at, &ft);
        printf("Last updated: %04d-%02d-%02d %02d:%02d:%02d\n",
               ft.tm_year + 1900, ft.tm_mon + 1, ft.tm_mday,
               ft.tm_hour, ft.tm_min, ft.tm_sec);
    } else {
        printf("Weather: Not available (run daemon to fetch)\n");
    }
    printf("\n");

    /* Override status */
    override_state_t ovr = config_load_override(paths);
    if (ovr.active) {
        printf("Mode: MANUAL OVERRIDE\n");
        printf("Target: %dK over %d min\n", ovr.target_temp, ovr.duration_minutes);

        struct tm it;
        localtime_r(&ovr.issued_at, &it);
        printf("Issued: %04d-%02d-%02d %02d:%02d:%02d\n",
               it.tm_year + 1900, it.tm_mon + 1, it.tm_mday,
               it.tm_hour, it.tm_min, it.tm_sec);
    } else {
        bool is_dark = !weather.has_error && weather.cloud_cover >= CLOUD_THRESHOLD;
        double min_from_sunrise = st.valid ? difftime(now, st.sunrise) / 60.0 : 0.0;
        double min_to_sunset    = st.valid ? difftime(st.sunset, now) / 60.0 : 0.0;

        int temp = calculate_solar_temp(min_from_sunrise, min_to_sunset, is_dark);

        printf("Mode: %s\n", is_dark ? "DARK" : "CLEAR");
        printf("Target temperature: %dK\n", temp);
    }
}

/* --- Set location --- */

static int cmd_set_location(const char *loc_str, const abraxas_paths_t *paths)
{
    /* Check for lat,lon format */
    if (strchr(loc_str, ',')) {
        double lat, lon;
        if (sscanf(loc_str, "%lf,%lf", &lat, &lon) != 2) {
            fprintf(stderr, "Invalid format. Use: LAT,LON (e.g., 41.88,-87.63)\n");
            return 1;
        }
        if (!config_save_location(paths, lat, lon)) {
            fprintf(stderr, "Failed to save config\n");
            return 1;
        }
        printf("Location set to: %.4f, %.4f\n", lat, lon);
        return 0;
    }

    /* ZIP code */
    size_t len = strlen(loc_str);
    bool all_digits = true;
    for (size_t i = 0; i < len; i++) {
        if (loc_str[i] < '0' || loc_str[i] > '9') { all_digits = false; break; }
    }

    if (!all_digits || len != 5) {
        fprintf(stderr, "Invalid ZIP code. Must be 5 digits.\n");
        return 1;
    }

    printf("Looking up ZIP code %s...\n", loc_str);
    float lat, lon;
    if (!zipdb_lookup(paths->zipdb_file, loc_str, &lat, &lon)) {
        fprintf(stderr, "ZIP code %s not found in database.\n", loc_str);
        return 1;
    }

    printf("Found: %s -> %.4f, %.4f\n", loc_str, lat, lon);
    if (!config_save_location(paths, (double)lat, (double)lon)) {
        fprintf(stderr, "Failed to save config\n");
        return 1;
    }
    printf("Location set to: %.4f, %.4f\n", lat, lon);
    return 0;
}

/* --- Refresh weather --- */

static int cmd_refresh(double lat, double lon, const abraxas_paths_t *paths)
{
    printf("Fetching weather...\n");
    weather_data_t wd = weather_fetch(lat, lon);

    if (wd.has_error) {
        fprintf(stderr, "Weather fetch failed\n");
        return 1;
    }

    config_save_weather_cache(paths, &wd);
    printf("Weather: %s\n", wd.forecast);
    printf("Cloud cover: %d%%\n", wd.cloud_cover);
    return 0;
}

/* --- Set temperature override --- */

static int cmd_set_temp(int target_temp, int duration_min,
                        const abraxas_paths_t *paths)
{
    if (target_temp < TEMP_MIN || target_temp > TEMP_MAX) {
        fprintf(stderr, "Temperature must be between %dK and %dK.\n", TEMP_MIN, TEMP_MAX);
        return 1;
    }

    override_state_t ovr = {
        .active = true,
        .target_temp = target_temp,
        .duration_minutes = duration_min,
        .issued_at = time(nullptr),
        .start_temp = 0  /* daemon fills this */
    };

    if (!config_save_override(paths, &ovr)) {
        fprintf(stderr, "Failed to write override\n");
        return 1;
    }

    if (duration_min > 0)
        printf("Override: -> %dK over %d min (sigmoid)\n", target_temp, duration_min);
    else
        printf("Override: -> %dK (instant)\n", target_temp);
    printf("Daemon will process on next tick (up to 60s).\n");
    return 0;
}

/* --- Resume solar control --- */

static int cmd_resume(const abraxas_paths_t *paths)
{
    override_state_t ovr = { .active = false };
    config_save_override(paths, &ovr);
    printf("Resume sent. Daemon will return to solar control.\n");
    return 0;
}

/* --- Reset gamma --- */

static int cmd_reset(const abraxas_paths_t *paths)
{
    config_clear_override(paths);

    meridian_state_t *state = nullptr;
    meridian_error_t err = meridian_init(&state);
    if (err == MERIDIAN_OK) {
        (void)meridian_restore(state);
        meridian_free(state);
    }

    printf("Screen temperature reset.\n");
    return 0;
}

/* --- Usage --- */

static void usage(void)
{
    printf("ABRAXAS - Dynamic color temperature daemon with weather awareness\n\n");
    printf("Usage: abraxas [OPTIONS]\n\n");
    printf("Options:\n");
    printf("  --daemon              Run as daemon (default)\n");
    printf("  --status              Show current status\n");
    printf("  --set-location LOC    Set location (ZIP code or LAT,LON)\n");
    printf("  --refresh             Force weather refresh\n");
    printf("  --set TEMP [MIN]      Override to TEMP (Kelvin) over MIN minutes (default 3)\n");
    printf("  --resume              Clear override, resume solar control\n");
    printf("  --reset               Restore gamma and exit\n");
    printf("  --help                Show this help\n");
}

/* --- Entry point --- */

static struct option long_opts[] = {
    { "daemon",       no_argument,       nullptr, 'd' },
    { "status",       no_argument,       nullptr, 's' },
    { "set-location", required_argument, nullptr, 'l' },
    { "refresh",      no_argument,       nullptr, 'r' },
    { "set",          required_argument, nullptr, 'S' },
    { "resume",       no_argument,       nullptr, 'R' },
    { "reset",        no_argument,       nullptr, 'x' },
    { "help",         no_argument,       nullptr, 'h' },
    { nullptr, 0, nullptr, 0 }
};

int main(int argc, char **argv)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);

    abraxas_paths_t paths;
    if (!config_init_paths(&paths)) {
        fprintf(stderr, "Failed to initialize paths (is $HOME set?)\n");
        curl_global_cleanup();
        return 1;
    }

    enum { CMD_DAEMON, CMD_STATUS, CMD_SET_LOC, CMD_REFRESH,
           CMD_SET_TEMP, CMD_RESUME, CMD_RESET } command = CMD_DAEMON;
    const char *loc_arg = nullptr;
    int set_temp_val = 0;
    int set_temp_dur = 3;

    int opt;
    while ((opt = getopt_long(argc, argv, "", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 'd': command = CMD_DAEMON;   break;
        case 's': command = CMD_STATUS;   break;
        case 'l': command = CMD_SET_LOC;  loc_arg = optarg; break;
        case 'r': command = CMD_REFRESH;  break;
        case 'S':
            command = CMD_SET_TEMP;
            set_temp_val = atoi(optarg);
            /* Check for optional duration in next arg */
            if (optind < argc && argv[optind][0] != '-')
                set_temp_dur = atoi(argv[optind++]);
            break;
        case 'R': command = CMD_RESUME;   break;
        case 'x': command = CMD_RESET;    break;
        case 'h': usage(); curl_global_cleanup(); return 0;
        default:  usage(); curl_global_cleanup(); return 1;
        }
    }

    /* Commands that don't need location */
    if (command == CMD_RESET) {
        int r = cmd_reset(&paths);
        curl_global_cleanup();
        return r;
    }
    if (command == CMD_RESUME) {
        int r = cmd_resume(&paths);
        curl_global_cleanup();
        return r;
    }
    if (command == CMD_SET_LOC) {
        int r = cmd_set_location(loc_arg, &paths);
        curl_global_cleanup();
        return r;
    }

    /* Remaining commands need location */
    location_t loc = config_load_location(&paths);
    if (!loc.valid) {
        fprintf(stderr, "No location configured. Use --set-location first.\n");
        fprintf(stderr, "  Example: abraxas --set-location 60614\n");
        fprintf(stderr, "  Example: abraxas --set-location 41.88,-87.63\n");
        curl_global_cleanup();
        return 1;
    }

    int result = 0;
    switch (command) {
    case CMD_STATUS:
        cmd_status(loc.lat, loc.lon, &paths);
        break;
    case CMD_REFRESH:
        result = cmd_refresh(loc.lat, loc.lon, &paths);
        break;
    case CMD_SET_TEMP:
        result = cmd_set_temp(set_temp_val, set_temp_dur, &paths);
        break;
    case CMD_DAEMON: {
        daemon_state_t state = {
            .location = loc,
            .paths = paths,
            .last_temp_valid = false
        };
        daemon_run(&state);
        break;
    }
    default:
        break;
    }

    curl_global_cleanup();
    return result;
}
