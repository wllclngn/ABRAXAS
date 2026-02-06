/*
 * gamma_gnome.c - GNOME/Mutter gamma control via sd-bus
 *
 * Uses org.gnome.Mutter.DisplayConfig.SetCrtcGamma to set gamma
 * ramps on GNOME Wayland sessions (Mutter compositor).
 *
 * Covers: GNOME on Debian, Ubuntu, Fedora, RHEL, etc.
 * Requires libsystemd (-lsystemd) for sd-bus.
 */

#ifdef MERIDIAN_HAS_GNOME

#define _GNU_SOURCE
#include "meridian.h"

#include <stdlib.h>
#include <string.h>

#include <systemd/sd-bus.h>

#define MUTTER_DBUS_NAME  "org.gnome.Mutter.DisplayConfig"
#define MUTTER_DBUS_PATH  "/org/gnome/Mutter/DisplayConfig"
#define MUTTER_DBUS_IFACE "org.gnome.Mutter.DisplayConfig"

/* Mutter doesn't expose gamma ramp size; hardcode 256 */
#define GNOME_GAMMA_SIZE 256

/* ============================================================
 * CRTC state
 * ============================================================ */

typedef struct {
    uint32_t crtc_id;
} gnome_crtc_t;

/* ============================================================
 * GNOME state
 * ============================================================ */

struct meridian_gnome_state {
    sd_bus *bus;
    uint32_t serial;
    int crtc_count;
    gnome_crtc_t *crtcs;
};

/* ============================================================
 * GetResources: parse CRTC IDs and serial from Mutter DBus
 * ============================================================ */

/*
 * GetResources returns: (ua(uxiiiiiuaua{sv})a(uxiausauau)a(uxuudu)ii)
 *
 * We need:
 *   - serial (first 'u')
 *   - crtcs array: a(uxiiiiiuaua{sv}) - we only need the CRTC ID (first 'u' in each struct)
 */
static meridian_error_t
gnome_get_resources(struct meridian_gnome_state *state)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = nullptr;
    int r;

    r = sd_bus_call_method(state->bus,
                           MUTTER_DBUS_NAME,
                           MUTTER_DBUS_PATH,
                           MUTTER_DBUS_IFACE,
                           "GetResources",
                           &error, &reply, "");
    if (r < 0) {
        sd_bus_error_free(&error);
        return MERIDIAN_ERR_GNOME_DBUS;
    }

    /* Read serial */
    r = sd_bus_message_read(reply, "u", &state->serial);
    if (r < 0) goto fail;

    /* Enter CRTC array: a(uxiiiiiuaua{sv}) */
    r = sd_bus_message_enter_container(reply, 'a', "(uxiiiiiuaua{sv})");
    if (r < 0) goto fail;

    /* Count and collect CRTC IDs */
    int capacity = 4;
    state->crtcs = malloc(capacity * sizeof(gnome_crtc_t));
    if (!state->crtcs) goto fail;
    state->crtc_count = 0;

    while ((r = sd_bus_message_enter_container(reply, 'r', "uxiiiiiuaua{sv}")) > 0) {
        uint32_t crtc_id;
        r = sd_bus_message_read(reply, "u", &crtc_id);
        if (r < 0) goto fail;

        /* Skip remaining fields in this CRTC struct */
        r = sd_bus_message_skip(reply, "xiiiiiuaua{sv}");
        if (r < 0) goto fail;

        r = sd_bus_message_exit_container(reply);
        if (r < 0) goto fail;

        if (state->crtc_count >= capacity) {
            capacity *= 2;
            gnome_crtc_t *new_arr = realloc(state->crtcs, capacity * sizeof(gnome_crtc_t));
            if (!new_arr) goto fail;
            state->crtcs = new_arr;
        }

        state->crtcs[state->crtc_count].crtc_id = crtc_id;
        state->crtc_count++;
    }

    /* Exit CRTC array */
    sd_bus_message_exit_container(reply);

    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);

    return (state->crtc_count > 0) ? MERIDIAN_OK : MERIDIAN_ERR_NO_CRTC;

fail:
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    free(state->crtcs);
    state->crtcs = nullptr;
    state->crtc_count = 0;
    return MERIDIAN_ERR_GNOME_DBUS;
}

/* ============================================================
 * Init / Free
 * ============================================================ */

meridian_error_t
meridian_gnome_init(meridian_gnome_state_t **state_out)
{
    struct meridian_gnome_state *state = calloc(1, sizeof(*state));
    if (!state) return MERIDIAN_ERR_RESOURCES;

    int r = sd_bus_open_user(&state->bus);
    if (r < 0) {
        free(state);
        return MERIDIAN_ERR_GNOME_DBUS;
    }

    meridian_error_t err = gnome_get_resources(state);
    if (err != MERIDIAN_OK) {
        sd_bus_unref(state->bus);
        free(state);
        return err;
    }

    *state_out = state;
    return MERIDIAN_OK;
}

void
meridian_gnome_free(meridian_gnome_state_t *state)
{
    if (!state) return;

    /* Restore linear gamma before shutting down */
    (void)meridian_gnome_restore(state);

    free(state->crtcs);
    if (state->bus) sd_bus_unref(state->bus);
    free(state);
}

/* ============================================================
 * Getters
 * ============================================================ */

int
meridian_gnome_get_crtc_count(const meridian_gnome_state_t *state)
{
    return state ? state->crtc_count : 0;
}

int
meridian_gnome_get_gamma_size(const meridian_gnome_state_t *state [[maybe_unused]],
                              int crtc_idx [[maybe_unused]])
{
    if (!state || crtc_idx < 0 || crtc_idx >= state->crtc_count) return 0;
    return GNOME_GAMMA_SIZE;
}

/* ============================================================
 * Set gamma via SetCrtcGamma DBus call
 *
 * Signature: SetCrtcGamma(uu aq aq aq)
 *   serial, crtc_id, red[], green[], blue[]
 * ============================================================ */

static meridian_error_t
gnome_set_gamma_crtc(struct meridian_gnome_state *state, int crtc_idx,
                     const uint16_t *r, const uint16_t *g, const uint16_t *b)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *msg = nullptr;
    int ret;

    ret = sd_bus_message_new_method_call(state->bus, &msg,
                                          MUTTER_DBUS_NAME,
                                          MUTTER_DBUS_PATH,
                                          MUTTER_DBUS_IFACE,
                                          "SetCrtcGamma");
    if (ret < 0) return MERIDIAN_ERR_GNOME_DBUS;

    /* Append serial and CRTC ID */
    ret = sd_bus_message_append(msg, "uu", state->serial,
                                state->crtcs[crtc_idx].crtc_id);
    if (ret < 0) goto fail;

    /* Append three gamma ramp arrays (aq = array of uint16) */
    ret = sd_bus_message_append_array(msg, 'q', r, GNOME_GAMMA_SIZE * sizeof(uint16_t));
    if (ret < 0) goto fail;

    ret = sd_bus_message_append_array(msg, 'q', g, GNOME_GAMMA_SIZE * sizeof(uint16_t));
    if (ret < 0) goto fail;

    ret = sd_bus_message_append_array(msg, 'q', b, GNOME_GAMMA_SIZE * sizeof(uint16_t));
    if (ret < 0) goto fail;

    ret = sd_bus_call(state->bus, msg, 0, &error, nullptr);
    sd_bus_message_unref(msg);
    sd_bus_error_free(&error);

    return (ret < 0) ? MERIDIAN_ERR_GNOME_DBUS : MERIDIAN_OK;

fail:
    sd_bus_message_unref(msg);
    sd_bus_error_free(&error);
    return MERIDIAN_ERR_GNOME_DBUS;
}

meridian_error_t
meridian_gnome_set_temperature_crtc(meridian_gnome_state_t *state, int crtc_idx,
                                    int temp, float brightness)
{
    if (!state || crtc_idx < 0 || crtc_idx >= state->crtc_count) {
        return MERIDIAN_ERR_GNOME_DBUS;
    }

    uint16_t r[GNOME_GAMMA_SIZE], g[GNOME_GAMMA_SIZE], b[GNOME_GAMMA_SIZE];
    meridian_error_t err = meridian_fill_gamma_ramps(temp, GNOME_GAMMA_SIZE,
                                                      r, g, b, brightness);
    if (err != MERIDIAN_OK) return err;

    return gnome_set_gamma_crtc(state, crtc_idx, r, g, b);
}

meridian_error_t
meridian_gnome_set_temperature(meridian_gnome_state_t *state,
                               int temp, float brightness)
{
    if (!state) return MERIDIAN_ERR_RESOURCES;

    meridian_error_t last_err = MERIDIAN_OK;
    int success_count = 0;

    for (int i = 0; i < state->crtc_count; i++) {
        meridian_error_t err = meridian_gnome_set_temperature_crtc(state, i,
                                                                    temp, brightness);
        if (err == MERIDIAN_OK) {
            success_count++;
        } else {
            last_err = err;
        }
    }

    return (success_count > 0) ? MERIDIAN_OK : last_err;
}

/* ============================================================
 * Restore: send identity (linear) gamma ramps
 * ============================================================ */

meridian_error_t
meridian_gnome_restore(meridian_gnome_state_t *state)
{
    if (!state) return MERIDIAN_ERR_RESOURCES;

    /* Build linear ramp */
    uint16_t r[GNOME_GAMMA_SIZE], g[GNOME_GAMMA_SIZE], b[GNOME_GAMMA_SIZE];
    for (int i = 0; i < GNOME_GAMMA_SIZE; i++) {
        uint16_t val = (uint16_t)((float)i / (GNOME_GAMMA_SIZE - 1) * UINT16_MAX);
        r[i] = g[i] = b[i] = val;
    }

    meridian_error_t last_err = MERIDIAN_OK;
    for (int i = 0; i < state->crtc_count; i++) {
        meridian_error_t err = gnome_set_gamma_crtc(state, i, r, g, b);
        if (err != MERIDIAN_OK) last_err = err;
    }

    return last_err;
}

#endif /* MERIDIAN_HAS_GNOME */
