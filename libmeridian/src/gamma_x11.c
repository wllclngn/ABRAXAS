/*
 * gamma_x11.c - X11 RandR gamma control fallback
 *
 * Used when DRM gamma fails (NVIDIA proprietary, etc.)
 * Requires libX11 and libXrandr at link time.
 */

#ifdef MERIDIAN_HAS_X11

#include "meridian.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

/* X11 state */
struct meridian_x11_state {
    Display *display;
    Window root;
    int screen;
    int crtc_count;
    RRCrtc *crtcs;
    int *gamma_sizes;
    XRRScreenResources *resources;
    /* Saved gamma for restore */
    XRRCrtcGamma **saved_gamma;
};

meridian_error_t
meridian_x11_init(meridian_x11_state_t **state_out)
{
    meridian_x11_state_t *state = calloc(1, sizeof(meridian_x11_state_t));
    if (!state) return MERIDIAN_ERR_DRM_RESOURCES;

    /* Open display */
    state->display = XOpenDisplay(nullptr);
    if (!state->display) {
        free(state);
        return MERIDIAN_ERR_DRM_OPEN;
    }

    state->screen = DefaultScreen(state->display);
    state->root = RootWindow(state->display, state->screen);

    /* Get screen resources */
    state->resources = XRRGetScreenResourcesCurrent(state->display, state->root);
    if (!state->resources) {
        XCloseDisplay(state->display);
        free(state);
        return MERIDIAN_ERR_DRM_RESOURCES;
    }

    state->crtc_count = state->resources->ncrtc;
    if (state->crtc_count == 0) {
        XRRFreeScreenResources(state->resources);
        XCloseDisplay(state->display);
        free(state);
        return MERIDIAN_ERR_NO_CRTC;
    }

    /* Allocate arrays */
    state->crtcs = calloc(state->crtc_count, sizeof(RRCrtc));
    state->gamma_sizes = calloc(state->crtc_count, sizeof(int));
    state->saved_gamma = calloc(state->crtc_count, sizeof(XRRCrtcGamma *));

    if (!state->crtcs || !state->gamma_sizes || !state->saved_gamma) {
        free(state->crtcs);
        free(state->gamma_sizes);
        free(state->saved_gamma);
        XRRFreeScreenResources(state->resources);
        XCloseDisplay(state->display);
        free(state);
        return MERIDIAN_ERR_DRM_RESOURCES;
    }

    /* Initialize each CRTC */
    for (int i = 0; i < state->crtc_count; i++) {
        state->crtcs[i] = state->resources->crtcs[i];
        state->gamma_sizes[i] = XRRGetCrtcGammaSize(state->display, state->crtcs[i]);

        /* Save original gamma */
        if (state->gamma_sizes[i] > 0) {
            state->saved_gamma[i] = XRRGetCrtcGamma(state->display, state->crtcs[i]);
        }
    }

    *state_out = state;
    return MERIDIAN_OK;
}

void
meridian_x11_free(meridian_x11_state_t *state)
{
    if (!state) return;

    /* Restore original gamma (ignore errors during cleanup) */
    (void)meridian_x11_restore(state);

    /* Free saved gamma */
    for (int i = 0; i < state->crtc_count; i++) {
        if (state->saved_gamma[i]) {
            XRRFreeGamma(state->saved_gamma[i]);
        }
    }

    free(state->crtcs);
    free(state->gamma_sizes);
    free(state->saved_gamma);

    if (state->resources) {
        XRRFreeScreenResources(state->resources);
    }

    if (state->display) {
        XCloseDisplay(state->display);
    }

    free(state);
}

int
meridian_x11_get_crtc_count(const meridian_x11_state_t *state)
{
    return state ? state->crtc_count : 0;
}

int
meridian_x11_get_gamma_size(const meridian_x11_state_t *state, int crtc_idx)
{
    if (!state || crtc_idx < 0 || crtc_idx >= state->crtc_count) {
        return 0;
    }
    return state->gamma_sizes[crtc_idx];
}

meridian_error_t
meridian_x11_set_temperature_crtc(meridian_x11_state_t *state, int crtc_idx,
                                int temp, float brightness)
{
    if (!state || crtc_idx < 0 || crtc_idx >= state->crtc_count) {
        return MERIDIAN_ERR_DRM_CRTC;
    }

    int gamma_size = state->gamma_sizes[crtc_idx];
    if (gamma_size <= 0) {
        return MERIDIAN_ERR_DRM_CRTC;
    }

    /* Allocate XRRCrtcGamma structure */
    XRRCrtcGamma *gamma = XRRAllocGamma(gamma_size);
    if (!gamma) {
        return MERIDIAN_ERR_DRM_RESOURCES;
    }

    /* Fill gamma ramps */
    meridian_error_t err = meridian_fill_gamma_ramps(temp, gamma_size,
                                                gamma->red, gamma->green, gamma->blue,
                                                brightness);
    if (err != MERIDIAN_OK) {
        XRRFreeGamma(gamma);
        return err;
    }

    /* Set gamma */
    XRRSetCrtcGamma(state->display, state->crtcs[crtc_idx], gamma);
    XFlush(state->display);

    XRRFreeGamma(gamma);
    return MERIDIAN_OK;
}

meridian_error_t
meridian_x11_set_temperature(meridian_x11_state_t *state, int temp, float brightness)
{
    if (!state) return MERIDIAN_ERR_DRM_RESOURCES;

    meridian_error_t last_err = MERIDIAN_OK;
    int success_count = 0;

    for (int i = 0; i < state->crtc_count; i++) {
        if (state->gamma_sizes[i] > 0) {
            meridian_error_t err = meridian_x11_set_temperature_crtc(state, i, temp, brightness);
            if (err == MERIDIAN_OK) {
                success_count++;
            } else {
                last_err = err;
            }
        }
    }

    return (success_count > 0) ? MERIDIAN_OK : last_err;
}

meridian_error_t
meridian_x11_restore(meridian_x11_state_t *state)
{
    if (!state) return MERIDIAN_ERR_DRM_RESOURCES;

    for (int i = 0; i < state->crtc_count; i++) {
        if (state->saved_gamma[i]) {
            XRRSetCrtcGamma(state->display, state->crtcs[i], state->saved_gamma[i]);
        }
    }
    XFlush(state->display);

    return MERIDIAN_OK;
}

#endif /* MERIDIAN_HAS_X11 */
