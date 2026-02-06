/*
 * libmeridian - Gamma control for Linux
 *
 * Named for the solar meridian - the moment the sun crosses your longitude.
 *
 * Backends (auto-detected at build time):
 *   - Wayland: wlr-gamma-control (Sway, Hyprland, river, labwc, wayfire, niri)
 *   - GNOME:   Mutter DBus (GNOME Wayland via org.gnome.Mutter.DisplayConfig)
 *   - DRM:     Direct kernel ioctl (always compiled, no dependencies)
 *   - X11:     RandR (NVIDIA proprietary, etc.)
 *
 * Optional compile flags:
 *   -DMERIDIAN_HAS_WAYLAND  + link -lwayland-client   (Wayland backend)
 *   -DMERIDIAN_HAS_GNOME    + link -lsystemd           (GNOME backend)
 *   -DMERIDIAN_HAS_X11      + link -lX11 -lXrandr      (X11 backend)
 *
 * Requires C23 (-std=c2x) for [[nodiscard]], nullptr, constexpr.
 */

#ifndef MERIDIAN_H
#define MERIDIAN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Temperature bounds (Kelvin) */
#define MERIDIAN_TEMP_MIN  1000
#define MERIDIAN_TEMP_MAX  25000

/* Default gamma ramp size */
#define MERIDIAN_GAMMA_RAMP_SIZE 256

/* Error codes */
typedef enum {
    MERIDIAN_OK = 0,
    MERIDIAN_ERR_INVALID_TEMP = -1,
    MERIDIAN_ERR_OPEN = -2,
    MERIDIAN_ERR_RESOURCES = -3,
    MERIDIAN_ERR_CRTC = -4,
    MERIDIAN_ERR_GAMMA = -5,
    MERIDIAN_ERR_NO_CRTC = -6,
    MERIDIAN_ERR_PERMISSION = -7,
    MERIDIAN_ERR_WAYLAND_CONNECT = -8,
    MERIDIAN_ERR_WAYLAND_PROTOCOL = -9,
    MERIDIAN_ERR_GNOME_DBUS = -10,
} meridian_error_t;

/* Opaque handles */
typedef struct meridian_state meridian_state_t;             /* Unified (auto-select backend) */
typedef struct meridian_drm_state meridian_drm_state_t;     /* DRM backend */
typedef struct meridian_x11_state meridian_x11_state_t;     /* X11 backend */
typedef struct meridian_wl_state meridian_wl_state_t;       /* Wayland backend */
typedef struct meridian_gnome_state meridian_gnome_state_t; /* GNOME backend */

/* RGB color (0.0 - 1.0 range) */
typedef struct {
    float r;
    float g;
    float b;
} meridian_rgb_t;

/* ============================================================
 * Color Ramp Functions
 * ============================================================ */

/*
 * Convert color temperature to RGB multipliers.
 *
 * temp: Color temperature in Kelvin (1000-25000)
 * rgb:  Output RGB values (0.0-1.0 range)
 *
 * Returns: MERIDIAN_OK on success, MERIDIAN_ERR_INVALID_TEMP if out of range
 */
[[nodiscard]]
meridian_error_t meridian_temp_to_rgb(int temp, meridian_rgb_t *rgb);

/*
 * Fill a gamma ramp array for the given temperature.
 *
 * temp:       Color temperature in Kelvin
 * gamma_size: Size of each ramp array (typically 256 or 1024)
 * r, g, b:    Output arrays (must be gamma_size * sizeof(uint16_t))
 *             restrict: arrays must not overlap
 * brightness: Brightness multiplier (0.0-1.0, typically 1.0)
 *
 * Returns: MERIDIAN_OK on success
 */
[[nodiscard]]
meridian_error_t meridian_fill_gamma_ramps(int temp, int gamma_size,
                                            uint16_t *restrict r,
                                            uint16_t *restrict g,
                                            uint16_t *restrict b,
                                            float brightness);

/* ============================================================
 * Unified Gamma Control (Auto-select DRM or X11)
 * ============================================================ */

/*
 * Initialize gamma control with automatic backend selection.
 * Tries DRM first, falls back to X11 if DRM gamma unavailable.
 *
 * state: Output pointer to allocated state
 *
 * Returns: MERIDIAN_OK on success, error code otherwise
 */
[[nodiscard]]
meridian_error_t meridian_init(meridian_state_t **state);

/*
 * Initialize gamma control for specific graphics card.
 *
 * card_num: Graphics card number (0 for /dev/dri/card0)
 * state:    Output pointer to allocated state
 */
[[nodiscard]]
meridian_error_t meridian_init_card(int card_num, meridian_state_t **state);

/*
 * Free state and restore original gamma.
 */
void meridian_free(meridian_state_t *state);

/*
 * Get name of active backend ("wayland", "gnome", "drm", or "x11").
 */
const char *meridian_get_backend_name(const meridian_state_t *state);

/*
 * Get number of CRTCs (displays) available.
 */
int meridian_get_crtc_count(const meridian_state_t *state);

/*
 * Get gamma ramp size for a CRTC.
 */
int meridian_get_gamma_size(const meridian_state_t *state, int crtc_idx);

/*
 * Set color temperature on all CRTCs.
 *
 * state:      State from meridian_init
 * temp:       Color temperature in Kelvin
 * brightness: Brightness (0.0-1.0)
 *
 * Returns: MERIDIAN_OK on success
 */
[[nodiscard]]
meridian_error_t meridian_set_temperature(meridian_state_t *state,
                                           int temp, float brightness);

/*
 * Set color temperature on a specific CRTC.
 */
[[nodiscard]]
meridian_error_t meridian_set_temperature_crtc(meridian_state_t *state,
                                                int crtc_idx, int temp,
                                                float brightness);

/*
 * Restore original gamma ramps on all CRTCs.
 */
[[nodiscard]]
meridian_error_t meridian_restore(meridian_state_t *state);

/*
 * Get error message string.
 */
const char *meridian_strerror(meridian_error_t err);

/* ============================================================
 * DRM Backend (Direct Kernel)
 * ============================================================ */

[[nodiscard]]
meridian_error_t meridian_drm_init(int card_num, meridian_drm_state_t **state);
void meridian_drm_free(meridian_drm_state_t *state);
int meridian_drm_get_crtc_count(const meridian_drm_state_t *state);
int meridian_drm_get_gamma_size(const meridian_drm_state_t *state, int crtc_idx);
[[nodiscard]]
meridian_error_t meridian_drm_set_temperature(meridian_drm_state_t *state,
                                               int temp, float brightness);
[[nodiscard]]
meridian_error_t meridian_drm_set_temperature_crtc(meridian_drm_state_t *state,
                                                    int crtc_idx, int temp,
                                                    float brightness);
[[nodiscard]]
meridian_error_t meridian_drm_restore(meridian_drm_state_t *state);

/* ============================================================
 * X11 Backend (RandR)
 * ============================================================ */

#ifdef MERIDIAN_HAS_X11
[[nodiscard]]
meridian_error_t meridian_x11_init(meridian_x11_state_t **state);
void meridian_x11_free(meridian_x11_state_t *state);
int meridian_x11_get_crtc_count(const meridian_x11_state_t *state);
int meridian_x11_get_gamma_size(const meridian_x11_state_t *state, int crtc_idx);
[[nodiscard]]
meridian_error_t meridian_x11_set_temperature(meridian_x11_state_t *state,
                                               int temp, float brightness);
[[nodiscard]]
meridian_error_t meridian_x11_set_temperature_crtc(meridian_x11_state_t *state,
                                                    int crtc_idx, int temp,
                                                    float brightness);
[[nodiscard]]
meridian_error_t meridian_x11_restore(meridian_x11_state_t *state);
#endif

/* ============================================================
 * Wayland Backend (wlr-gamma-control)
 * ============================================================ */

#ifdef MERIDIAN_HAS_WAYLAND
[[nodiscard]]
meridian_error_t meridian_wl_init(meridian_wl_state_t **state);
void meridian_wl_free(meridian_wl_state_t *state);
int meridian_wl_get_crtc_count(const meridian_wl_state_t *state);
int meridian_wl_get_gamma_size(const meridian_wl_state_t *state, int crtc_idx);
[[nodiscard]]
meridian_error_t meridian_wl_set_temperature(meridian_wl_state_t *state,
                                              int temp, float brightness);
[[nodiscard]]
meridian_error_t meridian_wl_set_temperature_crtc(meridian_wl_state_t *state,
                                                   int crtc_idx, int temp,
                                                   float brightness);
[[nodiscard]]
meridian_error_t meridian_wl_restore(meridian_wl_state_t *state);
#endif

/* ============================================================
 * GNOME Backend (Mutter DBus)
 * ============================================================ */

#ifdef MERIDIAN_HAS_GNOME
[[nodiscard]]
meridian_error_t meridian_gnome_init(meridian_gnome_state_t **state);
void meridian_gnome_free(meridian_gnome_state_t *state);
int meridian_gnome_get_crtc_count(const meridian_gnome_state_t *state);
int meridian_gnome_get_gamma_size(const meridian_gnome_state_t *state, int crtc_idx);
[[nodiscard]]
meridian_error_t meridian_gnome_set_temperature(meridian_gnome_state_t *state,
                                                 int temp, float brightness);
[[nodiscard]]
meridian_error_t meridian_gnome_set_temperature_crtc(meridian_gnome_state_t *state,
                                                      int crtc_idx, int temp,
                                                      float brightness);
[[nodiscard]]
meridian_error_t meridian_gnome_restore(meridian_gnome_state_t *state);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MERIDIAN_H */
