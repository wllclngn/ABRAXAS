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
 */

#include "weather.h"
#include "json.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Dynamic response buffer for libcurl */
typedef struct {
    char   *data;
    size_t  size;
    size_t  cap;
} response_buf_t;

static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    response_buf_t *buf = userdata;
    size_t total = size * nmemb;

    if (buf->size + total + 1 > buf->cap) {
        size_t new_cap = (buf->cap ? buf->cap * 2 : 4096);
        while (new_cap < buf->size + total + 1) new_cap *= 2;
        char *new_data = realloc(buf->data, new_cap);
        if (!new_data) return 0;
        buf->data = new_data;
        buf->cap = new_cap;
    }

    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

static bool http_get(const char *url, response_buf_t *resp)
{
    resp->data = nullptr;
    resp->size = 0;
    resp->cap = 0;

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: abraxas/1.0 (weather color temp daemon)");
    headers = curl_slist_append(headers, "Accept: application/geo+json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
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
        lower[i] = (char)(forecast[i] | 0x20); /* ASCII tolower */
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
