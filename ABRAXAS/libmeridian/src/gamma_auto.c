/*
 * gamma_auto.c - Unified gamma control with automatic backend selection
 *
 * Detection order:
 *   1. If $WAYLAND_DISPLAY set:
 *      a. Wayland (wlr-gamma-control) - Sway, Hyprland, river, etc.
 *      b. GNOME (Mutter DBus) - GNOME Wayland
 *   2. DRM (kernel ioctl) - always available
 *   3. X11 (RandR) - NVIDIA fallback
 */

#include "meridian.h"

#include <stdlib.h>
#include <stdio.h>

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
    if (!state) return MERIDIAN_ERR_DRM_RESOURCES;

    meridian_error_t err;
    const char *wayland_display = getenv("WAYLAND_DISPLAY");

    /* Wayland session: try Wayland backends first */
    if (wayland_display && wayland_display[0]) {

#ifdef MERIDIAN_HAS_WAYLAND
        /* Try wlr-gamma-control (Sway, Hyprland, river, etc.) */
        err = meridian_wl_init(&state->wl);
        if (err == MERIDIAN_OK) {
            state->backend = BACKEND_WAYLAND;
            *state_out = state;
            return MERIDIAN_OK;
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

#ifdef MERIDIAN_HAS_X11
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
        meridian_wl_free(state->wl);
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
        return meridian_wl_get_crtc_count(state->wl);
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
        return meridian_wl_get_gamma_size(state->wl, crtc_idx);
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
    if (!state) return MERIDIAN_ERR_DRM_RESOURCES;

    switch (state->backend) {
    case BACKEND_DRM:
        return meridian_drm_set_temperature(state->drm, temp, brightness);
#ifdef MERIDIAN_HAS_X11
    case BACKEND_X11:
        return meridian_x11_set_temperature(state->x11, temp, brightness);
#endif
#ifdef MERIDIAN_HAS_WAYLAND
    case BACKEND_WAYLAND:
        return meridian_wl_set_temperature(state->wl, temp, brightness);
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
    if (!state) return MERIDIAN_ERR_DRM_RESOURCES;

    switch (state->backend) {
    case BACKEND_DRM:
        return meridian_drm_set_temperature_crtc(state->drm, crtc_idx, temp, brightness);
#ifdef MERIDIAN_HAS_X11
    case BACKEND_X11:
        return meridian_x11_set_temperature_crtc(state->x11, crtc_idx, temp, brightness);
#endif
#ifdef MERIDIAN_HAS_WAYLAND
    case BACKEND_WAYLAND:
        return meridian_wl_set_temperature_crtc(state->wl, crtc_idx, temp, brightness);
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
    if (!state) return MERIDIAN_ERR_DRM_RESOURCES;

    switch (state->backend) {
    case BACKEND_DRM:
        return meridian_drm_restore(state->drm);
#ifdef MERIDIAN_HAS_X11
    case BACKEND_X11:
        return meridian_x11_restore(state->x11);
#endif
#ifdef MERIDIAN_HAS_WAYLAND
    case BACKEND_WAYLAND:
        return meridian_wl_restore(state->wl);
#endif
#ifdef MERIDIAN_HAS_GNOME
    case BACKEND_GNOME:
        return meridian_gnome_restore(state->gnome);
#endif
    default:
        return MERIDIAN_ERR_NO_CRTC;
    }
}
