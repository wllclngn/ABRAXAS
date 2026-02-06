/*
 * config.c - Configuration, override state, and path resolution
 *
 * Hand-rolled INI parser for [location] section (two keys: latitude, longitude).
 * JSON override and weather cache parsed via json.h, written via fprintf.
 */

#include "config.h"
#include "json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool config_init_paths(abraxas_paths_t *paths)
{
    const char *home = getenv("HOME");
    if (!home) return false;

    /* Build config dir first, bail if it won't fit */
    char dir[ABRAXAS_PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/.config/abraxas", home);
    if (n < 0 || (size_t)n >= sizeof(dir)) return false;
    memcpy(paths->config_dir, dir, (size_t)n + 1);

    /* dir is verified to fit above; GCC can't prove it across calls */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(paths->config_file,   sizeof(paths->config_file),   "%s/config.ini",        dir);
    snprintf(paths->cache_file,    sizeof(paths->cache_file),    "%s/weather_cache.json", dir);
    snprintf(paths->override_file, sizeof(paths->override_file), "%s/override.json",      dir);
    snprintf(paths->zipdb_file,    sizeof(paths->zipdb_file),    "%s/us_zipcodes.bin",    dir);
#pragma GCC diagnostic pop

    /* Create config directory if it doesn't exist */
    if (mkdir(paths->config_dir, 0755) < 0 && errno != EEXIST)
        return false;

    return true;
}

/* --- INI config --- */

location_t config_load_location(const abraxas_paths_t *paths)
{
    location_t loc = { .valid = false };

    FILE *f = fopen(paths->config_file, "r");
    if (!f) return loc;

    bool in_location = false;
    bool has_lat = false, has_lon = false;
    char line[256];

    while (fgets(line, sizeof(line), f)) {
        /* Strip leading whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        /* Skip comments and blank lines */
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\0') continue;

        /* Section header */
        if (*p == '[') {
            in_location = (strncmp(p, "[location]", 10) == 0);
            continue;
        }

        if (!in_location) continue;

        /* Key = value */
        char *eq = strchr(p, '=');
        if (!eq) continue;

        /* Trim key */
        char *kend = eq - 1;
        while (kend > p && (*kend == ' ' || *kend == '\t')) kend--;
        size_t klen = (size_t)(kend - p + 1);

        /* Trim value */
        char *vstart = eq + 1;
        while (*vstart == ' ' || *vstart == '\t') vstart++;
        char *vend = vstart + strlen(vstart) - 1;
        while (vend > vstart && (*vend == '\n' || *vend == '\r' || *vend == ' ')) vend--;
        *(vend + 1) = '\0';

        if (klen == 8 && strncmp(p, "latitude", 8) == 0) {
            char *end;
            loc.lat = strtod(vstart, &end);
            has_lat = (end != vstart);
        } else if (klen == 9 && strncmp(p, "longitude", 9) == 0) {
            char *end;
            loc.lon = strtod(vstart, &end);
            has_lon = (end != vstart);
        }
    }

    fclose(f);
    loc.valid = has_lat && has_lon;
    return loc;
}

bool config_save_location(const abraxas_paths_t *paths, double lat, double lon)
{
    FILE *f = fopen(paths->config_file, "w");
    if (!f) return false;

    fprintf(f, "[location]\n");
    fprintf(f, "latitude = %.6f\n", lat);
    fprintf(f, "longitude = %.6f\n", lon);
    fclose(f);
    return true;
}

/* --- Override JSON --- */

override_state_t config_load_override(const abraxas_paths_t *paths)
{
    override_state_t ovr = { .active = false };

    FILE *f = fopen(paths->override_file, "r");
    if (!f) return ovr;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    constexpr long MAX_OVERRIDE_FILE_SIZE = 4096;
    if (sz <= 0 || sz > MAX_OVERRIDE_FILE_SIZE) { fclose(f); return ovr; }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return ovr; }

    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);

    json_value_t *root = json_parse(buf);
    free(buf);
    if (!root) return ovr;

    const json_value_t *v;

    v = json_get(root, "active");
    if (v) ovr.active = json_bool(v);

    v = json_get(root, "target_temp");
    if (v) ovr.target_temp = (int)json_number(v);

    v = json_get(root, "duration_minutes");
    if (v) ovr.duration_minutes = (int)json_number(v);

    v = json_get(root, "issued_at");
    if (v) ovr.issued_at = (time_t)json_number(v);

    v = json_get(root, "start_temp");
    if (v) ovr.start_temp = (int)json_number(v);

    json_free(root);
    return ovr;
}

bool config_save_override(const abraxas_paths_t *paths, const override_state_t *ovr)
{
    FILE *f = fopen(paths->override_file, "w");
    if (!f) return false;

    fprintf(f, "{\n");
    fprintf(f, "  \"active\": %s,\n", ovr->active ? "true" : "false");
    fprintf(f, "  \"target_temp\": %d,\n", ovr->target_temp);
    fprintf(f, "  \"duration_minutes\": %d,\n", ovr->duration_minutes);
    fprintf(f, "  \"issued_at\": %ld,\n", (long)ovr->issued_at);
    fprintf(f, "  \"start_temp\": %d\n", ovr->start_temp);
    fprintf(f, "}\n");
    fclose(f);
    return true;
}

void config_clear_override(const abraxas_paths_t *paths)
{
    unlink(paths->override_file);
}

/* --- Weather cache JSON --- */

weather_data_t config_load_weather_cache(const abraxas_paths_t *paths)
{
    weather_data_t wd = { .has_error = true };

    FILE *f = fopen(paths->cache_file, "r");
    if (!f) return wd;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    constexpr long MAX_WEATHER_FILE_SIZE = 8192;
    if (sz <= 0 || sz > MAX_WEATHER_FILE_SIZE) { fclose(f); return wd; }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return wd; }

    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);

    json_value_t *root = json_parse(buf);
    free(buf);
    if (!root) return wd;

    const json_value_t *v;

    v = json_get(root, "cloud_cover");
    if (v) wd.cloud_cover = (int)json_number(v);

    v = json_get(root, "forecast");
    if (v && json_string(v)) {
        strncpy(wd.forecast, json_string(v), sizeof(wd.forecast) - 1);
        wd.forecast[sizeof(wd.forecast) - 1] = '\0';
    }

    v = json_get(root, "temperature");
    if (v) wd.temperature = json_number(v);

    v = json_get(root, "is_day");
    if (v) wd.is_day = json_bool(v);

    v = json_get(root, "fetched_at");
    if (v) wd.fetched_at = (time_t)json_number(v);

    /* Check for error key */
    v = json_get(root, "error");
    wd.has_error = (v != nullptr);

    json_free(root);

    /* If no error key and we have fetched_at, it's valid */
    if (!wd.has_error && wd.fetched_at == 0)
        wd.has_error = true;

    return wd;
}

static void json_write_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\b': fputs("\\b", f);  break;
        case '\f': fputs("\\f", f);  break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:
            if (c < 0x20)
                fprintf(f, "\\u%04x", c);
            else
                fputc(c, f);
            break;
        }
    }
    fputc('"', f);
}

bool config_save_weather_cache(const abraxas_paths_t *paths, const weather_data_t *wd)
{
    FILE *f = fopen(paths->cache_file, "w");
    if (!f) return false;

    if (wd->has_error) {
        fprintf(f, "{\n");
        fprintf(f, "  \"error\": \"fetch failed\",\n");
        fprintf(f, "  \"cloud_cover\": 0,\n");
        fprintf(f, "  \"fetched_at\": %ld\n", (long)wd->fetched_at);
        fprintf(f, "}\n");
    } else {
        fprintf(f, "{\n");
        fprintf(f, "  \"cloud_cover\": %d,\n", wd->cloud_cover);
        fprintf(f, "  \"forecast\": ");
        json_write_string(f, wd->forecast);
        fprintf(f, ",\n");
        fprintf(f, "  \"temperature\": %.1f,\n", wd->temperature);
        fprintf(f, "  \"is_day\": %s,\n", wd->is_day ? "true" : "false");
        fprintf(f, "  \"fetched_at\": %ld\n", (long)wd->fetched_at);
        fprintf(f, "}\n");
    }

    fclose(f);
    return true;
}

bool config_weather_needs_refresh(const weather_data_t *wd)
{
    if (wd->has_error || wd->fetched_at == 0)
        return true;

    time_t now = time(nullptr);
    return difftime(now, wd->fetched_at) > WEATHER_REFRESH_SEC;
}
