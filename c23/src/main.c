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

#define _GNU_SOURCE

#include "abraxas.h"
#include "config.h"
#include "daemon.h"
#include "sigmoid.h"
#include "solar.h"
#include "uring.h"
#include "weather.h"
#include "zipdb.h"

#include <getopt.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <meridian.h>

/* --- Status display --- */

static void cmd_status(double lat, double lon, const abraxas_paths_t *paths)
{
    printf("ABRAXAS v5.1.0 [C23]\n\n");
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
        printf("Weather: Not available\n");
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

    if (config_check_daemon_alive(paths))
        printf("Daemon will process on next tick (up to 60s).\n");
    else
        fprintf(stderr, "[warn] Daemon is not running. Override saved but won't apply until daemon starts.\n");
    return 0;
}

/* --- Resume solar control --- */

static int cmd_resume(const abraxas_paths_t *paths)
{
    override_state_t ovr = { .active = false };
    config_save_override(paths, &ovr);

    if (config_check_daemon_alive(paths))
        printf("Resume sent. Daemon will return to solar control.\n");
    else
        fprintf(stderr, "[warn] Daemon is not running. Resume saved but won't apply until daemon starts.\n");
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

/* --- Benchmark --- */

static inline uint64_t bench_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void bench_print(const char *label, uint64_t ns, int iterations)
{
    if (iterations > 1) {
        uint64_t per = ns / (uint64_t)iterations;
        if (ns >= 1000000)
            printf("  %-30s %8lu us  (%lu ns/call, %d calls)\n",
                   label, ns / 1000, per, iterations);
        else if (ns >= 1000)
            printf("  %-30s %8lu us  (%lu ns/call, %d calls)\n",
                   label, ns / 1000, per, iterations);
        else
            printf("  %-30s %8lu ns  (%lu ns/call, %d calls)\n",
                   label, ns, per, iterations);
    } else {
        if (ns >= 1000000)
            printf("  %-30s %8lu us\n", label, ns / 1000);
        else if (ns >= 1000)
            printf("  %-30s %8lu us\n", label, ns / 1000);
        else
            printf("  %-30s %8lu ns\n", label, ns);
    }
}

static int cmd_benchmark(const abraxas_paths_t *paths)
{
    printf("ABRAXAS v5.1.0 [C23] -- Kernel-grade benchmark\n");
    printf("Clock: CLOCK_MONOTONIC_RAW (hardware TSC)\n\n");

    constexpr int N = 1000;
    uint64_t t0, t1;

    /* config_init_paths */
    t0 = bench_ns();
    for (int i = 0; i < N; i++) {
        abraxas_paths_t p;
        config_init_paths(&p);
    }
    t1 = bench_ns();
    bench_print("config_init_paths()", t1 - t0, N);

    /* config_load_location */
    t0 = bench_ns();
    for (int i = 0; i < N; i++) {
        (void)config_load_location(paths);
    }
    t1 = bench_ns();
    bench_print("config_load_location()", t1 - t0, N);

    /* Load location for solar tests */
    location_t loc = config_load_location(paths);
    if (!loc.valid) {
        loc.lat = 34.26;
        loc.lon = -88.38;
    }
    time_t now = time(nullptr);

    /* solar_sunrise_sunset */
    t0 = bench_ns();
    for (int i = 0; i < N; i++) {
        volatile sun_times_t st = solar_sunrise_sunset(now, loc.lat, loc.lon);
        (void)st;
    }
    t1 = bench_ns();
    bench_print("solar_sunrise_sunset()", t1 - t0, N);

    /* solar_position */
    t0 = bench_ns();
    for (int i = 0; i < N; i++) {
        volatile sun_position_t sp = solar_position(now, loc.lat, loc.lon);
        (void)sp;
    }
    t1 = bench_ns();
    bench_print("solar_position()", t1 - t0, N);

    /* calculate_solar_temp */
    sun_times_t st = solar_sunrise_sunset(now, loc.lat, loc.lon);
    double min_from_sr = st.valid ? difftime(now, st.sunrise) / 60.0 : 0.0;
    double min_to_ss   = st.valid ? difftime(st.sunset, now)  / 60.0 : 0.0;

    t0 = bench_ns();
    for (int i = 0; i < N; i++) {
        volatile int t = calculate_solar_temp(min_from_sr, min_to_ss, false);
        (void)t;
    }
    t1 = bench_ns();
    bench_print("calculate_solar_temp()", t1 - t0, N);

    /* sigmoid_norm */
    t0 = bench_ns();
    for (int i = 0; i < N; i++) {
        volatile double s = sigmoid_norm(0.5, SIGMOID_STEEPNESS);
        (void)s;
    }
    t1 = bench_ns();
    bench_print("sigmoid_norm()", t1 - t0, N);

    /* JSON parse (override) */
    override_state_t ovr = config_load_override(paths);
    (void)ovr;
    t0 = bench_ns();
    for (int i = 0; i < N; i++) {
        (void)config_load_override(paths);
    }
    t1 = bench_ns();
    bench_print("config_load_override()", t1 - t0, N);

    /* JSON parse (weather cache) */
    t0 = bench_ns();
    for (int i = 0; i < N; i++) {
        (void)config_load_weather_cache(paths);
    }
    t1 = bench_ns();
    bench_print("config_load_weather_cache()", t1 - t0, N);

    printf("\nKernel facilities:\n");

    /* io_uring setup + teardown */
    {
        abraxas_ring_t ring;
        t0 = bench_ns();
        bool ok = uring_init(&ring, 8);
        t1 = bench_ns();
        if (ok) {
            bench_print("io_uring_setup()", t1 - t0, 1);
            uring_destroy(&ring);
        } else {
            printf("  %-30s unavailable\n", "io_uring_setup()");
        }
    }

    /* seccomp (can't actually install in benchmark -- would restrict us) */
    printf("  %-30s (not measured -- would restrict process)\n", "seccomp_install()");
    printf("  %-30s (not measured -- would restrict process)\n", "landlock_install()");

    printf("\nDone.\n");
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
    printf("  --benchmark           Nanosecond performance benchmark\n");
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
    { "benchmark",    no_argument,       nullptr, 'B' },
    { "help",         no_argument,       nullptr, 'h' },
    { nullptr, 0, nullptr, 0 }
};

int main(int argc, char **argv)
{
    abraxas_paths_t paths;
    if (!config_init_paths(&paths)) {
        fprintf(stderr, "Failed to initialize paths (is $HOME set?)\n");
        return 1;
    }

    enum { CMD_DAEMON, CMD_STATUS, CMD_SET_LOC, CMD_REFRESH,
           CMD_SET_TEMP, CMD_RESUME, CMD_RESET, CMD_BENCHMARK } command = CMD_DAEMON;
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
        case 'S': {
            command = CMD_SET_TEMP;
            char *end;
            errno = 0;
            long val = strtol(optarg, &end, 10);
            if (*end != '\0' || end == optarg || errno == ERANGE
                || val < INT_MIN || val > INT_MAX) {
                fprintf(stderr, "Invalid temperature: %s\n", optarg);
                usage();
                return 1;
            }
            set_temp_val = (int)val;
            /* Check for optional duration in next arg */
            if (optind < argc && argv[optind][0] != '-') {
                errno = 0;
                val = strtol(argv[optind], &end, 10);
                if (*end != '\0' || end == argv[optind] || errno == ERANGE
                    || val < 0 || val > INT_MAX) {
                    fprintf(stderr, "Invalid duration: %s\n", argv[optind]);
                    usage();
                    return 1;
                }
                set_temp_dur = (int)val;
                optind++;
            }
            break;
        }
        case 'R': command = CMD_RESUME;   break;
        case 'x': command = CMD_RESET;    break;
        case 'B': command = CMD_BENCHMARK; break;
        case 'h': usage(); return 0;
        default:  usage(); return 1;
        }
    }

    /* Commands that don't need location */
    if (command == CMD_RESET)
        return cmd_reset(&paths);
    if (command == CMD_RESUME)
        return cmd_resume(&paths);
    if (command == CMD_SET_LOC)
        return cmd_set_location(loc_arg, &paths);
    if (command == CMD_SET_TEMP)
        return cmd_set_temp(set_temp_val, set_temp_dur, &paths);
    if (command == CMD_BENCHMARK)
        return cmd_benchmark(&paths);

    /* Remaining commands need location */
    location_t loc = config_load_location(&paths);
    if (!loc.valid) {
        fprintf(stderr, "No location configured. Use --set-location first.\n");
        fprintf(stderr, "  Example: abraxas --set-location 60614\n");
        fprintf(stderr, "  Example: abraxas --set-location 41.88,-87.63\n");
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

    return result;
}
