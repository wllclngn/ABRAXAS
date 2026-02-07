/*
 * weather.c - NOAA weather API client
 *
 * Two-step API:
 *   1. GET https://api.weather.gov/points/{lat},{lon}
 *      -> extract properties.forecastHourly URL
 *   2. GET that URL
 *      -> extract first period's shortForecast, temperature, isDaytime
 *
 * Cloud cover is derived from forecast keyword heuristic (no direct cloud %
 * in the hourly forecast -- NOAA provides probabilityOfPrecipitation instead).
 *
 * HTTP is handled by exec'ing the curl(1) binary via posix_spawnp.
 * No libcurl linkage -- zero shared library overhead for CLI commands.
 * When NOAA_DISABLED is defined (non-US builds), all three public functions
 * compile to no-ops/stubs.
 */

#define _GNU_SOURCE
#include "weather.h"
#include "json.h"

#ifndef NOAA_DISABLED

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

extern char **environ;

/* Dynamic response buffer */
typedef struct {
    char   *data;
    size_t  size;
    size_t  cap;
} response_buf_t;

static bool buf_grow(response_buf_t *buf, size_t needed)
{
    if (buf->size + needed + 1 <= buf->cap) return true;
    size_t new_cap = (buf->cap ? buf->cap * 2 : 4096);
    while (new_cap < buf->size + needed + 1) new_cap *= 2;
    char *new_data = realloc(buf->data, new_cap);
    if (!new_data) return false;
    buf->data = new_data;
    buf->cap = new_cap;
    return true;
}

/*
 * HTTP GET via curl(1) binary.
 * Spawns: curl -s -f -L --max-time 5 -H ... -H ... <url>
 * Reads stdout through a pipe. Returns false on spawn/HTTP failure.
 */
static bool http_get(const char *url, response_buf_t *resp)
{
    resp->data = nullptr;
    resp->size = 0;
    resp->cap = 0;

    int pipefd[2];
    if (pipe(pipefd) != 0) return false;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);

    char *argv[] = {
        "curl", "-s", "-f", "-L", "--max-time", "5",
        "-H", "User-Agent: abraxas/4.1 (weather color temp daemon)",
        "-H", "Accept: application/geo+json",
        (char *)url, nullptr
    };

    pid_t pid;
    int err = posix_spawnp(&pid, "curl", &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]); /* parent doesn't write */

    if (err != 0) {
        close(pipefd[0]);
        return false;
    }

    /* Read child stdout into buffer */
    char chunk[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], chunk, sizeof(chunk));
        if (n <= 0) break;
        if (!buf_grow(resp, (size_t)n)) break;
        memcpy(resp->data + resp->size, chunk, (size_t)n);
        resp->size += (size_t)n;
    }
    close(pipefd[0]);

    if (resp->data) resp->data[resp->size] = '\0';

    int status;
    waitpid(pid, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !resp->data || resp->size == 0) {
        free(resp->data);
        resp->data = nullptr;
        resp->size = 0;
        return false;
    }

    return true;
}

/*
 * Cloud cover heuristic from NOAA forecast keywords.
 * Ordered by priority: more specific matches first.
 */
static int cloud_cover_from_forecast(const char *forecast)
{
    if (!forecast) return 0;

    /* Convert to lowercase for matching */
    char lower[128];
    size_t i = 0;
    for (; forecast[i] && i < sizeof(lower) - 1; i++)
        lower[i] = (char)tolower((unsigned char)forecast[i]);
    lower[i] = '\0';

    /* Precipitation always means heavy cloud */
    if (strstr(lower, "rain") || strstr(lower, "storm") ||
        strstr(lower, "snow") || strstr(lower, "drizzle") ||
        strstr(lower, "showers"))
        return 95;

    /* Overcast */
    if (strstr(lower, "overcast"))
        return 90;

    /* Mostly cloudy (before general "cloudy" check) */
    if (strstr(lower, "mostly cloudy"))
        return 75;

    /* Cloudy */
    if (strstr(lower, "cloudy"))
        return 90;

    /* Partly */
    if (strstr(lower, "partly"))
        return 50;

    /* Mostly sunny/clear (before general "sunny"/"clear") */
    if (strstr(lower, "mostly sunny") || strstr(lower, "mostly clear"))
        return 25;

    /* Sunny/clear */
    if (strstr(lower, "sunny") || strstr(lower, "clear"))
        return 10;

    return 0;
}

void weather_init(void)  {}
void weather_cleanup(void) {}

weather_data_t weather_fetch(double lat, double lon)
{
    weather_data_t wd = {
        .cloud_cover = 0,
        .forecast = "Unknown",
        .temperature = 0.0,
        .is_day = true,
        .fetched_at = time(nullptr),
        .has_error = true
    };

    /* Step 1: Get grid point */
    char url[256];
    snprintf(url, sizeof(url), "https://api.weather.gov/points/%.4f,%.4f", lat, lon);

    response_buf_t resp;
    if (!http_get(url, &resp)) return wd;

    json_value_t *root = json_parse(resp.data);
    free(resp.data);
    if (!root) return wd;

    const char *forecast_url = json_string(json_path(root, "properties.forecastHourly"));
    if (!forecast_url) {
        json_free(root);
        return wd;
    }

    /* Copy URL before freeing root */
    char hourly_url[512];
    snprintf(hourly_url, sizeof(hourly_url), "%s", forecast_url);
    json_free(root);

    /* Step 2: Get hourly forecast */
    if (!http_get(hourly_url, &resp)) return wd;

    root = json_parse(resp.data);
    free(resp.data);
    if (!root) return wd;

    const json_value_t *periods = json_path(root, "properties.periods");
    const json_value_t *period = json_at(periods, 0);

    if (!period) {
        json_free(root);
        return wd;
    }

    /* Extract forecast data */
    const char *short_forecast = json_string(json_get(period, "shortForecast"));
    if (short_forecast) {
        strncpy(wd.forecast, short_forecast, sizeof(wd.forecast) - 1);
        wd.forecast[sizeof(wd.forecast) - 1] = '\0';
    }

    const json_value_t *temp_val = json_get(period, "temperature");
    if (temp_val) wd.temperature = json_number(temp_val);

    const json_value_t *day_val = json_get(period, "isDaytime");
    if (day_val) wd.is_day = json_bool(day_val);

    /* Cloud cover from keyword heuristic */
    wd.cloud_cover = cloud_cover_from_forecast(wd.forecast);

    wd.has_error = false;
    json_free(root);
    return wd;
}

#else /* NOAA_DISABLED -- non-US build, no libcurl */

#include <string.h>
#include <time.h>

void weather_init(void)    {}
void weather_cleanup(void) {}

weather_data_t weather_fetch(double lat, double lon)
{
    (void)lat;
    (void)lon;

    weather_data_t wd = {
        .cloud_cover = 0,
        .temperature = 0.0,
        .is_day = true,
        .fetched_at = time(nullptr),
        .has_error = true
    };
    strncpy(wd.forecast, "Disabled (non-USA build)", sizeof(wd.forecast) - 1);
    wd.forecast[sizeof(wd.forecast) - 1] = '\0';
    return wd;
}

#endif /* NOAA_DISABLED */
