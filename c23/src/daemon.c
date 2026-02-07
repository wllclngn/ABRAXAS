/*
 * daemon.c - Main daemon event loop
 *
 * Linux kernel interfaces:
 *   - io_uring: single-syscall event loop (poll + timeout in one enter call)
 *   - inotify: config file change detection
 *   - signalfd: clean shutdown via SIGTERM/SIGINT
 *   - prctl: timer slack, no_new_privs, dumpable
 *   - seccomp-bpf: syscall whitelist (post-init)
 *   - landlock: filesystem sandbox (post-init)
 *
 * No fallback. Requires kernel >= 5.1 (io_uring).
 * Gamma control via libmeridian (statically linked).
 */

#define _GNU_SOURCE

#include "daemon.h"
#include "config.h"
#include "landlock.h"
#include "seccomp.h"
#include "sigmoid.h"
#include "solar.h"
#include "uring.h"
#include "weather.h"

#include <meridian.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

/* io_uring user_data tags */
constexpr uint64_t EV_INOTIFY = 1;
constexpr uint64_t EV_SIGNAL  = 2;
constexpr uint64_t EV_TIMEOUT = 3;
constexpr uint64_t EV_CANCEL  = 4;
constexpr uint64_t EV_WEATHER = 5;

/* --- Gamma control (libmeridian direct calls) --- */

static meridian_state_t *gamma_state = nullptr;

static bool gamma_init(void)
{
    meridian_error_t err = meridian_init(&gamma_state);
    if (err != MERIDIAN_OK) {
        fprintf(stderr, "[libmeridian] Init failed: %s\n", meridian_strerror(err));
        gamma_state = nullptr;
        return false;
    }
    fprintf(stderr, "[libmeridian] Initialized with %s backend\n",
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

static int create_inotify_watch(const char *dir_path)
{
    int fd = inotify_init1(IN_CLOEXEC);
    if (fd < 0) return -1;

    int wd = inotify_add_watch(fd, dir_path, IN_CLOSE_WRITE);
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

/* --- Inotify event processing (shared by both event loops) --- */

static void process_inotify(int inotify_fd, const daemon_state_t *state,
                            bool *config_changed, bool *override_changed)
{
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len = read(inotify_fd, buf, sizeof(buf));
    if (len <= 0) return;

    for (char *ptr = buf; ptr < buf + len; ) {
        struct inotify_event *event = (struct inotify_event *)ptr;
        if (event->len > 0) {
            const char *override_name = strrchr(state->paths.override_file, '/');
            override_name = override_name ? override_name + 1 : state->paths.override_file;

            const char *config_name = strrchr(state->paths.config_file, '/');
            config_name = config_name ? config_name + 1 : state->paths.config_file;

            if (strcmp(event->name, override_name) == 0)
                *override_changed = true;
            if (strcmp(event->name, config_name) == 0) {
                *config_changed = true;
                fprintf(stderr, "[inotify] %s changed, reloading...\n", event->name);
            }
        }
        ptr += sizeof(struct inotify_event) + event->len;
    }
}

/* --- io_uring event loop --- */

static void event_loop_uring(daemon_state_t *state, abraxas_ring_t *ring,
                             int inotify_fd, int signal_fd)
{
    struct __kernel_timespec ts = {
        .tv_sec  = TEMP_UPDATE_SEC,
        .tv_nsec = 0
    };

    weather_fetch_state_t wfs;
    weather_async_init(&wfs);

    while (1) {
        /* Submit: poll both fds + timeout */
        if (inotify_fd >= 0)
            uring_prep_poll(ring, inotify_fd, EV_INOTIFY);
        if (signal_fd >= 0)
            uring_prep_poll(ring, signal_fd, EV_SIGNAL);
        if (wfs.pipe_fd >= 0)
            uring_prep_poll(ring, wfs.pipe_fd, EV_WEATHER);
        uring_prep_timeout(ring, &ts, EV_TIMEOUT);

        int ret = uring_submit_and_wait(ring);
        if (ret < 0 && errno != EINTR) break;

        /* Process completions */
        bool timer_expired = false;
        bool got_signal = false;
        _Atomic bool weather_ready = false;
        bool config_changed = false;
        bool override_changed = false;

        struct io_uring_cqe *cqe;
        while (uring_peek_cqe(ring, &cqe)) {
            switch (cqe->user_data) {
            case EV_TIMEOUT:
                timer_expired = true;
                break;
            case EV_SIGNAL:
                got_signal = true;
                break;
            case EV_INOTIFY:
                if (cqe->res > 0)
                    process_inotify(inotify_fd, state,
                                    &config_changed, &override_changed);
                break;
            case EV_WEATHER:
                if (cqe->res > 0) weather_ready = true;
                break;
            case EV_CANCEL:
                break;
            }
            uring_cqe_seen(ring);
        }

        /* If we woke early (inotify/signal), cancel the pending timeout */
        if (!timer_expired) {
            uring_prep_cancel(ring, EV_TIMEOUT, EV_CANCEL);
            uring_submit_and_wait(ring);
            /* Drain cancel completion */
            while (uring_peek_cqe(ring, &cqe))
                uring_cqe_seen(ring);
        }

        if (got_signal) {
            /* Drain signalfd so it doesn't refire */
            if (signal_fd >= 0) {
                struct signalfd_siginfo si;
                ssize_t n = read(signal_fd, &si, sizeof(si));
                (void)n;
            }
            fprintf(stderr, "\nReceived shutdown signal...\n");
            weather_async_cleanup(&wfs);
            break;
        }

        /* --- Common tick processing --- */

        time_t now = time(nullptr);

        if (config_changed) {
            location_t new_loc = config_load_location(&state->paths);
            if (new_loc.valid) {
                state->location = new_loc;
                fprintf(stderr, "[config] Location updated: %.4f, %.4f\n",
                       state->location.lat, state->location.lon);
            }
            state->weather = config_load_weather_cache(&state->paths);
        }

        if (override_changed) {
            override_state_t od = config_load_override(&state->paths);

            if (od.active && od.issued_at != state->manual_issued_at) {
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
                    fprintf(stderr, "[manual] Override: %dK -> %dK over %d min\n",
                           state->manual_start_temp, state->manual_target_temp,
                           state->manual_duration_min);
                else
                    fprintf(stderr, "[manual] Override: -> %dK (instant)\n", state->manual_target_temp);

                struct tm rt;
                localtime_r(&state->manual_resume_time, &rt);
                fprintf(stderr, "[manual] Auto-resume at: %02d:%02d\n", rt.tm_hour, rt.tm_min);

            } else if (!od.active && state->manual_mode) {
                state->manual_mode = false;
                state->manual_issued_at = 0;
                config_clear_override(&state->paths);
                fprintf(stderr, "[manual] Override cleared, resuming solar control\n");
            }
        }

#ifndef NOAA_DISABLED
        /* Start async weather fetch if needed and not in-flight */
        if (wfs.phase == WEATHER_IDLE && config_weather_needs_refresh(&state->weather)) {
            struct tm nt;
            localtime_r(&now, &nt);
            fprintf(stderr, "[%02d:%02d:%02d] Starting weather fetch...\n",
                    nt.tm_hour, nt.tm_min, nt.tm_sec);
            (void)weather_async_start(&wfs, state->location.lat, state->location.lon);
        }

        /* Process async weather data if pipe signaled */
        if (weather_ready && wfs.phase != WEATHER_IDLE) {
            weather_data_t result;
            int rc = weather_async_read(&wfs, &result);
            if (rc == -1) {
                /* Done (success or error) */
                state->weather = result;
                config_save_weather_cache(&state->paths, &state->weather);
                if (!state->weather.has_error)
                    fprintf(stderr, "  Weather: %s (%d%% clouds)\n",
                           state->weather.forecast, state->weather.cloud_cover);
                else
                    fprintf(stderr, "  Weather fetch failed\n");
            }
            /* rc==0: EAGAIN, re-poll next iteration */
            /* rc==1: phase transition, new pipe_fd, poll next iteration */
        }
#endif

        int temp;
        if (state->manual_mode) {
            temp = calculate_manual_temp(state->manual_start_temp, state->manual_target_temp,
                                         state->manual_start_time, state->manual_duration_min, now);

            double elapsed = difftime(now, state->manual_start_time) / 60.0;
            if (elapsed >= (double)state->manual_duration_min &&
                state->manual_resume_time > 0 && now >= state->manual_resume_time) {
                state->manual_mode = false;
                state->manual_issued_at = 0;
                config_clear_override(&state->paths);
                fprintf(stderr, "[manual] Auto-resuming solar control (transition window approaching)\n");
                temp = solar_temperature(now, state->location.lat, state->location.lon,
                                         &state->weather);
            }
        } else {
            temp = solar_temperature(now, state->location.lat, state->location.lon,
                                     &state->weather);
        }

        if (!state->last_temp_valid || temp != state->last_temp) {
            struct tm nt;
            localtime_r(&now, &nt);

            if (state->manual_mode) {
                double elapsed = difftime(now, state->manual_start_time) / 60.0;
                if (elapsed < (double)state->manual_duration_min) {
                    int pct = (int)(elapsed / (double)state->manual_duration_min * 100.0);
                    if (pct > 100) pct = 100;
                    fprintf(stderr, "[%02d:%02d:%02d] Manual: %dK (%d%%)\n",
                           nt.tm_hour, nt.tm_min, nt.tm_sec, temp, pct);
                } else {
                    fprintf(stderr, "[%02d:%02d:%02d] Manual: %dK (holding)\n",
                           nt.tm_hour, nt.tm_min, nt.tm_sec, temp);
                }
            } else {
                sun_position_t sp = solar_position(now, state->location.lat, state->location.lon);
                fprintf(stderr, "[%02d:%02d:%02d] Solar: %dK (sun: %.1f, clouds: %d%%)\n",
                       nt.tm_hour, nt.tm_min, nt.tm_sec, temp, sp.elevation,
                       state->weather.cloud_cover);
            }

            gamma_set(temp);
            state->last_temp = temp;
            state->last_temp_valid = true;
        }
    }
}

/* --- Main entry point --- */

void daemon_run(daemon_state_t *state)
{
    fprintf(stderr, "Starting abraxas daemon\n");
    fprintf(stderr, "Location: %.4f, %.4f\n", state->location.lat, state->location.lon);
    fprintf(stderr, "Weather refresh: every %d min\n", WEATHER_REFRESH_SEC / 60);
    fprintf(stderr, "Temperature update: every %ds\n", TEMP_UPDATE_SEC);

    /* Block SIGTERM/SIGINT immediately and create signalfd.
     * Must happen before gamma retry so SIGTERM is never lost during init.
     * The signalfd is polled between gamma retries and consumed in the event loop. */
    int signal_fd = create_signalfd_masked();
    if (signal_fd >= 0)
        fprintf(stderr, "[kernel] signalfd created (fd=%d)\n", signal_fd);
    else
        fprintf(stderr, "[warn] signalfd failed\n");

    /* Retry gamma init -- display server may not be ready at boot.
     * Poll every 500ms for up to 30s to catch X11/Wayland readiness fast. */
    constexpr int  GAMMA_INIT_MAX_RETRIES = 60;
    constexpr long GAMMA_INIT_RETRY_NS    = 500000000L; /* 500ms */
    for (int attempt = 0; attempt < GAMMA_INIT_MAX_RETRIES; attempt++) {
        if (gamma_init()) break;
        if (attempt == GAMMA_INIT_MAX_RETRIES - 1) {
            fprintf(stderr, "[fatal] No gamma backend after 30s\n");
            exit(1);
        }
        /* Check for SIGTERM between retries (non-blocking) */
        if (signal_fd >= 0) {
            struct pollfd pfd = { .fd = signal_fd, .events = POLLIN };
            if (poll(&pfd, 1, 0) > 0) {
                fprintf(stderr, "Received signal during gamma init, exiting...\n");
                close(signal_fd);
                exit(0);
            }
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = GAMMA_INIT_RETRY_NS };
        nanosleep(&ts, nullptr);
    }

    /* Write PID file */
    config_write_pid(&state->paths);

    /* Load cached data */
    state->weather = config_load_weather_cache(&state->paths);

    /* Apply correct temperature immediately at startup */
    int startup_temp = solar_temperature(time(nullptr),
        state->location.lat, state->location.lon, &state->weather);
    gamma_set(startup_temp);
    fprintf(stderr, "[startup] Applied %dK\n", startup_temp);

    /* Init weather subsystem */
    weather_init();

    /* Create kernel fds */
    int inotify_fd = create_inotify_watch(state->paths.config_dir);
    if (inotify_fd >= 0)
        fprintf(stderr, "[kernel] inotify watching %s (fd=%d)\n", state->paths.config_dir, inotify_fd);
    else
        fprintf(stderr, "[warn] inotify failed, config changes require restart\n");

    /* prctl hardening */
    prctl(PR_SET_TIMERSLACK, 1);         /* 1ns timer precision (default 50us) */
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    prctl(PR_SET_DUMPABLE, 0);
    fprintf(stderr, "[kernel] prctl: timerslack=1ns, no_new_privs, !dumpable\n");

    /* Landlock filesystem sandbox */
    if (landlock_install_sandbox(state->paths.config_dir))
        fprintf(stderr, "[kernel] landlock: filesystem sandbox active\n");
    else
        fprintf(stderr, "[kernel] landlock: unavailable (kernel too old or disabled)\n");

    /* seccomp-bpf syscall whitelist */
    if (seccomp_install_filter())
        fprintf(stderr, "[kernel] seccomp: syscall whitelist active\n");
    else
        fprintf(stderr, "[kernel] seccomp: filter install failed\n");

    /* Recover from active override on restart */
    override_state_t ovr = config_load_override(&state->paths);
    if (ovr.active) {
        double elapsed = difftime(time(nullptr), ovr.issued_at) / 60.0;
        if (elapsed >= (double)ovr.duration_minutes) {
            config_clear_override(&state->paths);
            fprintf(stderr, "[manual] Cleared stale override (completed %.0f min ago)\n",
                   elapsed - (double)ovr.duration_minutes);
        } else {
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
            fprintf(stderr, "[manual] Recovered override: -> %dK (%d min)\n",
                   state->manual_target_temp, state->manual_duration_min);

            struct tm rt;
            localtime_r(&state->manual_resume_time, &rt);
            fprintf(stderr, "[manual] Auto-resume at: %02d:%02d\n", rt.tm_hour, rt.tm_min);
        }
    }

    /* io_uring event loop (no fallback -- requires kernel >= 5.1) */
    abraxas_ring_t ring;
    if (!uring_init(&ring, 8)) {
        fprintf(stderr, "[fatal] io_uring_setup failed (kernel >= 5.1 required)\n");
        exit(1);
    }
    fprintf(stderr, "[kernel] io_uring initialized (1 syscall/tick)\n\n");
    event_loop_uring(state, &ring, inotify_fd, signal_fd);
    uring_destroy(&ring);

    /* Clean shutdown */
    fprintf(stderr, "Shutting down...\n");
    weather_cleanup();
    gamma_restore();
    gamma_cleanup();
    config_remove_pid(&state->paths);

    if (inotify_fd >= 0) close(inotify_fd);
    if (signal_fd >= 0)  close(signal_fd);
}
