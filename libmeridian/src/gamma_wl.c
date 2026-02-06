/*
 * gamma_wl.c - Wayland gamma control via wlr-gamma-control-unstable-v1
 *
 * Covers compositors implementing the wlr protocol:
 *   Sway, Hyprland, river, labwc, wayfire, niri
 *
 * Uses memfd for gamma ramp transfer (no tmpfile needed).
 * Protocol auto-restores gamma when controls are destroyed.
 */

#ifdef MERIDIAN_HAS_WAYLAND

#define _GNU_SOURCE
#include "meridian.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <wayland-client.h>
#include "wlr-gamma-control-unstable-v1-client-protocol.h"

/* ============================================================
 * Per-output state
 * ============================================================ */

typedef struct {
    struct wl_output *wl_output;
    struct zwlr_gamma_control_v1 *gamma_control;
    uint32_t gamma_size;
    bool failed;
} wl_output_state_t;

/* ============================================================
 * Wayland state
 * ============================================================ */

struct meridian_wl_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct zwlr_gamma_control_manager_v1 *gamma_manager;
    int output_count;
    int output_capacity;
    wl_output_state_t *outputs;
};

/* ============================================================
 * Gamma control listener (per-output)
 * ============================================================ */

static void
gamma_control_gamma_size(void *data,
                         struct zwlr_gamma_control_v1 *control [[maybe_unused]],
                         uint32_t size)
{
    wl_output_state_t *output = data;
    output->gamma_size = size;
}

static void
gamma_control_failed(void *data,
                     struct zwlr_gamma_control_v1 *control [[maybe_unused]])
{
    wl_output_state_t *output = data;
    output->failed = true;
    if (output->gamma_control) {
        zwlr_gamma_control_v1_destroy(output->gamma_control);
        output->gamma_control = nullptr;
    }
}

static const struct zwlr_gamma_control_v1_listener gamma_control_listener = {
    .gamma_size = gamma_control_gamma_size,
    .failed = gamma_control_failed,
};

/* ============================================================
 * Registry listener (globals)
 * ============================================================ */

static void
add_output(struct meridian_wl_state *state, struct wl_output *output)
{
    if (state->output_count >= state->output_capacity) {
        int new_cap = state->output_capacity ? state->output_capacity * 2 : 4;
        wl_output_state_t *new_arr = realloc(state->outputs,
                                              new_cap * sizeof(wl_output_state_t));
        if (!new_arr) return;
        state->outputs = new_arr;
        state->output_capacity = new_cap;
    }

    wl_output_state_t *out = &state->outputs[state->output_count++];
    memset(out, 0, sizeof(*out));
    out->wl_output = output;
}

static void
registry_global(void *data, struct wl_registry *registry,
                uint32_t name, const char *interface, uint32_t version [[maybe_unused]])
{
    struct meridian_wl_state *state = data;

    if (strcmp(interface, zwlr_gamma_control_manager_v1_interface.name) == 0) {
        state->gamma_manager = wl_registry_bind(registry, name,
                                                 &zwlr_gamma_control_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct wl_output *output = wl_registry_bind(registry, name,
                                                     &wl_output_interface, 1);
        if (output) {
            add_output(state, output);
        }
    }
}

static void
registry_global_remove(void *data [[maybe_unused]],
                       struct wl_registry *registry [[maybe_unused]],
                       uint32_t name [[maybe_unused]])
{
    /* Hot-unplug not handled for gamma control */
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ============================================================
 * Init / Free
 * ============================================================ */

meridian_error_t
meridian_wl_init(meridian_wl_state_t **state_out)
{
    struct meridian_wl_state *state = calloc(1, sizeof(*state));
    if (!state) return MERIDIAN_ERR_DRM_RESOURCES;

    state->display = wl_display_connect(nullptr);
    if (!state->display) {
        free(state);
        return MERIDIAN_ERR_WAYLAND_CONNECT;
    }

    state->registry = wl_display_get_registry(state->display);
    wl_registry_add_listener(state->registry, &registry_listener, state);

    /* First roundtrip: discover globals (manager + outputs) */
    wl_display_roundtrip(state->display);

    if (!state->gamma_manager) {
        /* Compositor doesn't support wlr-gamma-control */
        if (state->registry) wl_registry_destroy(state->registry);
        wl_display_disconnect(state->display);
        free(state->outputs);
        free(state);
        return MERIDIAN_ERR_WAYLAND_PROTOCOL;
    }

    if (state->output_count == 0) {
        zwlr_gamma_control_manager_v1_destroy(state->gamma_manager);
        wl_registry_destroy(state->registry);
        wl_display_disconnect(state->display);
        free(state->outputs);
        free(state);
        return MERIDIAN_ERR_NO_CRTC;
    }

    /* Acquire gamma control for each output */
    for (int i = 0; i < state->output_count; i++) {
        wl_output_state_t *out = &state->outputs[i];
        out->gamma_control = zwlr_gamma_control_manager_v1_get_gamma_control(
            state->gamma_manager, out->wl_output);
        zwlr_gamma_control_v1_add_listener(out->gamma_control,
                                            &gamma_control_listener, out);
    }

    /* Second roundtrip: receive gamma_size events (or failed) */
    wl_display_roundtrip(state->display);

    /* Check that at least one output has usable gamma */
    int usable = 0;
    for (int i = 0; i < state->output_count; i++) {
        if (!state->outputs[i].failed && state->outputs[i].gamma_size > 0) {
            usable++;
        }
    }

    if (usable == 0) {
        meridian_wl_free(state);
        return MERIDIAN_ERR_NO_CRTC;
    }

    *state_out = state;
    return MERIDIAN_OK;
}

void
meridian_wl_free(meridian_wl_state_t *state)
{
    if (!state) return;

    /* Destroying gamma controls auto-restores original gamma */
    for (int i = 0; i < state->output_count; i++) {
        if (state->outputs[i].gamma_control) {
            zwlr_gamma_control_v1_destroy(state->outputs[i].gamma_control);
        }
        if (state->outputs[i].wl_output) {
            wl_output_destroy(state->outputs[i].wl_output);
        }
    }

    if (state->gamma_manager) {
        zwlr_gamma_control_manager_v1_destroy(state->gamma_manager);
    }
    if (state->registry) {
        wl_registry_destroy(state->registry);
    }

    /* Flush destroy requests before disconnect */
    if (state->display) {
        wl_display_flush(state->display);
        wl_display_disconnect(state->display);
    }

    free(state->outputs);
    free(state);
}

/* ============================================================
 * Getters
 * ============================================================ */

int
meridian_wl_get_crtc_count(const meridian_wl_state_t *state)
{
    return state ? state->output_count : 0;
}

int
meridian_wl_get_gamma_size(const meridian_wl_state_t *state, int crtc_idx)
{
    if (!state || crtc_idx < 0 || crtc_idx >= state->output_count) return 0;
    if (state->outputs[crtc_idx].failed) return 0;
    return (int)state->outputs[crtc_idx].gamma_size;
}

/* ============================================================
 * Set temperature (memfd + protocol)
 * ============================================================ */

static meridian_error_t
wl_set_gamma_crtc(struct meridian_wl_state *state, int crtc_idx,
                  int temp, float brightness)
{
    wl_output_state_t *out = &state->outputs[crtc_idx];
    if (out->failed || !out->gamma_control || out->gamma_size == 0) {
        return MERIDIAN_ERR_WAYLAND_PROTOCOL;
    }

    uint32_t gs = out->gamma_size;
    size_t ramp_bytes = gs * sizeof(uint16_t);
    size_t total = ramp_bytes * 3; /* R + G + B contiguous */

    /* Create memfd for gamma ramp transfer */
    int fd = memfd_create("meridian-gamma", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return MERIDIAN_ERR_DRM_RESOURCES;

    if (ftruncate(fd, (off_t)total) < 0) {
        close(fd);
        return MERIDIAN_ERR_DRM_RESOURCES;
    }

    uint16_t *map = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return MERIDIAN_ERR_DRM_RESOURCES;
    }

    uint16_t *r = map;
    uint16_t *g = map + gs;
    uint16_t *b = map + gs * 2;

    meridian_error_t err = meridian_fill_gamma_ramps(temp, (int)gs, r, g, b, brightness);
    if (err != MERIDIAN_OK) {
        munmap(map, total);
        close(fd);
        return err;
    }

    munmap(map, total);

    /* Seal the fd as required by the protocol */
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);

    zwlr_gamma_control_v1_set_gamma(out->gamma_control, fd);
    wl_display_flush(state->display);

    close(fd);
    return MERIDIAN_OK;
}

meridian_error_t
meridian_wl_set_temperature_crtc(meridian_wl_state_t *state, int crtc_idx,
                                 int temp, float brightness)
{
    if (!state || crtc_idx < 0 || crtc_idx >= state->output_count) {
        return MERIDIAN_ERR_WAYLAND_PROTOCOL;
    }
    return wl_set_gamma_crtc(state, crtc_idx, temp, brightness);
}

meridian_error_t
meridian_wl_set_temperature(meridian_wl_state_t *state, int temp, float brightness)
{
    if (!state) return MERIDIAN_ERR_DRM_RESOURCES;

    meridian_error_t last_err = MERIDIAN_OK;
    int success_count = 0;

    for (int i = 0; i < state->output_count; i++) {
        if (!state->outputs[i].failed && state->outputs[i].gamma_size > 0) {
            meridian_error_t err = wl_set_gamma_crtc(state, i, temp, brightness);
            if (err == MERIDIAN_OK) {
                success_count++;
            } else {
                last_err = err;
            }
        }
    }

    return (success_count > 0) ? MERIDIAN_OK : last_err;
}

/* ============================================================
 * Restore
 * ============================================================ */

meridian_error_t
meridian_wl_restore(meridian_wl_state_t *state)
{
    if (!state) return MERIDIAN_ERR_DRM_RESOURCES;

    /*
     * wlr-gamma-control restores original gamma when the control object
     * is destroyed. Destroy existing controls and re-acquire fresh ones
     * so the object can continue to be used for further set calls.
     */
    for (int i = 0; i < state->output_count; i++) {
        wl_output_state_t *out = &state->outputs[i];
        if (out->gamma_control) {
            zwlr_gamma_control_v1_destroy(out->gamma_control);
            out->gamma_control = nullptr;
        }
        out->failed = false;
        out->gamma_size = 0;
    }

    wl_display_flush(state->display);

    /* Re-acquire gamma controls */
    for (int i = 0; i < state->output_count; i++) {
        wl_output_state_t *out = &state->outputs[i];
        out->gamma_control = zwlr_gamma_control_manager_v1_get_gamma_control(
            state->gamma_manager, out->wl_output);
        zwlr_gamma_control_v1_add_listener(out->gamma_control,
                                            &gamma_control_listener, out);
    }

    wl_display_roundtrip(state->display);
    return MERIDIAN_OK;
}

#endif /* MERIDIAN_HAS_WAYLAND */
