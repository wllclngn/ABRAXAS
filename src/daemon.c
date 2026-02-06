/*
 * daemon.c - Main daemon event loop
 *
 * Linux kernel interfaces: timerfd (60s tick), inotify (config changes),
 * signalfd (clean shutdown via SIGTERM/SIGINT). All multiplexed with select().
 * Gamma control via libmeridian (statically linked).
 */

#define _GNU_SOURCE

#include "daemon.h"
#include "config.h"
#include "sigmoid.h"
#include "solar.h"
#include "weather.h"

#include <meridian.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* --- Gamma control (libmeridian direct calls, no ctypes) --- */

static meridian_state_t *gamma_state = nullptr;

static bool gamma_init(void)
{
    meridian_error_t err = meridian_init(&gamma_state);
    if (err != MERIDIAN_OK) {
        fprintf(stderr, "[libmeridian] Init failed: %s\n", meridian_strerror(err));
        gamma_state = nullptr;
        return false;
    }
    printf("[libmeridian] Initialized with %s backend\n",
           meridian_get_backend_name(gamma_state));
    return true;
}

static bool gamma_set(int temp)
{
    if (!gamma_state) return false;
    meridian_error_t err = meridian_set_temperature(gamma_state, temp, 1.0f);
    if (err != MERIDIAN_OK) {
        fprintf(stderr, "[libmeridian] Set temperature failed: %s\n",
                meridian_strerror(err));
        return false;
    }
    return true;
}

static void gamma_restore(void)
{
    if (gamma_state) (void)meridian_restore(gamma_state);
}

static void gamma_cleanup(void)
{
    if (gamma_state) {
        meridian_free(gamma_state);
        gamma_state = nullptr;
    }
}

/* --- Linux kernel fd helpers --- */

static int create_timerfd(int interval_sec)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (fd < 0) return -1;

    struct itimerspec spec = {
        .it_interval = { .tv_sec = interval_sec, .tv_nsec = 0 },
        .it_value    = { .tv_sec = interval_sec, .tv_nsec = 0 }
    };

    if (timerfd_settime(fd, 0, &spec, nullptr) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int create_inotify_watch(const char *dir_path)
{
    int fd = inotify_init1(IN_CLOEXEC);
    if (fd < 0) return -1;

    int wd = inotify_add_watch(fd, dir_path, IN_CLOSE_WRITE | IN_MODIFY);
    if (wd < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int create_signalfd_masked(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);

    if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) return -1;

    int fd = signalfd(-1, &mask, SFD_CLOEXEC);
    return fd;
}

/* --- Solar temperature helper --- */

static int solar_temperature(time_t now, double lat, double lon,
                             const weather_data_t *weather)
{
    sun_times_t st = solar_sunrise_sunset(now, lat, lon);
    if (!st.valid) return TEMP_NIGHT;

    double minutes_from_sunrise = difftime(now, st.sunrise) / 60.0;
    double minutes_to_sunset    = difftime(st.sunset, now)  / 60.0;

    int cloud_cover = weather ? weather->cloud_cover : 0;
    bool is_dark = cloud_cover >= CLOUD_THRESHOLD;

    return calculate_solar_temp(minutes_from_sunrise, minutes_to_sunset, is_dark);
}

/* --- Main event loop --- */

void daemon_run(daemon_state_t *state)
{
    printf("Starting abraxas daemon\n");
    printf("Location: %.4f, %.4f\n", state->location.lat, state->location.lon);
    printf("Weather refresh: every %d min\n", WEATHER_REFRESH_SEC / 60);
    printf("Temperature update: every %ds\n", TEMP_UPDATE_SEC);
    printf("Using kernel timerfd + inotify + signalfd\n\n");

    if (!gamma_init()) {
        fprintf(stderr, "[fatal] No gamma backend available\n");
        return;
    }

    /* Load cached data */
    state->weather = config_load_weather_cache(&state->paths);

    bool running = true;

    /* Create kernel fds */
    int timer_fd = create_timerfd(TEMP_UPDATE_SEC);
    if (timer_fd >= 0)
        printf("[kernel] timerfd created (fd=%d)\n", timer_fd);
    else
        fprintf(stderr, "[warn] timerfd failed, falling back to sleep()\n");

    int inotify_fd = create_inotify_watch(state->paths.config_dir);
    if (inotify_fd >= 0)
        printf("[kernel] inotify watching %s (fd=%d)\n", state->paths.config_dir, inotify_fd);
    else
        fprintf(stderr, "[warn] inotify failed, config changes require restart\n");

    int signal_fd = create_signalfd_masked();
    if (signal_fd >= 0)
        printf("[kernel] signalfd created (fd=%d)\n", signal_fd);
    else
        fprintf(stderr, "[warn] signalfd failed\n");

    /* Recover from active override on restart */
    override_state_t ovr = config_load_override(&state->paths);
    if (ovr.active) {
        state->manual_mode = true;
        state->manual_target_temp = ovr.target_temp;
        state->manual_duration_min = ovr.duration_minutes;
        state->manual_issued_at = ovr.issued_at;
        state->manual_start_time = ovr.issued_at;
        state->manual_start_temp = ovr.start_temp;
        if (state->manual_start_temp == 0) {
            state->manual_start_temp = solar_temperature(time(nullptr),
                state->location.lat, state->location.lon, &state->weather);
            ovr.start_temp = state->manual_start_temp;
            config_save_override(&state->paths, &ovr);
        }
        state->manual_resume_time = next_transition_resume(time(nullptr),
            state->location.lat, state->location.lon);
        printf("[manual] Recovered override: -> %dK (%d min)\n",
               state->manual_target_temp, state->manual_duration_min);

        struct tm rt;
        localtime_r(&state->manual_resume_time, &rt);
        printf("[manual] Auto-resume at: %02d:%02d\n", rt.tm_hour, rt.tm_min);
    }

    /* Event loop */
    while (running) {
        /* Build fd_set */
        fd_set readfds;
        FD_ZERO(&readfds);
        int maxfd = -1;

        if (timer_fd >= 0)   { FD_SET(timer_fd, &readfds);   if (timer_fd > maxfd) maxfd = timer_fd; }
        if (inotify_fd >= 0) { FD_SET(inotify_fd, &readfds); if (inotify_fd > maxfd) maxfd = inotify_fd; }
        if (signal_fd >= 0)  { FD_SET(signal_fd, &readfds);  if (signal_fd > maxfd) maxfd = signal_fd; }

        struct timeval timeout = { .tv_sec = TEMP_UPDATE_SEC, .tv_usec = 0 };
        struct timeval *tv = (timer_fd >= 0) ? nullptr : &timeout;

        int ready = select(maxfd + 1, &readfds, nullptr, nullptr, tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        bool config_changed = false;
        bool override_changed = false;

        /* Process events */
        if (timer_fd >= 0 && FD_ISSET(timer_fd, &readfds)) {
            uint64_t exp;
            (void)read(timer_fd, &exp, sizeof(exp));
        }

        if (signal_fd >= 0 && FD_ISSET(signal_fd, &readfds)) {
            struct signalfd_siginfo si;
            (void)read(signal_fd, &si, sizeof(si));
            printf("\nReceived shutdown signal...\n");
            running = false;
            break;
        }

        if (inotify_fd >= 0 && FD_ISSET(inotify_fd, &readfds)) {
            char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t len = read(inotify_fd, buf, sizeof(buf));

            for (char *ptr = buf; ptr < buf + len; ) {
                struct inotify_event *event = (struct inotify_event *)ptr;
                if (event->len > 0) {
                    const char *override_name = strrchr(state->paths.override_file, '/');
                    override_name = override_name ? override_name + 1 : state->paths.override_file;

                    const char *config_name = strrchr(state->paths.config_file, '/');
                    config_name = config_name ? config_name + 1 : state->paths.config_file;

                    const char *cache_name = strrchr(state->paths.cache_file, '/');
                    cache_name = cache_name ? cache_name + 1 : state->paths.cache_file;

                    if (strcmp(event->name, override_name) == 0)
                        override_changed = true;
                    if (strcmp(event->name, config_name) == 0 ||
                        strcmp(event->name, cache_name) == 0) {
                        config_changed = true;
                        printf("[inotify] %s changed, reloading...\n", event->name);
                    }
                }
                ptr += sizeof(struct inotify_event) + event->len;
            }
        }

        time_t now = time(nullptr);

        /* Reload config if changed */
        if (config_changed) {
            location_t new_loc = config_load_location(&state->paths);
            if (new_loc.valid) {
                state->location = new_loc;
                printf("[config] Location updated: %.4f, %.4f\n",
                       state->location.lat, state->location.lon);
            }
            state->weather = config_load_weather_cache(&state->paths);
        }

        /* Process override changes */
        if (override_changed) {
            override_state_t od = config_load_override(&state->paths);

            if (od.active && od.issued_at != state->manual_issued_at) {
                /* New override command from CLI */
                state->manual_mode = true;
                state->manual_issued_at = od.issued_at;
                state->manual_target_temp = od.target_temp;
                state->manual_duration_min = od.duration_minutes;
                state->manual_start_time = od.issued_at;
                state->manual_start_temp = state->last_temp_valid
                    ? state->last_temp
                    : solar_temperature(now, state->location.lat, state->location.lon, &state->weather);

                if (od.start_temp == 0) {
                    od.start_temp = state->manual_start_temp;
                    config_save_override(&state->paths, &od);
                }

                state->manual_resume_time = next_transition_resume(now,
                    state->location.lat, state->location.lon);

                if (state->manual_duration_min > 0)
                    printf("[manual] Override: %dK -> %dK over %d min\n",
                           state->manual_start_temp, state->manual_target_temp,
                           state->manual_duration_min);
                else
                    printf("[manual] Override: -> %dK (instant)\n", state->manual_target_temp);

                struct tm rt;
                localtime_r(&state->manual_resume_time, &rt);
                printf("[manual] Auto-resume at: %02d:%02d\n", rt.tm_hour, rt.tm_min);

            } else if (!od.active && state->manual_mode) {
                /* Resume command from CLI */
                state->manual_mode = false;
                state->manual_issued_at = 0;
                config_clear_override(&state->paths);
                printf("[manual] Override cleared, resuming solar control\n");
            }
        }

        /* Refresh weather if needed */
        if (config_weather_needs_refresh(&state->weather)) {
            struct tm nt;
            localtime_r(&now, &nt);
            printf("[%02d:%02d:%02d] Refreshing weather...\n", nt.tm_hour, nt.tm_min, nt.tm_sec);

            state->weather = weather_fetch(state->location.lat, state->location.lon);
            config_save_weather_cache(&state->paths, &state->weather);

            if (!state->weather.has_error)
                printf("  Weather: %s (%d%% clouds)\n",
                       state->weather.forecast, state->weather.cloud_cover);
            else
                printf("  Weather fetch failed\n");
        }

        /* Calculate temperature */
        int temp;
        if (state->manual_mode) {
            temp = calculate_manual_temp(state->manual_start_temp, state->manual_target_temp,
                                         state->manual_start_time, state->manual_duration_min, now);

            /* Check auto-resume (only after transition is complete) */
            double elapsed = difftime(now, state->manual_start_time) / 60.0;
            if (elapsed >= (double)state->manual_duration_min &&
                state->manual_resume_time > 0 && now >= state->manual_resume_time) {
                state->manual_mode = false;
                state->manual_issued_at = 0;
                config_clear_override(&state->paths);
                printf("[manual] Auto-resuming solar control (transition window approaching)\n");
                temp = solar_temperature(now, state->location.lat, state->location.lon,
                                         &state->weather);
            }
        } else {
            temp = solar_temperature(now, state->location.lat, state->location.lon,
                                     &state->weather);
        }

        /* Apply temperature if changed */
        if (!state->last_temp_valid || temp != state->last_temp) {
            struct tm nt;
            localtime_r(&now, &nt);

            if (state->manual_mode) {
                double elapsed = difftime(now, state->manual_start_time) / 60.0;
                if (elapsed < (double)state->manual_duration_min) {
                    int pct = (int)(elapsed / (double)state->manual_duration_min * 100.0);
                    if (pct > 100) pct = 100;
                    printf("[%02d:%02d:%02d] Manual: %dK (%d%%)\n",
                           nt.tm_hour, nt.tm_min, nt.tm_sec, temp, pct);
                } else {
                    printf("[%02d:%02d:%02d] Manual: %dK (holding)\n",
                           nt.tm_hour, nt.tm_min, nt.tm_sec, temp);
                }
            } else {
                sun_position_t sp = solar_position(now, state->location.lat, state->location.lon);
                printf("[%02d:%02d:%02d] Solar: %dK (sun: %.1f, clouds: %d%%)\n",
                       nt.tm_hour, nt.tm_min, nt.tm_sec, temp, sp.elevation,
                       state->weather.cloud_cover);
            }

            gamma_set(temp);
            state->last_temp = temp;
            state->last_temp_valid = true;
        }
    }

    /* Clean shutdown */
    printf("Shutting down...\n");
    gamma_restore();
    gamma_cleanup();

    if (timer_fd >= 0)   close(timer_fd);
    if (inotify_fd >= 0) close(inotify_fd);
    if (signal_fd >= 0)  close(signal_fd);
}
