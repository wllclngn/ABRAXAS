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
#include <sys/types.h>   /* pid_t */

/* No-op (kept for API compatibility). */
void weather_init(void);

/* No-op (kept for API compatibility). */
void weather_cleanup(void);

/* Fetch current weather from NOAA api.weather.gov.
   Fills weather_data_t with cloud_cover, forecast, temperature, is_day.
   Sets has_error=true on network/parse failure (or when NOAA_DISABLED). */
weather_data_t weather_fetch(double lat, double lon);

/* --- Async weather fetch (non-blocking, io_uring integrated) --- */

#ifndef NOAA_DISABLED

typedef enum {
    WEATHER_IDLE = 0,
    WEATHER_READING_POINTS,
    WEATHER_READING_FORECAST,
} weather_phase_t;

typedef struct {
    weather_phase_t phase;
    pid_t           child_pid;
    int             pipe_fd;       /* read end, O_NONBLOCK */
    char           *buf;
    size_t          buf_size;
    size_t          buf_cap;
    double          lat;
    double          lon;
    char            forecast_url[512];
} weather_fetch_state_t;

void weather_async_init(weather_fetch_state_t *wfs);

/* Start async fetch if IDLE. Returns pipe_fd to poll, or -1. */
int  weather_async_start(weather_fetch_state_t *wfs, double lat, double lon);

/* Call when POLLIN on pipe_fd. Returns:
 *   0  = EAGAIN (re-poll next iteration)
 *   1  = phase complete, new pipe_fd in wfs->pipe_fd (re-poll)
 *  -1  = done or error (result in *out if phase == IDLE) */
int  weather_async_read(weather_fetch_state_t *wfs, weather_data_t *out);

void weather_async_cleanup(weather_fetch_state_t *wfs);

#else /* NOAA_DISABLED */

typedef struct { int phase; } weather_fetch_state_t;
static inline void weather_async_init(weather_fetch_state_t *wfs) { wfs->phase = 0; }
static inline int  weather_async_start(weather_fetch_state_t *wfs, double lat, double lon)
    { (void)wfs; (void)lat; (void)lon; return -1; }
static inline int  weather_async_read(weather_fetch_state_t *wfs, weather_data_t *out)
    { (void)wfs; (void)out; return -1; }
static inline void weather_async_cleanup(weather_fetch_state_t *wfs) { (void)wfs; }

#endif /* !NOAA_DISABLED */

#endif /* WEATHER_H */
