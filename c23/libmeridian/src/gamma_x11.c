/*
 * gamma_x11.c - X11 RandR gamma control fallback
 *
 * Used when DRM gamma fails (NVIDIA proprietary, etc.)
 * Libraries loaded at runtime via dlopen -- no link-time dependency.
 */

#ifdef MERIDIAN_HAS_X11

#define _GNU_SOURCE
#include "meridian.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

/* Runtime-loaded X11 + XRandR functions */
static struct {
    void *libx11;
    void *libxrandr;
    Display *(*XOpenDisplay)(const char *);
    int (*XCloseDisplay)(Display *);
    int (*XFlush)(Display *);
    XRRScreenResources *(*XRRGetScreenResourcesCurrent)(Display *, Window);
    void (*XRRFreeScreenResources)(XRRScreenResources *);
    int (*XRRGetCrtcGammaSize)(Display *, RRCrtc);
    XRRCrtcGamma *(*XRRGetCrtcGamma)(Display *, RRCrtc);
    XRRCrtcGamma *(*XRRAllocGamma)(int);
    void (*XRRSetCrtcGamma)(Display *, RRCrtc, XRRCrtcGamma *);
    void (*XRRFreeGamma)(XRRCrtcGamma *);
    bool loaded;
} x11;

static bool x11_load(void)
{
    if (x11.loaded) return true;

    x11.libx11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!x11.libx11) return false;

    x11.libxrandr = dlopen("libXrandr.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!x11.libxrandr) { dlclose(x11.libx11); x11.libx11 = nullptr; return false; }

    /* POSIX guarantees dlsym void*->fptr works; suppress ISO C pedantic */
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wpedantic"
    #define LOAD(lib, name) \
        x11.name = dlsym(x11.lib, #name); \
        if (!x11.name) goto fail

    LOAD(libx11, XOpenDisplay);
    LOAD(libx11, XCloseDisplay);
    LOAD(libx11, XFlush);
    LOAD(libxrandr, XRRGetScreenResourcesCurrent);
    LOAD(libxrandr, XRRFreeScreenResources);
    LOAD(libxrandr, XRRGetCrtcGammaSize);
    LOAD(libxrandr, XRRGetCrtcGamma);
    LOAD(libxrandr, XRRAllocGamma);
    LOAD(libxrandr, XRRSetCrtcGamma);
    LOAD(libxrandr, XRRFreeGamma);
    #undef LOAD
    #pragma GCC diagnostic pop

    x11.loaded = true;
    return true;

fail:
    dlclose(x11.libxrandr); x11.libxrandr = nullptr;
    dlclose(x11.libx11);    x11.libx11 = nullptr;
    return false;
}

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
    if (!x11_load()) return MERIDIAN_ERR_OPEN;

    meridian_x11_state_t *state = calloc(1, sizeof(meridian_x11_state_t));
    if (!state) return MERIDIAN_ERR_RESOURCES;

    /* Open display */
    state->display = x11.XOpenDisplay(nullptr);
    if (!state->display) {
        free(state);
        return MERIDIAN_ERR_OPEN;
    }

    state->screen = DefaultScreen(state->display);
    state->root = RootWindow(state->display, state->screen);

    /* Get screen resources */
    state->resources = x11.XRRGetScreenResourcesCurrent(state->display, state->root);
    if (!state->resources) {
        x11.XCloseDisplay(state->display);
        free(state);
        return MERIDIAN_ERR_RESOURCES;
    }

    state->crtc_count = state->resources->ncrtc;
    if (state->crtc_count == 0) {
        x11.XRRFreeScreenResources(state->resources);
        x11.XCloseDisplay(state->display);
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
        x11.XRRFreeScreenResources(state->resources);
        x11.XCloseDisplay(state->display);
        free(state);
        return MERIDIAN_ERR_RESOURCES;
    }

    /* Initialize each CRTC */
    for (int i = 0; i < state->crtc_count; i++) {
        state->crtcs[i] = state->resources->crtcs[i];
        state->gamma_sizes[i] = x11.XRRGetCrtcGammaSize(state->display, state->crtcs[i]);

        /* Save original gamma */
        if (state->gamma_sizes[i] > 0) {
            state->saved_gamma[i] = x11.XRRGetCrtcGamma(state->display, state->crtcs[i]);
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
            x11.XRRFreeGamma(state->saved_gamma[i]);
        }
    }

    free(state->crtcs);
    free(state->gamma_sizes);
    free(state->saved_gamma);

    if (state->resources) {
        x11.XRRFreeScreenResources(state->resources);
    }

    if (state->display) {
        x11.XCloseDisplay(state->display);
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
        return MERIDIAN_ERR_CRTC;
    }

    int gamma_size = state->gamma_sizes[crtc_idx];
    if (gamma_size <= 0) {
        return MERIDIAN_ERR_CRTC;
    }

    /* Allocate XRRCrtcGamma structure */
    XRRCrtcGamma *gamma = x11.XRRAllocGamma(gamma_size);
    if (!gamma) {
        return MERIDIAN_ERR_RESOURCES;
    }

    /* Fill gamma ramps */
    meridian_error_t err = meridian_fill_gamma_ramps(temp, gamma_size,
                                                gamma->red, gamma->green, gamma->blue,
                                                brightness);
    if (err != MERIDIAN_OK) {
        x11.XRRFreeGamma(gamma);
        return err;
    }

    /* Set gamma */
    x11.XRRSetCrtcGamma(state->display, state->crtcs[crtc_idx], gamma);
    x11.XFlush(state->display);

    x11.XRRFreeGamma(gamma);
    return MERIDIAN_OK;
}

meridian_error_t
meridian_x11_set_temperature(meridian_x11_state_t *state, int temp, float brightness)
{
    if (!state) return MERIDIAN_ERR_RESOURCES;

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
    if (!state) return MERIDIAN_ERR_RESOURCES;

    for (int i = 0; i < state->crtc_count; i++) {
        if (state->saved_gamma[i]) {
            x11.XRRSetCrtcGamma(state->display, state->crtcs[i], state->saved_gamma[i]);
        }
    }
    x11.XFlush(state->display);

    return MERIDIAN_OK;
}

#endif /* MERIDIAN_HAS_X11 */
