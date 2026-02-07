/*
 * gamma_auto.c - Unified gamma control with automatic backend selection
 *
 * Detection order:
 *   1. If $WAYLAND_DISPLAY set:
 *      a. Wayland (wlr-gamma-control) - Sway, Hyprland, river, etc.
 *      b. GNOME (Mutter DBus) - GNOME Wayland
 *   2. DRM (kernel ioctl) - always available
 *   3. X11 (RandR) - NVIDIA fallback
 *
 * X11 and GNOME backends load their libraries via dlopen (in their own files).
 * Wayland backend is a separate .so plugin (meridian_wl.so) loaded here.
 */

#define _GNU_SOURCE
#include "meridian.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>

/* ============================================================
 * Error handling (shared by all backends)
 * ============================================================ */

static const char *error_messages[] = {
    [0] = "Success",
    [-MERIDIAN_ERR_INVALID_TEMP] = "Invalid temperature",
    [-MERIDIAN_ERR_OPEN] = "Failed to open display device",
    [-MERIDIAN_ERR_RESOURCES] = "Failed to get display resources",
    [-MERIDIAN_ERR_CRTC] = "Failed to get CRTC info",
    [-MERIDIAN_ERR_GAMMA] = "Failed to set gamma ramp",
    [-MERIDIAN_ERR_NO_CRTC] = "No usable CRTC found",
    [-MERIDIAN_ERR_PERMISSION] = "Permission denied (need video group?)",
    [-MERIDIAN_ERR_WAYLAND_CONNECT] = "Failed to connect to Wayland display",
    [-MERIDIAN_ERR_WAYLAND_PROTOCOL] = "Wayland compositor lacks gamma control protocol",
    [-MERIDIAN_ERR_GNOME_DBUS] = "Failed to communicate with Mutter via DBus",
};

const char *
meridian_strerror(meridian_error_t err)
{
    int idx = -err;
    if (idx >= 0 && idx < (int)(sizeof(error_messages) / sizeof(error_messages[0]))) {
        return error_messages[idx] ? error_messages[idx] : "Unknown error";
    }
    return "Unknown error";
}

/* ============================================================
 * Wayland plugin (loaded via dlopen -- contains libwayland-client)
 * ============================================================ */

#ifdef MERIDIAN_HAS_WAYLAND
static struct {
    void *lib;
    meridian_error_t (*init)(meridian_wl_state_t **);
    void (*free)(meridian_wl_state_t *);
    int (*get_crtc_count)(const meridian_wl_state_t *);
    int (*get_gamma_size)(const meridian_wl_state_t *, int);
    meridian_error_t (*set_temperature)(meridian_wl_state_t *, int, float);
    meridian_error_t (*set_temperature_crtc)(meridian_wl_state_t *, int, int, float);
    meridian_error_t (*restore)(meridian_wl_state_t *);
    bool loaded;
} wl_plugin;

static bool wl_plugin_load(void)
{
    if (wl_plugin.loaded) return true;

    wl_plugin.lib = dlopen("meridian_wl.so", RTLD_LAZY | RTLD_LOCAL);
    if (!wl_plugin.lib) return false;

    /* POSIX guarantees dlsym void*->fptr works; suppress ISO C pedantic */
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wpedantic"
    #define LOAD_WL(field, sym) \
        wl_plugin.field = dlsym(wl_plugin.lib, sym); \
        if (!wl_plugin.field) goto fail

    LOAD_WL(init,               "meridian_wl_init");
    LOAD_WL(free,               "meridian_wl_free");
    LOAD_WL(get_crtc_count,     "meridian_wl_get_crtc_count");
    LOAD_WL(get_gamma_size,     "meridian_wl_get_gamma_size");
    LOAD_WL(set_temperature,    "meridian_wl_set_temperature");
    LOAD_WL(set_temperature_crtc, "meridian_wl_set_temperature_crtc");
    LOAD_WL(restore,            "meridian_wl_restore");
    #undef LOAD_WL
    #pragma GCC diagnostic pop

    wl_plugin.loaded = true;
    return true;

fail:
    dlclose(wl_plugin.lib);
    wl_plugin.lib = nullptr;
    return false;
}
#endif

/* Backend type */
typedef enum {
    BACKEND_NONE = 0,
    BACKEND_DRM,
    BACKEND_X11,
    BACKEND_WAYLAND,
    BACKEND_GNOME,
} backend_type_t;

/* Unified state */
struct meridian_state {
    backend_type_t backend;
    union {
        meridian_drm_state_t *drm;
#ifdef MERIDIAN_HAS_X11
        meridian_x11_state_t *x11;
#endif
#ifdef MERIDIAN_HAS_WAYLAND
        meridian_wl_state_t *wl;
#endif
#ifdef MERIDIAN_HAS_GNOME
        meridian_gnome_state_t *gnome;
#endif
    };
};

meridian_error_t
meridian_init(meridian_state_t **state_out)
{
    return meridian_init_card(0, state_out);
}

meridian_error_t
meridian_init_card(int card_num, meridian_state_t **state_out)
{
    meridian_state_t *state = calloc(1, sizeof(meridian_state_t));
    if (!state) return MERIDIAN_ERR_RESOURCES;

    meridian_error_t err;

#ifndef ABXS_STATIC
    /* Wayland session: try Wayland backends first */
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    if (wayland_display && wayland_display[0]) {

#ifdef MERIDIAN_HAS_WAYLAND
        /* Try wlr-gamma-control (Sway, Hyprland, river, etc.) */
        if (wl_plugin_load()) {
            err = wl_plugin.init(&state->wl);
            if (err == MERIDIAN_OK) {
                state->backend = BACKEND_WAYLAND;
                *state_out = state;
                return MERIDIAN_OK;
            }
        }
#endif

#ifdef MERIDIAN_HAS_GNOME
        /* Try Mutter DBus (GNOME Wayland) */
        err = meridian_gnome_init(&state->gnome);
        if (err == MERIDIAN_OK) {
            state->backend = BACKEND_GNOME;
            *state_out = state;
            return MERIDIAN_OK;
        }
#endif
    }
#endif /* !ABXS_STATIC */

    /* Try DRM */
    err = meridian_drm_init(card_num, &state->drm);
    if (err == MERIDIAN_OK) {
        /* Check if any CRTC has usable gamma */
        int usable = 0;
        int count = meridian_drm_get_crtc_count(state->drm);
        for (int i = 0; i < count; i++) {
            if (meridian_drm_get_gamma_size(state->drm, i) > 1) {
                usable++;
            }
        }

        if (usable > 0) {
            state->backend = BACKEND_DRM;
            *state_out = state;
            return MERIDIAN_OK;
        }

        /* DRM opened but no usable gamma (NVIDIA, etc.) */
        meridian_drm_free(state->drm);
        state->drm = nullptr;
    }

#if !defined(ABXS_STATIC) && defined(MERIDIAN_HAS_X11)
    /* Fall back to X11 */
    err = meridian_x11_init(&state->x11);
    if (err == MERIDIAN_OK) {
        state->backend = BACKEND_X11;
        *state_out = state;
        return MERIDIAN_OK;
    }
#endif

    /* All backends failed */
    free(state);
    return MERIDIAN_ERR_NO_CRTC;
}

void
meridian_free(meridian_state_t *state)
{
    if (!state) return;

    switch (state->backend) {
    case BACKEND_DRM:
        meridian_drm_free(state->drm);
        break;
#ifdef MERIDIAN_HAS_X11
    case BACKEND_X11:
        meridian_x11_free(state->x11);
        break;
#endif
#ifdef MERIDIAN_HAS_WAYLAND
    case BACKEND_WAYLAND:
        wl_plugin.free(state->wl);
        break;
#endif
#ifdef MERIDIAN_HAS_GNOME
    case BACKEND_GNOME:
        meridian_gnome_free(state->gnome);
        break;
#endif
    default:
        break;
    }

    free(state);
}

const char *
meridian_get_backend_name(const meridian_state_t *state)
{
    if (!state) return "none";

    switch (state->backend) {
    case BACKEND_DRM:      return "drm";
    case BACKEND_X11:      return "x11";
    case BACKEND_WAYLAND:  return "wayland";
    case BACKEND_GNOME:    return "gnome";
    default:               return "none";
    }
}

int
meridian_get_crtc_count(const meridian_state_t *state)
{
    if (!state) return 0;

    switch (state->backend) {
    case BACKEND_DRM:
        return meridian_drm_get_crtc_count(state->drm);
#ifdef MERIDIAN_HAS_X11
    case BACKEND_X11:
        return meridian_x11_get_crtc_count(state->x11);
#endif
#ifdef MERIDIAN_HAS_WAYLAND
    case BACKEND_WAYLAND:
        return wl_plugin.get_crtc_count(state->wl);
#endif
#ifdef MERIDIAN_HAS_GNOME
    case BACKEND_GNOME:
        return meridian_gnome_get_crtc_count(state->gnome);
#endif
    default:
        return 0;
    }
}

int
meridian_get_gamma_size(const meridian_state_t *state, int crtc_idx)
{
    if (!state) return 0;

    switch (state->backend) {
    case BACKEND_DRM:
        return meridian_drm_get_gamma_size(state->drm, crtc_idx);
#ifdef MERIDIAN_HAS_X11
    case BACKEND_X11:
        return meridian_x11_get_gamma_size(state->x11, crtc_idx);
#endif
#ifdef MERIDIAN_HAS_WAYLAND
    case BACKEND_WAYLAND:
        return wl_plugin.get_gamma_size(state->wl, crtc_idx);
#endif
#ifdef MERIDIAN_HAS_GNOME
    case BACKEND_GNOME:
        return meridian_gnome_get_gamma_size(state->gnome, crtc_idx);
#endif
    default:
        return 0;
    }
}

meridian_error_t
meridian_set_temperature(meridian_state_t *state, int temp, float brightness)
{
    if (!state) return MERIDIAN_ERR_RESOURCES;

    switch (state->backend) {
    case BACKEND_DRM:
        return meridian_drm_set_temperature(state->drm, temp, brightness);
#ifdef MERIDIAN_HAS_X11
    case BACKEND_X11:
        return meridian_x11_set_temperature(state->x11, temp, brightness);
#endif
#ifdef MERIDIAN_HAS_WAYLAND
    case BACKEND_WAYLAND:
        return wl_plugin.set_temperature(state->wl, temp, brightness);
#endif
#ifdef MERIDIAN_HAS_GNOME
    case BACKEND_GNOME:
        return meridian_gnome_set_temperature(state->gnome, temp, brightness);
#endif
    default:
        return MERIDIAN_ERR_NO_CRTC;
    }
}

meridian_error_t
meridian_set_temperature_crtc(meridian_state_t *state, int crtc_idx,
                            int temp, float brightness)
{
    if (!state) return MERIDIAN_ERR_RESOURCES;

    switch (state->backend) {
    case BACKEND_DRM:
        return meridian_drm_set_temperature_crtc(state->drm, crtc_idx, temp, brightness);
#ifdef MERIDIAN_HAS_X11
    case BACKEND_X11:
        return meridian_x11_set_temperature_crtc(state->x11, crtc_idx, temp, brightness);
#endif
#ifdef MERIDIAN_HAS_WAYLAND
    case BACKEND_WAYLAND:
        return wl_plugin.set_temperature_crtc(state->wl, crtc_idx, temp, brightness);
#endif
#ifdef MERIDIAN_HAS_GNOME
    case BACKEND_GNOME:
        return meridian_gnome_set_temperature_crtc(state->gnome, crtc_idx, temp, brightness);
#endif
    default:
        return MERIDIAN_ERR_NO_CRTC;
    }
}

meridian_error_t
meridian_restore(meridian_state_t *state)
{
    if (!state) return MERIDIAN_ERR_RESOURCES;

    switch (state->backend) {
    case BACKEND_DRM:
        return meridian_drm_restore(state->drm);
#ifdef MERIDIAN_HAS_X11
    case BACKEND_X11:
        return meridian_x11_restore(state->x11);
#endif
#ifdef MERIDIAN_HAS_WAYLAND
    case BACKEND_WAYLAND:
        return wl_plugin.restore(state->wl);
#endif
#ifdef MERIDIAN_HAS_GNOME
    case BACKEND_GNOME:
        return meridian_gnome_restore(state->gnome);
#endif
    default:
        return MERIDIAN_ERR_NO_CRTC;
    }
}
