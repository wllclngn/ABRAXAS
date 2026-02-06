# ABRAXAS

Dynamic color temperature daemon for Linux. Replaces both **redshift** and **redshift-scheduler**.

Named for the Gnostic deity of transformation -- the one who bridges light and dark.

## What It Does

ABRAXAS adjusts your screen's color temperature throughout the day based on the sun's position at your location. At noon, your display is a cool 6500K. As the sun sets, ABRAXAS smoothly transitions through a sigmoid curve down to a warm 2900K for night. At dawn, it ramps back up.

Unlike redshift, which requires an external scheduler or manual `-O` calls, ABRAXAS is a single daemon that handles everything: sun position calculation, weather awareness, smooth transitions, and manual overrides.

### Replaces redshift + redshift-scheduler

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

ABRAXAS is a single binary + shared library. No cron jobs, no geoclue, no dbus.

## Architecture

```
abraxas (Python daemon)
    |
    +-- NOAA sun ephemeris (offline calculation)
    +-- Weather from api.weather.gov (15-min refresh)
    +-- Sigmoid transition engine
    |
    +-- libmeridian.so (C23)
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

### Event Loop

The daemon uses Linux kernel primitives for zero-poll operation:

- **timerfd** -- 60-second update ticks (no `sleep()` drift)
- **inotify** -- config file hot-reload
- **signalfd** -- clean SIGTERM/SIGINT shutdown via `select()`

CPU usage is ~180ms over 3 hours. The daemon is effectively invisible.

### libmeridian

A C23 shared library providing direct gamma ramp control. Four backends, all auto-detected at build time via pkg-config:

- **Wayland** (preferred on Wayland): Uses the `wlr-gamma-control-unstable-v1` protocol via `wayland-client`. Covers Sway, Hyprland, river, labwc, wayfire, niri. Protocol XML shipped in-tree.

- **GNOME** (Mutter fallback): Uses `org.gnome.Mutter.DisplayConfig.SetCrtcGamma` via `sd-bus`. Covers GNOME Wayland sessions (Debian, Ubuntu, Fedora). Requires `libsystemd`.

- **DRM** (always compiled): Opens `/dev/dri/card0` directly, uses `MODE_SETGAMMA` ioctl. Pure kernel interface -- no libdrm, no X11, no Wayland compositor needed. Works on AMD, Intel, and Nouveau. User must be in the `video` group.

- **X11/RandR** (last resort): For NVIDIA proprietary drivers where DRM gamma is locked. Uses `XRRSetCrtcGamma`.

Runtime detection order: if `$WAYLAND_DISPLAY` is set, try Wayland then GNOME. Then DRM. Then X11.

### Sigmoid Transitions (Why Not Linear)

redshift doesn't transition at all. It holds a day temp and a night temp and lets the user (or a cron scheduler) flip between them. `redshift -O 3500` is an instant hard-cut. redshift-scheduler wraps this with cron jobs that fire at fixed times, still producing step-function jumps between temperatures. Your screen goes from 6500K to 2900K in a single frame.

ABRAXAS uses a normalized sigmoid function for every temperature transition:

```
            1
f(x) = -----------        normalized to exactly [0, 1] over x in [-1, 1]
        1 + e^(-kx)
```

**Why sigmoid, not linear?** A linear ramp changes temperature at a constant rate. The problem is perceptual: humans notice color shifts most when they start and stop. A linear ramp begins abruptly (you see the shift kick in) and ends abruptly (you see it stop). A sigmoid is the opposite -- it changes slowly at the edges and quickly through the middle. The transition is imperceptible at both ends, with the bulk of the shift happening when your eyes are already adapting. You never notice it start. You never notice it stop. You just look up and the light is different.

**Dusk is the canonical transition.** A 120-minute window is centered on sunset. As the window progresses, `x` sweeps from `+1` (full day) to `-1` (full night) through the sigmoid. At x=+1 the function outputs exactly 1.0 (day temp). At x=0 (sunset) it outputs 0.5 (midpoint). At x=-1 it outputs exactly 0.0 (night temp). The curve is steepest through the middle third -- that's where 80% of the 3600K shift happens, fast enough that it tracks the actual fading light.

**Dawn is the inverse.** Same 90-minute window, centered on sunrise, but `x` sweeps from `-1` (night) to `+1` (day). The sigmoid naturally produces the mirror curve: slow departure from night, rapid climb through mid-tones, slow arrival at day. No special-casing. The math is identical -- only the direction of `x` changes.

**Manual overrides use the same curve.** When you run `abraxas --set 3500 30`, the sigmoid maps [0, 30 minutes] to [-1, 1] and interpolates between your current temperature and 3500K. Same function, same perceptual smoothness, over whatever duration you specify.

**Endpoint normalization.** A raw sigmoid with steepness k=6 outputs 0.0025 at x=-1 and 0.9975 at x=+1 -- close to [0, 1] but not exact. Over a 3600K range that's a 9K error at the boundaries. ABRAXAS normalizes the output to hit exactly 0.0 and 1.0 at the window edges, so the transition arrives precisely at the target temperature with no residual drift.

```
  Temp (K)
  6500 |         ___________
       |        /                          <- ABRAXAS sigmoid (dusk)
       |       /
       |      |
  4700 |     |
       |    /
       |___/
  2900 |_________________________________________
       sunset-60   sunset    sunset+60     Time

  6500 |__________
       |          |                        <- redshift -O / cron step function
       |          |
       |          |
  4700 |          |
       |          |
       |          |_________________________
  2900 |_________________________________________
       sunset-60   sunset    sunset+60     Time
```

### Manual Override

When `--set TEMP MINUTES` is called, the daemon enters manual mode:

1. The sigmoid begins transitioning from current temperature to the target over the specified duration
2. A manual flag is set -- the daemon stops evaluating solar/weather conditions
3. After reaching the target, the daemon holds until the next natural transition point (e.g., 15 minutes before the next sunrise)
4. At that point, the flag clears and solar-based control resumes

The manual call communicates with the running daemon via a control file (watched by inotify). The daemon itself drives the gamma -- no second process.

### Weather Awareness

Every 15 minutes, ABRAXAS fetches the hourly forecast from `api.weather.gov` (NOAA, US only). If cloud cover exceeds 75%, daytime temperature drops from 6500K to 4500K ("dark mode"). This prevents eye strain on overcast days when the ambient light is already dim.

The weather API requires no API key. Rate limits are generous (per User-Agent).

## Installation

### Dependencies

```bash
# Required: build tools
pacman -S gcc make pkg-config

# Optional: Wayland backend (Sway, Hyprland, river, etc.)
pacman -S wayland wayland-protocols

# Optional: GNOME backend (GNOME Wayland)
pacman -S systemd-libs

# Optional: X11 fallback (NVIDIA users)
pacman -S libx11 libxrandr
```

No runtime dependencies beyond libc and libm. Python 3.10+ for the daemon. All backends are optional and auto-detected at build time.

### Build & Install

```bash
# Automated installer (builds libmeridian, copies files, enables service)
./install.py

# Or manually:
cd libmeridian && make
mkdir -p ~/.local/lib ~/.local/bin ~/.config/abraxas
cp libmeridian.so ~/.local/lib/
cp abraxas ~/.local/bin/ && chmod +x ~/.local/bin/abraxas
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

Edit the constants at the top of the `abraxas` script:

```python
TEMP_DAY_CLEAR = 6500       # Clear sky daytime temperature (K)
TEMP_DAY_DARK  = 4500       # Overcast daytime temperature (K)
TEMP_NIGHT     = 2900       # Night temperature (K)
CLOUD_THRESHOLD = 75        # Cloud cover % to trigger dark mode
WEATHER_REFRESH_MINUTES = 15
TEMP_UPDATE_SECONDS = 60
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

Build with:
```bash
gcc -I/usr/local/include myapp.c -lmeridian -o myapp
```

See `libmeridian/include/meridian.h` for the full API.

## How It Works

1. On startup, ABRAXAS calculates sunrise and sunset from your coordinates using the NOAA solar ephemeris (Jean Meeus algorithms). No network call needed.

2. Every 60 seconds (via timerfd), it recalculates the sun's elevation angle and determines where you are in the day/night cycle: before the dawn window, inside it, between windows (full day), inside the dusk window, or after it (full night).

3. Inside a transition window, the normalized sigmoid maps elapsed time to a smooth interpolation factor between night and day temperatures. Dusk runs day-to-night over 120 minutes centered on sunset. Dawn runs night-to-day over 90 minutes centered on sunrise -- the same curve in reverse. Outside both windows, temperature is held constant at the appropriate endpoint.

4. Every 15 minutes, weather is fetched from NOAA. If cloud cover exceeds 75%, the daytime target drops from 6500K to 4500K. The sigmoid still governs the transition shape -- only the target changes.

5. The calculated temperature is applied via libmeridian, which fills a gamma ramp from a blackbody color table (Ingo Thies, 2013) and writes it via whichever backend is active (Wayland protocol, Mutter DBus, DRM ioctl, or X11 RandR).

## Platform Support

- **Linux only**.
- **Wayland (wlr)**: Native gamma control on Sway, Hyprland, river, labwc, wayfire, niri
- **GNOME Wayland**: Mutter DBus gamma control (Debian, Ubuntu, Fedora defaults)
- **AMD/Intel/Nouveau**: DRM backend (pure kernel, no compositor needed)
- **NVIDIA proprietary**: X11/RandR fallback (requires X11 libs)
- **US locations**: Weather via api.weather.gov (NOAA). Non-US locations work for solar calculations but weather features require a US grid point.

## License

MIT
