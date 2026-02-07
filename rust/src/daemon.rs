//! Daemon event loop.
//!
//! Linux kernel interfaces: io_uring (60s timeout + poll), inotify (config
//! changes), signalfd (clean shutdown via SIGTERM/SIGINT). Single
//! io_uring_enter per tick. Gamma control via auto-detected backend.

use crate::config::{self, Location, Paths, WeatherData};
use crate::{
    sigmoid, solar, weather, CLOUD_THRESHOLD, TEMP_UPDATE_SEC, now_epoch,
    landlock, seccomp,
};
use crate::weather::FetchState;
use crate::gamma;
use crate::uring::{self, AbraxasRing, KernelTimespec};

use std::ffi::CString;
use std::sync::atomic::{AtomicBool, Ordering};

const GAMMA_INIT_MAX_RETRIES: i32 = 60;
const GAMMA_INIT_RETRY_MS: u64 = 500;

/// Event discrimination from inotify
#[derive(Default)]
struct EventResult {
    override_changed: bool,
    config_changed: bool,
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
    let t = epoch;
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

/// io_uring event loop: single io_uring_enter per tick.
fn event_loop_uring(
    state: &mut DaemonState,
    ring: &mut AbraxasRing,
    ino_fd: i32,
    signal_fd: i32,
) {
    let ts = KernelTimespec {
        tv_sec: TEMP_UPDATE_SEC,
        tv_nsec: 0,
    };

    let mut wfs = FetchState::new();

    loop {
        // Submit: poll both fds + timeout
        if ino_fd >= 0 {
            ring.prep_poll(ino_fd, uring::EV_INOTIFY);
        }
        if signal_fd >= 0 {
            ring.prep_poll(signal_fd, uring::EV_SIGNAL);
        }
        if wfs.needs_poll() {
            ring.prep_poll(wfs.pipe_fd, uring::EV_WEATHER);
        }
        ring.prep_timeout(&ts, uring::EV_TIMEOUT);

        let ret = ring.submit_and_wait();
        if ret < 0 {
            break;
        }

        // Process completions
        let mut timer_expired = false;
        let mut got_signal = false;
        let weather_ready = AtomicBool::new(false);
        let mut event = EventResult::default();

        while let Some(cqe) = ring.peek_cqe() {
            match cqe.user_data {
                uring::EV_TIMEOUT => timer_expired = true,
                uring::EV_SIGNAL => got_signal = true,
                uring::EV_INOTIFY => {
                    if cqe.res > 0 {
                        parse_inotify_fd(ino_fd, &state.paths, &mut event);
                    }
                }
                uring::EV_WEATHER => {
                    if cqe.res > 0 { weather_ready.store(true, Ordering::Relaxed); }
                }
                uring::EV_CANCEL => {}
                _ => {}
            }
            ring.cqe_seen();
        }

        // If we woke early (inotify/signal), cancel the pending timeout
        if !timer_expired {
            ring.prep_cancel(uring::EV_TIMEOUT, uring::EV_CANCEL);
            ring.submit_and_wait();
            // Drain cancel completion
            while ring.peek_cqe().is_some() {
                ring.cqe_seen();
            }
        }

        if got_signal {
            // Drain signalfd
            if signal_fd >= 0 {
                let mut buf = [0u8; 128];
                unsafe {
                    libc::read(signal_fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len());
                }
            }
            eprintln!("\nReceived shutdown signal...");
            wfs.abort();
            break;
        }

        tick(state, &event);

        // Async weather fetch (non-blocking, io_uring integrated)
        #[cfg(feature = "noaa")]
        {
            use crate::weather::{FetchPhase, ReadResult};

            if wfs.phase == FetchPhase::Idle {
                let needs = if let Some(ref w) = state.weather {
                    config::weather_needs_refresh(w)
                } else {
                    true
                };
                if needs {
                    let lt = local_time(now_epoch());
                    eprintln!(
                        "[{:02}:{:02}:{:02}] Starting weather fetch...",
                        lt.hour, lt.min, lt.sec
                    );
                    wfs.start(state.location.lat, state.location.lon);
                }
            }

            if weather_ready.load(Ordering::Relaxed) {
                match wfs.read_response() {
                    ReadResult::Pending => {}
                    ReadResult::NewPipe => {}
                    ReadResult::Done(result) => {
                        match result {
                            Ok(wd) => {
                                let _ = config::save_weather_cache(&state.paths, &wd);
                                eprintln!(
                                    "  Weather: {} ({}% clouds)",
                                    wd.forecast, wd.cloud_cover
                                );
                                state.weather = Some(wd);
                            }
                            Err(_) => {
                                eprintln!("  Weather fetch failed");
                                state.weather = Some(WeatherData {
                                    cloud_cover: 0,
                                    forecast: "Unknown".to_string(),
                                    temperature: 0.0,
                                    is_day: true,
                                    fetched_at: now_epoch(),
                                    has_error: true,
                                });
                            }
                        }
                    }
                }
            }
        }
    }
}

/// Read and parse inotify events from fd (used by io_uring path).
fn parse_inotify_fd(fd: i32, paths: &Paths, result: &mut EventResult) {
    let mut buf = [0u8; 4096];
    let len = unsafe { libc::read(fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len()) };
    if len > 0 {
        parse_inotify_events(&buf[..len as usize], paths, result);
    }
}

pub fn run(location: Location, paths: &Paths) {
    // Block SIGTERM/SIGINT immediately and create signalfd.
    // Must happen before gamma retry so SIGTERM is never lost during init.
    let signal_fd = setup_signalfd();

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
                // Check for SIGTERM between retries (non-blocking)
                if signal_fd >= 0 {
                    let mut pfd = libc::pollfd {
                        fd: signal_fd,
                        events: libc::POLLIN,
                        revents: 0,
                    };
                    if unsafe { libc::poll(&mut pfd, 1, 0) } > 0 {
                        eprintln!("Received signal during gamma init, exiting...");
                        unsafe { libc::close(signal_fd) };
                        std::process::exit(0);
                    }
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
    let ino_fd = setup_inotify(&state.paths);

    // Write PID file
    if let Err(e) = config::write_pid(&state.paths) {
        eprintln!("[warn] Failed to write PID file: {}", e);
    }

    // prctl hardening
    unsafe {
        libc::prctl(libc::PR_SET_TIMERSLACK, 1); // 1ns timer precision
        libc::prctl(libc::PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0); // required for seccomp
        libc::prctl(libc::PR_SET_DUMPABLE, 0); // no core dumps, no ptrace
    }
    eprintln!("[kernel] prctl: timerslack=1ns, no_new_privs, !dumpable");

    // Landlock filesystem sandbox
    let config_dir = state.paths.override_file.parent()
        .map(|p| p.to_string_lossy().to_string())
        .unwrap_or_default();
    if !config_dir.is_empty() {
        if landlock::install_sandbox(&config_dir) {
            eprintln!("[kernel] landlock: filesystem sandbox active");
        } else {
            eprintln!("[kernel] landlock: unavailable (running unsandboxed)");
        }
    }

    // seccomp-bpf syscall whitelist (must be last -- no new syscalls after this)
    if seccomp::install_filter() {
        eprintln!("[kernel] seccomp: syscall whitelist active (~81 syscalls)");
    } else {
        eprintln!("[kernel] seccomp: failed to install filter");
    }

    // Recover from active override on restart
    recover_override(&mut state);

    // Apply gamma immediately at startup (force override check)
    tick(&mut state, &EventResult { override_changed: true, ..Default::default() });

    // Initialize weather subsystem
    weather::init();

    // io_uring event loop (no fallback -- requires kernel >= 5.1)
    let mut ring = match AbraxasRing::init(8) {
        Some(r) => r,
        None => {
            eprintln!("[fatal] io_uring_setup failed (kernel >= 5.1 required)");
            std::process::exit(1);
        }
    };
    eprintln!(
        "[abraxas] daemon started (backend: {}, io_uring: active, inotify: {}, signalfd: {})",
        state.gamma.as_ref().map(|g| g.backend_name()).unwrap_or("none"),
        if ino_fd >= 0 { "active" } else { "unavailable" },
        if signal_fd >= 0 { "active" } else { "unavailable" },
    );
    event_loop_uring(&mut state, &mut ring, ino_fd, signal_fd);

    // Clean shutdown
    eprintln!("[abraxas] shutting down...");
    weather::cleanup();
    if let Some(ref mut g) = state.gamma {
        let _ = g.restore();
    }
    config::remove_pid(&state.paths);

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

    // Weather refresh is now async via io_uring POLL_ADD in event_loop_uring()

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
