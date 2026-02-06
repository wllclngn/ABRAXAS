# ABRAXAS

A single-binary C23 daemon that smoothly adjusts your screen's color temperature throughout the day based on the sun's position at your location. No cron jobs, no geoclue, no shared libraries to install.

**Key Features:**

### Solar Grayline Engine
- **NOAA Ephemeris**: Offline sunrise/sunset from Jean Meeus algorithms -- pure trig, no network call
- **Sigmoid Transitions**: Normalized sigmoid over 90-min dawn and 120-min dusk windows, imperceptible at both ends
- **Endpoint Normalization**: Exact [0, 1] output over [-1, 1] domain -- no residual drift at target temperatures
- **Weather Awareness**: NOAA api.weather.gov cloud cover shifts daytime target (6500K clear, 4500K overcast)
- **Manual Override**: `--set TEMP MINUTES` with the same sigmoid curve, auto-resumes at next transition window

### libmeridian (Gamma Control)
- **4 Backends**: Wayland (wlr-gamma-control), GNOME (Mutter DBus), DRM (kernel ioctl), X11 (RandR)
- **Auto-Detection**: Runtime probe based on `$WAYLAND_DISPLAY`, compositor availability, DRM access
- **Statically Linked**: Single 75K binary, no .so install, no RPATH, no ctypes
- **Blackbody Ramp**: Planckian locus approximation, 1000K-25000K

### Kernel Event Loop
- **timerfd**: 60-second update ticks (no `sleep()` drift)
- **inotify**: Config file hot-reload, override detection
- **signalfd**: Clean SIGTERM/SIGINT shutdown via `select()`
- **Zero Polling**: CPU usage ~180ms over 3 hours

### C23
- **Custom JSON Parser**: RFC 8259 recursive descent with dot-path navigation, zero vendored code
- **Compile-Time Safety**: `constexpr`, `[[nodiscard]]`, `static_assert`, `nullptr`, native `bool`
- **Zero Warnings**: `-std=c2x -Wall -Wextra -Wpedantic`

---

## Modernizing redshift + redshift-scheduler

| Feature | redshift | redshift-scheduler | ABRAXAS |
|---------|----------|--------------------|---------|
| Set gamma ramp | Yes | No | Yes |
| Automatic day/night transitions | Manual via `-t` | Cron-based | Built-in sigmoid transitions |
| Sun position calculation | Via geoclue/manual | External | Built-in NOAA ephemeris |
| Weather awareness | No | No | Yes (NOAA api.weather.gov) |
| Smooth transitions | Step function (instant) | N/A | Normalized sigmoid over 90-120 min windows |
| Manual override with gradient | No (`-O` is instant) | No | `--set TEMP MINUTES` |
| Cloud cover dark mode | No | No | Yes (75% threshold) |
| DRM kernel gamma | No (X11 only) | No | Yes (direct ioctl) |
| Wayland native | No | No | Yes (wlr-gamma-control + Mutter DBus) |
| NVIDIA support | Via X11 | N/A | X11/RandR fallback |
| Dependencies | geoclue, X11 | cron, redshift | libc, libm (all backends optional) |
| Scheduler needed | Yes | Is the scheduler | No |

## Architecture

```
abraxas (C23, single binary -- libmeridian statically linked)
    |
    +-- NOAA sun ephemeris (offline calculation)
    +-- Weather from api.weather.gov (15-min refresh)
    +-- Sigmoid transition engine
    +-- Custom RFC 8259 JSON parser (no vendored code)
    |
    +-- libmeridian.a (C23, statically linked)
            |
            +-- Wayland backend: wlr-gamma-control protocol
            |   (Sway, Hyprland, river, labwc, wayfire, niri)
            |
            +-- GNOME backend: Mutter DBus (SetCrtcGamma)
            |   (GNOME Wayland on Debian, Ubuntu, Fedora)
            |
            +-- DRM backend: raw kernel ioctl to /dev/dri/card*
            |   (AMD, Intel, Nouveau -- no libdrm dependency)
            |
            +-- X11 backend: XRandR gamma
                (NVIDIA proprietary driver fallback)
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
  6500 |         ___________
       |        /                          <- sigmoid (dusk)
       |       /
       |      |
  4700 |     |
       |    /
       |___/
  2900 |_________________________________________
       sunset-60   sunset    sunset+60     Time

  6500 |__________
       |          |                        <- step function
       |          |
       |          |
  4700 |          |
       |          |
       |          |_________________________
  2900 |_________________________________________
       sunset-60   sunset    sunset+60     Time
```

**Dusk** (canonical): 120-minute window centered on sunset. `x` sweeps `+1` to `-1`, mapping day temp through the sigmoid to night temp. **Dawn** (inverse): 90-minute window centered on sunrise. `x` sweeps `-1` to `+1`. Same function, reversed direction.

**Manual overrides** use the same curve. `abraxas --set 3500 30` maps [0, 30 min] to [-1, 1] and interpolates between current temperature and 3500K.

**Endpoint normalization.** A raw sigmoid with k=6 outputs 0.0025 at x=-1 and 0.9975 at x=+1. Over a 3600K range that's 9K of drift at the boundaries. The output is normalized to hit exact 0.0 and 1.0 at window edges -- no residual error at target temperatures.

## Manual Override

When `--set TEMP MINUTES` is called, the daemon enters manual mode:

1. The sigmoid begins transitioning from current temperature to the target over the specified duration
2. A manual flag is set -- the daemon stops evaluating solar/weather conditions
3. After reaching the target, the daemon holds until the next natural transition point (e.g., 15 minutes before the next sunrise)
4. At that point, the flag clears and solar-based control resumes

The manual call communicates with the running daemon via a control file (watched by inotify). The daemon itself drives the gamma -- no second process.

## Weather Awareness

Every 15 minutes, ABRAXAS fetches the hourly forecast from `api.weather.gov` (NOAA, US only). If cloud cover exceeds 75%, daytime temperature drops from 6500K to 4500K ("dark mode"). This prevents eye strain on overcast days when the ambient light is already dim.

The weather API requires no API key. Rate limits are generous (per User-Agent).

## Installation

### Dependencies

```bash
# Required: build tools + libcurl
pacman -S gcc make pkg-config curl

# Optional: Wayland backend (Sway, Hyprland, river, etc.)
pacman -S wayland wayland-protocols

# Optional: GNOME backend (GNOME Wayland)
pacman -S systemd-libs

# Optional: X11 fallback (NVIDIA users)
pacman -S libx11 libxrandr
```

No runtime dependencies beyond libc, libm, and libcurl. All backends are optional and auto-detected at build time.

### Build & Install

```bash
# Automated installer (builds everything, copies binary, enables service)
./install.py

# Or manually:
make
mkdir -p ~/.local/bin ~/.config/abraxas
cp abraxas ~/.local/bin/
cp us_zipcodes.bin ~/.config/abraxas/
cp abraxas.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now abraxas.service
```

### Setup

```bash
# Set location (US ZIP code or lat,lon)
abraxas --set-location 60614
abraxas --set-location 41.88,-87.63

# Verify
abraxas --status
```

### Migrating from redshift

```bash
# Disable redshift
systemctl --user disable --now redshift.service redshift-scheduler.service 2>/dev/null

# Enable ABRAXAS
systemctl --user enable --now abraxas.service
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
| `us_zipcodes.bin` | ZIP code database (33k entries, 429 KB) |

### Tuning

Edit the constants in `include/abraxas.h` and rebuild:

```c
constexpr int TEMP_DAY_CLEAR = 6500;    // Clear sky daytime temperature (K)
constexpr int TEMP_DAY_DARK  = 4500;    // Overcast daytime temperature (K)
constexpr int TEMP_NIGHT     = 2900;    // Night temperature (K)
constexpr int CLOUD_THRESHOLD = 75;     // Cloud cover % to trigger dark mode
constexpr int WEATHER_REFRESH_SEC = 900; // 15 minutes
constexpr int TEMP_UPDATE_SEC = 60;
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
gcc -Ilibmeridian/include myapp.c libmeridian/libmeridian.a -lm $(pkg-config --libs ...) -o myapp
```

See `libmeridian/include/meridian.h` for the full API.

## Platform Support

- **Linux only**.
- **Wayland (wlr)**: Native gamma control on Sway, Hyprland, river, labwc, wayfire, niri
- **GNOME Wayland**: Mutter DBus gamma control (Debian, Ubuntu, Fedora defaults)
- **AMD/Intel/Nouveau**: DRM backend (pure kernel, no compositor needed)
- **NVIDIA proprietary**: X11/RandR fallback (requires X11 libs)
- **US locations**: Weather via api.weather.gov (NOAA). Non-US locations work for solar calculations but weather features require a US grid point.

## License

MIT
