//! Daemon event loop.
//!
//! Linux kernel interfaces: timerfd (60s tick), inotify (config changes),
//! signalfd (clean shutdown via SIGTERM/SIGINT). All multiplexed with poll().
//! Gamma control via auto-detected backend.

use crate::config::{self, Location, Paths, WeatherData};
use crate::{
    sigmoid, solar, weather, CLOUD_THRESHOLD, TEMP_UPDATE_SEC, now_epoch,
};
use crate::gamma;

use std::ffi::CString;

const GAMMA_INIT_MAX_RETRIES: i32 = 60;
const GAMMA_INIT_RETRY_MS: u64 = 500;

/// Event discrimination from poll/inotify
#[derive(Default)]
struct EventResult {
    override_changed: bool,
    config_changed: bool,
    signal_received: bool,
}

/// Full daemon runtime state
struct DaemonState {
    location: Location,
    paths: Paths,
    weather: Option<WeatherData>,
    gamma: Option<gamma::GammaState>,

    // Manual mode tracking
    manual_mode: bool,
    manual_start_temp: i32,
    manual_target_temp: i32,
    manual_start_time: i64,
    manual_duration_min: i32,
    manual_issued_at: i64,
    manual_resume_time: i64,

    // Last applied temperature
    last_temp: i32,
    last_temp_valid: bool,
}

// --- Linux kernel fd helpers ---

/// Create a timerfd with the given interval in seconds.
fn setup_timerfd(interval_secs: i64) -> i32 {
    let fd = unsafe { libc::timerfd_create(libc::CLOCK_MONOTONIC, libc::TFD_CLOEXEC) };
    if fd < 0 {
        return -1;
    }

    let spec = libc::itimerspec {
        it_interval: libc::timespec { tv_sec: interval_secs, tv_nsec: 0 },
        it_value: libc::timespec { tv_sec: interval_secs, tv_nsec: 0 },
    };

    if unsafe { libc::timerfd_settime(fd, 0, &spec, std::ptr::null_mut()) } < 0 {
        unsafe { libc::close(fd) };
        return -1;
    }

    fd
}

/// Set up inotify watching the config directory for file writes.
fn setup_inotify(paths: &Paths) -> i32 {
    let fd = unsafe { libc::inotify_init1(libc::IN_CLOEXEC) };
    if fd < 0 {
        return -1;
    }

    let dir = match paths.override_file.parent() {
        Some(d) => d,
        None => {
            unsafe { libc::close(fd) };
            return -1;
        }
    };

    let dir_cstr = match CString::new(dir.to_string_lossy().as_bytes()) {
        Ok(c) => c,
        Err(_) => {
            unsafe { libc::close(fd) };
            return -1;
        }
    };

    let wd = unsafe {
        libc::inotify_add_watch(
            fd,
            dir_cstr.as_ptr(),
            libc::IN_CLOSE_WRITE,
        )
    };
    if wd < 0 {
        unsafe { libc::close(fd) };
        return -1;
    }

    fd
}

/// Block SIGTERM/SIGINT and create a signalfd for clean shutdown.
fn setup_signalfd() -> i32 {
    unsafe {
        let mut mask: libc::sigset_t = std::mem::zeroed();
        libc::sigemptyset(&mut mask);
        libc::sigaddset(&mut mask, libc::SIGTERM);
        libc::sigaddset(&mut mask, libc::SIGINT);

        if libc::sigprocmask(libc::SIG_BLOCK, &mask, std::ptr::null_mut()) < 0 {
            return -1;
        }

        libc::signalfd(-1, &mask, libc::SFD_CLOEXEC)
    }
}

/// Poll on timerfd + inotify + signalfd. Parse inotify events to determine
/// which config files changed. Returns immediately on signal or inotify event,
/// otherwise blocks until timerfd fires (or timeout_secs if no timerfd).
fn wait_for_event(
    timer_fd: i32,
    ino_fd: i32,
    signal_fd: i32,
    timeout_secs: i64,
    paths: &Paths,
) -> EventResult {
    let mut pfds: [libc::pollfd; 3] = unsafe { std::mem::zeroed() };
    let mut nfds: usize = 0;
    let mut timer_idx = usize::MAX;
    let mut ino_idx = usize::MAX;
    let mut sig_idx = usize::MAX;

    if timer_fd >= 0 {
        timer_idx = nfds;
        pfds[nfds] = libc::pollfd { fd: timer_fd, events: libc::POLLIN as i16, revents: 0 };
        nfds += 1;
    }
    if ino_fd >= 0 {
        ino_idx = nfds;
        pfds[nfds] = libc::pollfd { fd: ino_fd, events: libc::POLLIN as i16, revents: 0 };
        nfds += 1;
    }
    if signal_fd >= 0 {
        sig_idx = nfds;
        pfds[nfds] = libc::pollfd { fd: signal_fd, events: libc::POLLIN as i16, revents: 0 };
        nfds += 1;
    }

    // If timerfd available, block indefinitely (timer provides the wake).
    // Otherwise fall back to poll timeout.
    let timeout_ms = if timer_fd >= 0 { -1 } else { (timeout_secs * 1000) as libc::c_int };

    if nfds > 0 {
        unsafe { libc::poll(pfds.as_mut_ptr(), nfds as libc::nfds_t, timeout_ms) };
    } else {
        std::thread::sleep(std::time::Duration::from_secs(timeout_secs as u64));
    }

    let mut result = EventResult::default();

    // Drain timerfd
    if timer_idx < nfds && pfds[timer_idx].revents & libc::POLLIN as i16 != 0 {
        let mut exp: u64 = 0;
        unsafe { libc::read(timer_fd, &mut exp as *mut u64 as *mut libc::c_void, 8) };
    }

    // Check signalfd
    if sig_idx < nfds && pfds[sig_idx].revents & libc::POLLIN as i16 != 0 {
        let mut buf = [0u8; 128]; // sizeof(signalfd_siginfo) = 128
        unsafe { libc::read(signal_fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len()) };
        result.signal_received = true;
    }

    // Parse inotify events -- discriminate which file changed
    if ino_idx < nfds && pfds[ino_idx].revents & libc::POLLIN as i16 != 0 {
        let mut buf = [0u8; 4096];
        let len = unsafe {
            libc::read(ino_fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len())
        };
        if len > 0 {
            parse_inotify_events(&buf[..len as usize], paths, &mut result);
        }
    }

    result
}

/// Parse inotify event buffer, comparing each event's filename against known
/// config files. Sets override_changed / config_changed accordingly.
fn parse_inotify_events(buf: &[u8], paths: &Paths, result: &mut EventResult) {
    // Extract known filenames from paths
    let override_name = paths.override_file.file_name().and_then(|n| n.to_str()).unwrap_or("override.json");
    let config_name = paths.config_file.file_name().and_then(|n| n.to_str()).unwrap_or("config.ini");

    // inotify_event layout: i32 wd, u32 mask, u32 cookie, u32 len, [u8] name
    const EVENT_HEADER_SIZE: usize = 16; // sizeof(inotify_event) without name
    let mut offset = 0;

    while offset + EVENT_HEADER_SIZE <= buf.len() {
        // Read len field (offset 12, u32)
        let name_len = u32::from_ne_bytes([
            buf[offset + 12], buf[offset + 13], buf[offset + 14], buf[offset + 15],
        ]) as usize;

        let event_size = EVENT_HEADER_SIZE + name_len;
        if offset + event_size > buf.len() {
            break;
        }

        if name_len > 0 {
            // Name is null-terminated within the name_len bytes
            let name_bytes = &buf[offset + EVENT_HEADER_SIZE..offset + event_size];
            let name_end = name_bytes.iter().position(|&b| b == 0).unwrap_or(name_bytes.len());
            if let Ok(name) = std::str::from_utf8(&name_bytes[..name_end]) {
                if name == override_name {
                    result.override_changed = true;
                }
                if name == config_name {
                    result.config_changed = true;
                }
            }
        }

        offset += event_size;
    }
}

struct LocalTime {
    hour: i32,
    min: i32,
    sec: i32,
}

fn local_time(epoch: i64) -> LocalTime {
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    let t = epoch as libc::time_t;
    unsafe { libc::localtime_r(&t, &mut tm) };
    LocalTime {
        hour: tm.tm_hour,
        min: tm.tm_min,
        sec: tm.tm_sec,
    }
}

/// Calculate solar temperature given current state.
fn solar_temperature(now: i64, lat: f64, lon: f64, weather: &Option<WeatherData>) -> i32 {
    let st = solar::sunrise_sunset(now, lat, lon);
    let is_dark = weather
        .as_ref()
        .map(|w| !w.has_error && w.cloud_cover >= CLOUD_THRESHOLD)
        .unwrap_or(false);

    let (min_from_sunrise, min_to_sunset) = if let Some(ref times) = st {
        (
            (now - times.sunrise) as f64 / 60.0,
            (times.sunset - now) as f64 / 60.0,
        )
    } else {
        (0.0, 0.0)
    };

    sigmoid::calculate_solar_temp(min_from_sunrise, min_to_sunset, is_dark)
}

pub fn run(location: Location, paths: &Paths) {
    // Initialize gamma with retries
    let mut gamma_state = None;
    for attempt in 0..GAMMA_INIT_MAX_RETRIES {
        match gamma::init() {
            Ok(state) => {
                gamma_state = Some(state);
                break;
            }
            Err(e) => {
                if attempt == GAMMA_INIT_MAX_RETRIES - 1 {
                    eprintln!("[fatal] No gamma backend after 30s: {}", e);
                    std::process::exit(1);
                }
                std::thread::sleep(std::time::Duration::from_millis(GAMMA_INIT_RETRY_MS));
            }
        }
    }

    // Load initial weather
    let weather = config::load_weather_cache(paths);

    let mut state = DaemonState {
        location,
        paths: paths.clone(),
        weather,
        gamma: gamma_state,
        manual_mode: false,
        manual_start_temp: 0,
        manual_target_temp: 0,
        manual_start_time: 0,
        manual_duration_min: 0,
        manual_issued_at: 0,
        manual_resume_time: 0,
        last_temp: 0,
        last_temp_valid: false,
    };

    // Create kernel fds
    let timer_fd = setup_timerfd(TEMP_UPDATE_SEC);
    let ino_fd = setup_inotify(&state.paths);
    let signal_fd = setup_signalfd();

    // Write PID file
    if let Err(e) = config::write_pid(&state.paths) {
        eprintln!("[warn] Failed to write PID file: {}", e);
    }

    eprintln!(
        "[abraxas] daemon started (backend: {}, timerfd: {}, inotify: {}, signalfd: {})",
        state
            .gamma
            .as_ref()
            .map(|g| g.backend_name())
            .unwrap_or("none"),
        if timer_fd >= 0 { "active" } else { "unavailable" },
        if ino_fd >= 0 { "active" } else { "unavailable" },
        if signal_fd >= 0 { "active" } else { "unavailable" },
    );

    // Recover from active override on restart
    recover_override(&mut state);

    // Apply gamma immediately at startup (force override check)
    tick(&mut state, &EventResult { override_changed: true, ..Default::default() });

    // Initialize weather subsystem
    weather::init();

    // Event loop
    loop {
        let event = wait_for_event(timer_fd, ino_fd, signal_fd, TEMP_UPDATE_SEC, &state.paths);
        if event.signal_received {
            break;
        }
        tick(&mut state, &event);
    }

    // Clean shutdown
    eprintln!("[abraxas] shutting down...");
    weather::cleanup();
    if let Some(ref mut g) = state.gamma {
        let _ = g.restore();
    }
    config::remove_pid(&state.paths);

    if timer_fd >= 0 { unsafe { libc::close(timer_fd) }; }
    if ino_fd >= 0 { unsafe { libc::close(ino_fd) }; }
    if signal_fd >= 0 { unsafe { libc::close(signal_fd) }; }
}

/// Recover from an active override that was in progress before daemon restart.
fn recover_override(state: &mut DaemonState) {
    let ovr = match config::load_override(&state.paths) {
        Some(o) => o,
        None => return,
    };

    if !ovr.active {
        return;
    }

    let now = now_epoch();
    let elapsed_min = (now - ovr.issued_at) as f64 / 60.0;

    if elapsed_min >= ovr.duration_minutes as f64 {
        // Override already completed before restart -- discard
        config::clear_override(&state.paths);
        eprintln!(
            "[manual] Cleared stale override (completed {:.0} min ago)",
            elapsed_min - ovr.duration_minutes as f64
        );
        return;
    }

    // Still active -- recover state
    state.manual_mode = true;
    state.manual_target_temp = ovr.target_temp;
    state.manual_duration_min = ovr.duration_minutes;
    state.manual_issued_at = ovr.issued_at;
    state.manual_start_time = ovr.issued_at;

    state.manual_start_temp = if ovr.start_temp != 0 {
        ovr.start_temp
    } else {
        let temp = solar_temperature(now, state.location.lat, state.location.lon, &state.weather);
        // Save start_temp back so subsequent restarts have it
        let updated = config::OverrideState {
            active: true,
            target_temp: ovr.target_temp,
            duration_minutes: ovr.duration_minutes,
            issued_at: ovr.issued_at,
            start_temp: temp,
        };
        let _ = config::save_override(&state.paths, &updated);
        temp
    };

    state.manual_resume_time = sigmoid::next_transition_resume(
        now, state.location.lat, state.location.lon,
    );

    eprintln!(
        "[manual] Recovered override: -> {}K ({} min)",
        state.manual_target_temp, state.manual_duration_min
    );
}

fn tick(state: &mut DaemonState, event: &EventResult) {
    let now = now_epoch();

    // Check for override changes -- ONLY when inotify detected a change
    if event.override_changed {
        let ovr = config::load_override(&state.paths);
        if let Some(ref o) = ovr {
            if o.active {
                if !state.manual_mode || o.issued_at != state.manual_issued_at {
                    // New or changed override
                    state.manual_mode = true;
                    state.manual_target_temp = o.target_temp;
                    state.manual_duration_min = o.duration_minutes;
                    state.manual_start_time = o.issued_at;
                    state.manual_issued_at = o.issued_at;
                    state.manual_start_temp = if state.last_temp_valid {
                        state.last_temp
                    } else {
                        o.target_temp
                    };

                    // Save start_temp back
                    if o.start_temp == 0 {
                        let updated = config::OverrideState {
                            start_temp: state.manual_start_temp,
                            ..*o
                        };
                        let _ = config::save_override(&state.paths, &updated);
                    }

                    state.manual_resume_time = sigmoid::next_transition_resume(
                        now, state.location.lat, state.location.lon,
                    );

                    if state.manual_duration_min > 0 {
                        eprintln!(
                            "[manual] Override: {}K -> {}K over {} min",
                            state.manual_start_temp, state.manual_target_temp,
                            state.manual_duration_min
                        );
                    } else {
                        eprintln!("[manual] Override: -> {}K (instant)", state.manual_target_temp);
                    }
                }
            } else if state.manual_mode {
                state.manual_mode = false;
                state.manual_issued_at = 0;
                config::clear_override(&state.paths);
                eprintln!("[manual] Override cleared, resuming solar control");
            }
        }
    }

    // Reload config if inotify detected a config file change
    if event.config_changed {
        if let Some(new_loc) = config::load_location(&state.paths) {
            state.location = new_loc;
            eprintln!(
                "[config] Location updated: {:.4}, {:.4}",
                state.location.lat, state.location.lon
            );
        }
        state.weather = config::load_weather_cache(&state.paths);
    }

    // Refresh weather if needed (gated by timestamp, no unnecessary file I/O)
    if let Some(ref w) = state.weather {
        if config::weather_needs_refresh(w) {
            let wd = weather::fetch(state.location.lat, state.location.lon);
            if !wd.has_error {
                let _ = config::save_weather_cache(&state.paths, &wd);
            }
            state.weather = Some(wd);
        }
    } else {
        let wd = weather::fetch(state.location.lat, state.location.lon);
        if !wd.has_error {
            let _ = config::save_weather_cache(&state.paths, &wd);
        }
        state.weather = Some(wd);
    }

    // Calculate target temperature
    let target_temp = if state.manual_mode {
        let temp = sigmoid::calculate_manual_temp(
            state.manual_start_temp,
            state.manual_target_temp,
            state.manual_start_time,
            state.manual_duration_min,
            now,
        );

        // Check auto-resume: after manual transition completes, resume solar
        // control when the next dawn/dusk transition window approaches
        let elapsed_min = (now - state.manual_start_time) as f64 / 60.0;
        if elapsed_min >= state.manual_duration_min as f64
            && state.manual_resume_time > 0
            && now >= state.manual_resume_time
        {
            state.manual_mode = false;
            state.manual_issued_at = 0;
            config::clear_override(&state.paths);
            eprintln!("[manual] Auto-resuming solar control (transition window approaching)");
            solar_temperature(now, state.location.lat, state.location.lon, &state.weather)
        } else {
            temp
        }
    } else {
        solar_temperature(now, state.location.lat, state.location.lon, &state.weather)
    };

    // Apply if changed
    if !state.last_temp_valid || target_temp != state.last_temp {
        let lt = local_time(now);

        if state.manual_mode {
            let elapsed_min = (now - state.manual_start_time) as f64 / 60.0;
            if elapsed_min < state.manual_duration_min as f64 {
                let pct = (elapsed_min / state.manual_duration_min as f64 * 100.0) as i32;
                let pct = pct.min(100);
                eprintln!(
                    "[{:02}:{:02}:{:02}] Manual: {}K ({}%)",
                    lt.hour, lt.min, lt.sec, target_temp, pct
                );
            } else {
                eprintln!(
                    "[{:02}:{:02}:{:02}] Manual: {}K (holding)",
                    lt.hour, lt.min, lt.sec, target_temp
                );
            }
        } else {
            let sp = solar::position(now, state.location.lat, state.location.lon);
            let cloud_cover = state.weather.as_ref().map(|w| w.cloud_cover).unwrap_or(0);
            eprintln!(
                "[{:02}:{:02}:{:02}] Solar: {}K (sun: {:.1}, clouds: {}%)",
                lt.hour, lt.min, lt.sec, target_temp, sp.elevation, cloud_cover
            );
        }

        if let Some(ref mut g) = state.gamma {
            if g.set_temperature(target_temp, 1.0).is_ok() {
                state.last_temp = target_temp;
                state.last_temp_valid = true;
            }
        }
    }
}
