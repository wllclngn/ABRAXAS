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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
        "-H", "User-Agent: abraxas/7.0 (weather color temp daemon)",
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

/* --- Async weather fetch (non-blocking, io_uring integrated) --- */

static bool spawn_curl_async(const char *url, pid_t *out_pid, int *out_pipe_fd)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;

    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags < 0 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);

    char *argv[] = {
        "curl", "-s", "-f", "-L", "--max-time", "5",
        "-H", "User-Agent: abraxas/7.0 (weather color temp daemon)",
        "-H", "Accept: application/geo+json",
        (char *)url, nullptr
    };

    pid_t pid;
    int err = posix_spawnp(&pid, "curl", &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);

    if (err != 0) {
        close(pipefd[0]);
        return false;
    }

    *out_pid = pid;
    *out_pipe_fd = pipefd[0];
    return true;
}

static void wfs_reset(weather_fetch_state_t *wfs)
{
    if (wfs->pipe_fd >= 0) close(wfs->pipe_fd);
    if (wfs->child_pid > 0) {
        kill(wfs->child_pid, SIGKILL);
        waitpid(wfs->child_pid, nullptr, 0);
    }
    free(wfs->buf);
    wfs->buf = nullptr;
    wfs->buf_size = 0;
    wfs->buf_cap = 0;
    wfs->pipe_fd = -1;
    wfs->child_pid = 0;
    wfs->phase = WEATHER_IDLE;
}

static weather_data_t wfs_error_result(void)
{
    weather_data_t wd = {
        .cloud_cover = 0,
        .temperature = 0.0,
        .is_day = true,
        .fetched_at = time(nullptr),
        .has_error = true
    };
    strncpy(wd.forecast, "Unknown", sizeof(wd.forecast));
    return wd;
}

/*
 * Non-blocking drain of pipe.
 * Returns: 0 = EAGAIN (more data coming)
 *          1 = EOF (child done)
 *         -1 = error
 */
static int wfs_drain_pipe(weather_fetch_state_t *wfs)
{
    char chunk[4096];
    for (;;) {
        ssize_t n = read(wfs->pipe_fd, chunk, sizeof(chunk));
        if (n > 0) {
            size_t needed = wfs->buf_size + (size_t)n + 1;
            if (needed > wfs->buf_cap) {
                size_t new_cap = wfs->buf_cap ? wfs->buf_cap * 2 : 4096;
                while (new_cap < needed) new_cap *= 2;
                char *nb = realloc(wfs->buf, new_cap);
                if (!nb) return -1;
                wfs->buf = nb;
                wfs->buf_cap = new_cap;
            }
            memcpy(wfs->buf + wfs->buf_size, chunk, (size_t)n);
            wfs->buf_size += (size_t)n;
            continue;
        }
        if (n == 0) return 1;  /* EOF */
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
}

void weather_async_init(weather_fetch_state_t *wfs)
{
    memset(wfs, 0, sizeof(*wfs));
    wfs->pipe_fd = -1;
    wfs->phase = WEATHER_IDLE;
}

int weather_async_start(weather_fetch_state_t *wfs, double lat, double lon)
{
    if (wfs->phase != WEATHER_IDLE) return -1;

    wfs->lat = lat;
    wfs->lon = lon;

    char url[256];
    snprintf(url, sizeof(url), "https://api.weather.gov/points/%.4f,%.4f", lat, lon);

    pid_t pid;
    int pipe_fd;
    if (!spawn_curl_async(url, &pid, &pipe_fd)) return -1;

    wfs->child_pid = pid;
    wfs->pipe_fd = pipe_fd;
    wfs->phase = WEATHER_READING_POINTS;
    wfs->buf = nullptr;
    wfs->buf_size = 0;
    wfs->buf_cap = 0;

    return pipe_fd;
}

int weather_async_read(weather_fetch_state_t *wfs, weather_data_t *out)
{
    int drain = wfs_drain_pipe(wfs);
    if (drain == 0) return 0;  /* EAGAIN */

    if (drain < 0) {
        wfs_reset(wfs);
        *out = wfs_error_result();
        return -1;
    }

    /* EOF: child finished writing */
    close(wfs->pipe_fd);
    wfs->pipe_fd = -1;

    int status;
    waitpid(wfs->child_pid, &status, 0);
    wfs->child_pid = 0;

    bool curl_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0
                   && wfs->buf && wfs->buf_size > 0;
    if (!curl_ok) {
        wfs_reset(wfs);
        *out = wfs_error_result();
        return -1;
    }

    wfs->buf[wfs->buf_size] = '\0';

    if (wfs->phase == WEATHER_READING_POINTS) {
        json_value_t *root = json_parse(wfs->buf);
        free(wfs->buf);
        wfs->buf = nullptr;
        wfs->buf_size = 0;
        wfs->buf_cap = 0;

        if (!root) {
            wfs_reset(wfs);
            *out = wfs_error_result();
            return -1;
        }

        const char *forecast_url = json_string(json_path(root, "properties.forecastHourly"));
        if (!forecast_url) {
            json_free(root);
            wfs_reset(wfs);
            *out = wfs_error_result();
            return -1;
        }

        snprintf(wfs->forecast_url, sizeof(wfs->forecast_url), "%s", forecast_url);
        json_free(root);

        /* Spawn phase 2: hourly forecast */
        pid_t pid;
        int pipe_fd;
        if (!spawn_curl_async(wfs->forecast_url, &pid, &pipe_fd)) {
            wfs_reset(wfs);
            *out = wfs_error_result();
            return -1;
        }

        wfs->child_pid = pid;
        wfs->pipe_fd = pipe_fd;
        wfs->phase = WEATHER_READING_FORECAST;
        return 1;  /* new pipe_fd to poll */
    }

    /* WEATHER_READING_FORECAST: parse final result */
    json_value_t *root = json_parse(wfs->buf);
    free(wfs->buf);
    wfs->buf = nullptr;
    wfs->buf_size = 0;
    wfs->buf_cap = 0;
    wfs->phase = WEATHER_IDLE;

    if (!root) {
        *out = wfs_error_result();
        return -1;
    }

    const json_value_t *periods = json_path(root, "properties.periods");
    const json_value_t *period = json_at(periods, 0);
    if (!period) {
        json_free(root);
        *out = wfs_error_result();
        return -1;
    }

    *out = (weather_data_t){
        .cloud_cover = 0,
        .temperature = 0.0,
        .is_day = true,
        .fetched_at = time(nullptr),
        .has_error = false
    };

    const char *sf = json_string(json_get(period, "shortForecast"));
    if (sf) {
        strncpy(out->forecast, sf, sizeof(out->forecast) - 1);
        out->forecast[sizeof(out->forecast) - 1] = '\0';
    } else {
        strncpy(out->forecast, "Unknown", sizeof(out->forecast));
    }

    const json_value_t *tv = json_get(period, "temperature");
    if (tv) out->temperature = json_number(tv);

    const json_value_t *dv = json_get(period, "isDaytime");
    if (dv) out->is_day = json_bool(dv);

    out->cloud_cover = cloud_cover_from_forecast(out->forecast);

    json_free(root);
    return -1;  /* done */
}

void weather_async_cleanup(weather_fetch_state_t *wfs)
{
    wfs_reset(wfs);
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
