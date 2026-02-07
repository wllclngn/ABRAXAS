# ABRAXAS

A daemon available in C23 or Rust that smoothly adjusts your screen's color temperature throughout the day based on the sun's position at your location. ABRAXAS allows for USA-specific builds (alongside manual calls for international users) which utilize NOAA data to further assist in screen temperature shifts during non-ideal weather.

**Key Features:**

### Solar Grayline Engine
- **Worldwide Usage**: Offline sunrise/sunset from Jean Meeus algorithms -- pure trig, no network call, any latitude/longitude
- **Sigmoid Transitions**: Normalized sigmoid over 90-min dawn and 180-min dusk windows (front-loaded 30 min before sunset, k=8), imperceptible at both ends
- **Endpoint Normalization**: Exact [0, 1] output over [-1, 1] domain -- no residual drift at target temperatures
- **Weather Awareness (US, optional)**: NOAA api.weather.gov cloud cover shifts daytime target (6500K clear, 4500K overcast). See [Build & Install](#build--install) for international builds
- **Manual Override**: `--set TEMP MINUTES` with the same sigmoid curve, auto-resumes at next transition window

### libmeridian (Gamma Control)
- **4 Backends**: Wayland (wlr-gamma-control), GNOME (Mutter DBus), DRM (kernel ioctl), X11 (RandR)
- **Auto-Detection**: Runtime probe based on `$WAYLAND_DISPLAY`, compositor availability, DRM access
- **Per-Backend Diagnostics**: Each backend logs why it succeeded or failed during probe
- **Runtime Loading**: X11 and GNOME backends load libraries via dlopen. Wayland backend is a separate .so plugin. CLI commands load zero backend code.
- **Blackbody Ramp**: Planckian locus approximation, 1000K-25000K

### Daemon Reliability
- **PID File Liveness**: Daemon writes PID on start, CLI commands check liveness before reporting success
- **Instant Startup**: Gamma applied before weather init -- screen is correct on first frame
- **io_uring Event Loop** (C23): 1 syscall per 60s tick via `IORING_OP_POLL_ADD` + `IORING_OP_TIMEOUT`. Automatic fallback to select()+timerfd on older kernels
- **inotify**: Config file hot-reload via IN_CLOSE_WRITE (no spurious partial-write triggers)
- **signalfd**: Clean SIGTERM/SIGINT shutdown
- **seccomp-bpf** (C23): ~75 whitelisted syscalls, KILL_PROCESS on violation. Raw BPF, no libseccomp
- **landlock** (C23): Filesystem sandboxed to config dir, /dev, /proc, /usr, /etc, /lib, /tmp
- **prctl hardening** (C23): 1ns timer slack, no-new-privs, non-dumpable
- **Temperature Logging**: Every tick logs current mode, temperature, sun position, and cloud cover to stderr
- **Zero Polling**: CPU usage ~180ms over 3 hours

### C23
- **Custom JSON Parser**: RFC 8259 recursive descent with dot-path navigation, zero vendored code
- **Compile-Time Safety**: `constexpr`, `[[nodiscard]]`, `static_assert`, `nullptr`, native `bool`
- **Zero Warnings**: `-std=c2x -Wall -Wextra -Wpedantic`
- **Zero Dependencies**: Binary loads only libc and libm. Weather via `posix_spawnp("curl")`. Backend libraries loaded on demand via dlopen.

---

## Modernizing redshift + redshift-scheduler

| Feature | redshift | redshift-scheduler | ABRAXAS |
|---------|----------|--------------------|---------|
| Set gamma ramp | Yes | No | Yes |
| Automatic day/night transitions | Manual via `-t` | Cron-based | Built-in sigmoid transitions |
| Sun position calculation | Via geoclue/manual | External | Built-in NOAA ephemeris |
| Weather awareness | No | No | Yes (NOAA api.weather.gov) |
| Smooth transitions | Step function (instant) | N/A | Front-loaded sigmoid over 90-180 min windows |
| Manual override with gradient | No (`-O` is instant) | No | `--set TEMP MINUTES` |
| Cloud cover dark mode | No | No | Yes (75% threshold) |
| DRM kernel gamma | No (X11 only) | No | Yes (direct ioctl) |
| Wayland native | No | No | Yes (wlr-gamma-control + Mutter DBus) |
| NVIDIA support | Via X11 | N/A | X11/RandR fallback |
| Dependencies | geoclue, X11 | cron, redshift | libc, libm (all backends runtime-loaded) |
| Scheduler needed | Yes | Is the scheduler | No |

## MAIN EVENT: C23 vs. Rust

Both implementations produce the same `abraxas` binary, same CLI interface, same config files, same systemd service. Pick at install time. Run `./test.py` for a head-to-head comparison across every subsystem.

### Implementation

|                  | C23                | Rust               |
|------------------|--------------------|--------------------|
| Compiler         | GCC 15 (-std=c2x)  | rustc 1.75+        |
| Source LOC        | ~5,900             | ~3,500             |
| Memory model     | manual alloc/free  | ownership/borrow   |
| Gamma: DRM       | raw ioctl          | raw ioctl          |
| Gamma: Wayland   | .so plugin (dlopen)| wayland-client     |
| Gamma: X11       | dlopen at runtime  | x11rb (pure Rust)  |
| Gamma: GNOME     | dlopen at runtime  | libsystemd/sd-bus  |
| Weather          | posix_spawnp curl(1)| ureq (5s timeout) |
| Config parse     | hand-rolled JSON   | serde_json         |
| Event loop       | io_uring (1 syscall/tick) | poll() on 3 fds (2 syscalls/tick) |
| Sandbox          | seccomp + landlock | none               |
| LTO              | -flto=auto + gc-sections | cargo (default) |
| Static build     | make static (musl) | N/A                |
| Heap allocs/tick | 0                  | 0                  |
| Default backends | auto (pkg-config)  | noaa + x11 (cargo) |

### Measured Performance (test.py, 2026-02-06)

|                     | C23          | Rust          | Notes                          |
|---------------------|--------------|---------------|--------------------------------|
| Binary size         | 69 KB        | 2,590 KB      | C23 wins: 37.5x smaller        |
| Binary (static musl)| 121 KB       | N/A           | Zero shared libs, DRM-only     |
| Shared libs (ldd)   | 3            | 5             | C23: libc, libm, ld-linux      |
| Startup (--help)    | **0.6 ms**   | 0.9 ms        | **C23 wins: 1.5x faster**      |
| --status            | **0.7 ms**   | 1.1 ms        | **C23 wins: 1.6x faster**      |
| --set               | **0.7 ms**   | 1.0 ms        | **C23 wins: 1.4x faster**      |
| Daemon memory       | **648 KB**   | ~14 MB        | **C23 wins: 22x less memory**  |
| Daemon peak memory  | 2.6 MB       | ~14 MB        | Peak during startup (dlopen)   |
| Syscalls/tick       | **1**        | 2             | io_uring vs poll+read          |
| Daemon threads      | 1            | 1             | Tie                            |
| CPU / hour          | < 1s         | < 1s          | Tie                            |
| Weather timeout     | 5s per req   | 5s per req    | Tie                            |

### v4.0.0 -> v4.1.0: How C23 Won

In v4.0.0, C23 linked 36 shared libraries at startup (libcurl's SSL/crypto chain + X11 + Wayland + systemd). Rust statically compiled everything into a single binary with only 4 shared libs. Result: Rust was 4.9x faster at startup (0.8ms vs 3.9ms).

v4.1.0 eliminated every unnecessary dependency:

| Step | Shared libs | --help time | What changed |
|------|-------------|-------------|-------------|
| v4.0.0 | 36 | 3.9 ms | Everything linked at compile time |
| Drop libcurl | 15 | 2.0 ms | HTTP via posix_spawnp("curl") instead of libcurl |
| dlopen X11 + GNOME | 5 | ~1 ms | Backend libs loaded on demand, not at startup |
| Wayland .so plugin | 3 | 0.7 ms | Wayland backend in separate meridian_wl.so |

The final C23 binary links only libc and libm. The dynamic linker resolves 3 libraries instead of 36. Backend libraries are loaded via dlopen only when the daemon actually probes for a display backend -- CLI commands like `--help`, `--status`, and `--set` never touch them.

### Test Suite

69 tests pass, 1 build warning check (pre-existing daemon.c FORTIFY_SOURCE diagnostic -- not a code error), 0 skipped. Full cross-compatibility verified: C23-written config and override files are correctly read by Rust and vice versa. Solar calculations agree to within 0K temperature, identical sunrise/sunset times, and matching sun elevation.

## Architecture

```
ABRAXAS/
  c23/              C23 implementation
  rust/             Rust implementation
  install.py        Installer (prompts C23 or Rust)
  test.py           Head-to-head test suite (70 tests)
  abraxas.service   Systemd user service
  us_zipcodes.bin   ZIP code database (shared)
```

### C23 implementation

```
abraxas (C23, single binary -- libmeridian statically linked)
    |
    +-- NOAA sun ephemeris (offline calculation)
    +-- Weather from api.weather.gov (posix_spawnp curl, 5s timeout)
    +-- Sigmoid transition engine
    +-- Custom RFC 8259 JSON parser (no vendored code)
    |
    +-- libmeridian.a (C23, statically linked)
    |       |
    |       +-- DRM backend: raw kernel ioctl to /dev/dri/card*
    |       +-- X11 backend: dlopen(libX11.so.6 + libXrandr.so.2)
    |       +-- GNOME backend: dlopen(libsystemd.so.0)
    |       +-- Auto-detect dispatcher (gamma_auto.c)
    |
    +-- meridian_wl.so (Wayland plugin, loaded via dlopen)
            |
            +-- Wayland backend: wlr-gamma-control protocol
            +-- Links -lwayland-client directly
```

### Rust implementation

```
abraxas (Rust, single binary -- all backends compiled in via cargo features)
    |
    +-- NOAA sun ephemeris (same algorithm, std::f64)
    +-- Weather from api.weather.gov (ureq)
    +-- Sigmoid transition engine (same curve)
    +-- Config via serde_json
    |
    +-- gamma module (no FFI to libmeridian)
            |
            +-- Wayland backend: wayland-client + wayland-protocols-wlr
            +-- GNOME backend: raw sd-bus FFI (same as C23)
            +-- DRM backend: raw kernel ioctl (same as C23)
            +-- X11 backend: x11rb (pure Rust X11 protocol, default feature)
```

## Sigmoid Transitions

All temperature transitions use a normalized sigmoid function:

```
            1
f(x) = -----------        normalized to exactly [0, 1] over x in [-1, 1]
        1 + e^(-kx)
```

A sigmoid changes slowly at the edges and quickly through the middle. This matches human color perception -- the transition is imperceptible at both ends, with ~80% of the shift happening through the middle third while ambient light is already changing.

A linear ramp has constant rate, which produces visible start and stop artifacts. A step function (the traditional approach) is a single-frame jump.

```
  Temp (K)
  6500 |            ___________
       |           /                          <- sigmoid (dusk)
       |          /
       |        |
  4700 |       |
       |      /
       |    /
       |___/
  2900 |_________________________________________
       sunset-90   sunset    sunset+90     Time

  6500 |__________
       |          |                        <- step function
       |          |
       |          |
  4700 |          |
       |          |
       |          |_________________________
  2900 |_________________________________________
       sunset-90   sunset    sunset+90     Time
```

**Dusk** (canonical): 180-minute window, front-loaded 30 minutes before sunset (runs from sunset-120min to sunset+60min). The sigmoid midpoint hits at sunset-30min, so the steep k=8 drop happens during golden hour -- by sunset you're already at ~3200K. **Dawn** (inverse): 90-minute window centered on sunrise. `x` sweeps `-1` to `+1`. Same function, no offset.

**Manual overrides** use the same curve. `abraxas --set 3500 30` maps [0, 30 min] to [-1, 1] and interpolates between current temperature and 3500K.

**Endpoint normalization.** A raw sigmoid with k=6 outputs 0.0025 at x=-1 and 0.9975 at x=+1. Over a 3600K range that's 9K of drift at the boundaries. The output is normalized to hit exact 0.0 and 1.0 at window edges -- no residual error at target temperatures.

## Manual Override

When `--set TEMP MINUTES` is called, the daemon enters manual mode:

1. The sigmoid begins transitioning from current temperature to the target over the specified duration
2. A manual flag is set -- the daemon stops evaluating solar/weather conditions
3. After reaching the target, the daemon holds until the next natural transition point (e.g., 15 minutes before the next sunrise)
4. At that point, the flag clears and solar-based control resumes

The manual call communicates with the running daemon via a control file (watched by inotify via IN_CLOSE_WRITE). The daemon itself drives the gamma -- no second process. If the daemon is not running, the CLI warns the user that the override was saved but won't apply until the daemon starts.

## Weather Awareness

Every 15 minutes, ABRAXAS fetches the hourly forecast from `api.weather.gov` (NOAA, US only). If cloud cover exceeds 75%, daytime temperature drops from 6500K to 4500K ("dark mode"). This prevents eye strain on overcast days when the ambient light is already dim.

The weather API requires no API key. Rate limits are generous (per User-Agent). Each request has a 5-second timeout (max 10s for both NOAA requests combined). C23 execs curl(1) via posix_spawnp (no libcurl linkage). Rust uses the ureq crate.

## Installation

### Dependencies

```bash
# C23: build tools
pacman -S gcc make pkg-config

# Rust: cargo
pacman -S rustup && rustup default stable

# Optional: curl binary (C23 weather -- usually pre-installed)
pacman -S curl

# Optional: Wayland backend
pacman -S wayland wayland-protocols

# Optional: GNOME backend
pacman -S systemd-libs

# Optional: X11 fallback (NVIDIA users)
pacman -S libx11 libxrandr
```

All backends are optional and auto-detected at build time. C23 backend libraries are loaded at runtime via dlopen -- they are build-time dependencies (for headers) but not runtime link dependencies. Rust defaults include NOAA and X11 (pure Rust x11rb, no system libs needed).

### Build & Install

```bash
# Automated installer (prompts C23 or Rust, then NOAA)
./install.py

# Non-interactive
./install.py --impl c23                 # C23 implementation
./install.py --impl rust                # Rust implementation
./install.py --impl c23 --non-usa       # C23 without NOAA
./install.py --impl rust --non-usa      # Rust without NOAA

# Show implementation comparison table
./install.py --test

# Or manually (C23):
make -C c23                             # US build
make -C c23 NOAA=0                      # International build
mkdir -p ~/.local/bin ~/.config/abraxas
cp c23/abraxas ~/.local/bin/
cp c23/meridian_wl.so ~/.local/bin/     # Wayland plugin (if built)

# Or manually (Rust):
cd rust && cargo build --release        # Defaults: noaa + x11
cp target/release/abraxas ~/.local/bin/
```

### Setup

```bash
# Set location (US ZIP code or lat,lon)
abraxas --set-location 60614
abraxas --set-location 41.88,-87.63

# International users: use lat,lon directly
abraxas --set-location 51.51,-0.13     # London
abraxas --set-location 48.86,2.35      # Paris

# Verify
abraxas --status
```

### Autostart

**Window managers (awesome, i3, sway, etc.):** Launch from your WM config. This guarantees the display server is ready -- gamma applies instantly on login.

```lua
-- awesome: rc.lua
awful.spawn("/home/USER/.local/bin/abraxas --daemon")
```
```bash
# i3/sway: config
exec --no-startup-id ~/.local/bin/abraxas --daemon
```

**Desktop environments (GNOME, KDE):** The systemd service works if your DE activates `graphical-session.target`:

```bash
cp abraxas.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now abraxas.service
```

### Migrating from redshift

```bash
systemctl --user disable --now redshift.service redshift-scheduler.service 2>/dev/null
```

No config migration needed. ABRAXAS calculates everything from your location.

## Usage

```
abraxas                       Run daemon (foreground)
abraxas --daemon              Run daemon (explicit)
abraxas --status              Show sun position, weather, current temperature
abraxas --set TEMP [MINUTES]  Transition to TEMP over MINUTES (default 3)
abraxas --resume              Clear manual override, resume solar control
abraxas --set-location LOC    Set location (ZIP code or LAT,LON)
abraxas --refresh             Force weather refresh from NOAA
abraxas --reset               Reset screen to default gamma and exit
```

### Examples

```bash
# Gradually shift to night mode over 30 minutes
abraxas --set 2900 30

# Quick warm shift for a movie
abraxas --set 3500 5

# Back to solar control
abraxas --resume

# Check what's happening
abraxas --status
```

## Configuration

All config lives in `~/.config/abraxas/`:

| File | Purpose |
|------|---------|
| `config.ini` | Location (latitude/longitude) |
| `weather_cache.json` | Cached NOAA forecast |
| `override.json` | Manual override state (daemon-managed) |
| `daemon.pid` | PID file for liveness checks |
| `us_zipcodes.bin` | ZIP code database (33k entries, 429 KB) |

### Tuning

Edit the constants in `include/abraxas.h` (C23) or `src/main.rs` (Rust) and rebuild:

```c
constexpr int TEMP_DAY_CLEAR = 6500;    // Clear sky daytime temperature (K)
constexpr int TEMP_DAY_DARK  = 4500;    // Overcast daytime temperature (K)
constexpr int TEMP_NIGHT     = 2900;    // Night temperature (K)
constexpr int CLOUD_THRESHOLD = 75;     // Cloud cover % to trigger dark mode
constexpr int WEATHER_REFRESH_SEC = 900; // 15 minutes
constexpr int TEMP_UPDATE_SEC = 60;
constexpr int DAWN_DURATION = 90;       // Dawn window (minutes, centered on sunrise)
constexpr int DUSK_DURATION = 180;      // Dusk window (minutes, total)
constexpr int DUSK_OFFSET   = 30;       // Shift sigmoid midpoint this many min before sunset
constexpr double SIGMOID_STEEPNESS = 8.0; // Higher = sharper mid-transition
```

## libmeridian C API

The library is usable standalone for any project that needs gamma control:

```c
#include <meridian.h>

meridian_state_t *state;
meridian_init(&state);                          // Auto-select best backend
meridian_set_temperature(state, 4500, 1.0f);    // Set 4500K, full brightness
meridian_restore(state);                        // Restore original gamma
meridian_free(state);                           // Clean up
```

Build against the static archive:
```bash
make -C libmeridian static
gcc -Ilibmeridian/include myapp.c libmeridian/libmeridian.a -lm -o myapp
```

See `libmeridian/include/meridian.h` for the full API.

## Platform Support

- **Linux only**.
- **Wayland (wlr)**: Native gamma control on Sway, Hyprland, river, labwc, wayfire, niri
- **GNOME Wayland**: Mutter DBus gamma control (Debian, Ubuntu, Fedora defaults)
- **AMD/Intel/Nouveau**: DRM backend (pure kernel, no compositor needed)
- **NVIDIA proprietary**: X11/RandR fallback (requires X11 libs at runtime)
- **International**: Solar calculations work worldwide. Build with `make NOAA=0` or `./install.py --non-usa` to skip NOAA weather.
- **US locations**: NOAA weather awareness via api.weather.gov (cloud cover adjusts daytime temperature). Enabled by default.

## License

MIT
