/*
 * gamma_drm.c - Direct DRM/KMS gamma control via raw kernel ioctl
 *
 * Pure kernel interface - no libdrm dependency.
 * Opens /dev/dri/card* directly, no X11 needed.
 */

#define _GNU_SOURCE
#include "meridian.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

/* ============================================================
 * DRM ioctl definitions (from linux/drm.h and drm/drm_mode.h)
 * ============================================================ */

#define DRM_IOCTL_BASE 'd'

/* ioctl construction macros */
#define DRM_IO(nr)          _IO(DRM_IOCTL_BASE, nr)
#define DRM_IOR(nr, type)   _IOR(DRM_IOCTL_BASE, nr, type)
#define DRM_IOW(nr, type)   _IOW(DRM_IOCTL_BASE, nr, type)
#define DRM_IOWR(nr, type)  _IOWR(DRM_IOCTL_BASE, nr, type)

/* DRM mode command numbers */
#define DRM_IOCTL_MODE_GETRESOURCES  0xA0
#define DRM_IOCTL_MODE_GETCRTC       0xA1
#define DRM_IOCTL_MODE_GETGAMMA      0xA4
#define DRM_IOCTL_MODE_SETGAMMA      0xA5

/* drm_mode_card_res - returned by MODE_GETRESOURCES */
struct drm_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width;
    uint32_t max_width;
    uint32_t min_height;
    uint32_t max_height;
};

/* drm_mode_crtc - returned by MODE_GETCRTC */
struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    /* drm_mode_modeinfo follows but we don't need it */
    char mode[68];  /* sizeof(struct drm_mode_modeinfo) */
};

/* drm_mode_crtc_lut - used by MODE_GETGAMMA and MODE_SETGAMMA */
struct drm_mode_crtc_lut {
    uint32_t crtc_id;
    uint32_t gamma_size;
    uint64_t red;    /* pointer to uint16_t array */
    uint64_t green;  /* pointer to uint16_t array */
    uint64_t blue;   /* pointer to uint16_t array */
};

/* ============================================================
 * Compile-time validation of kernel ABI structures
 * ============================================================ */

static_assert(sizeof(struct drm_mode_card_res) == 64,
              "drm_mode_card_res size mismatch with kernel ABI");
static_assert(sizeof(struct drm_mode_crtc) == 104,
              "drm_mode_crtc size mismatch with kernel ABI");
static_assert(sizeof(struct drm_mode_crtc_lut) == 32,
              "drm_mode_crtc_lut size mismatch with kernel ABI");

/* ============================================================
 * Internal state structures
 * ============================================================ */

/* CRTC state for saving/restoring gamma */
typedef struct {
    uint32_t crtc_id;
    uint32_t gamma_size;
    uint16_t *saved_r;
    uint16_t *saved_g;
    uint16_t *saved_b;
} crtc_state_t;

/* DRM state */
struct meridian_drm_state {
    int fd;
    int card_num;
    int crtc_count;
    uint32_t *crtc_ids;
    crtc_state_t *crtcs;
};

/* ============================================================
 * Raw ioctl wrappers
 * ============================================================ */

static int
drm_get_resources(int fd, struct drm_mode_card_res *res)
{
    memset(res, 0, sizeof(*res));
    return ioctl(fd, DRM_IOWR(DRM_IOCTL_MODE_GETRESOURCES, struct drm_mode_card_res), res);
}

static int
drm_get_crtc(int fd, struct drm_mode_crtc *crtc)
{
    return ioctl(fd, DRM_IOWR(DRM_IOCTL_MODE_GETCRTC, struct drm_mode_crtc), crtc);
}

static int
drm_get_gamma(int fd, struct drm_mode_crtc_lut *lut)
{
    return ioctl(fd, DRM_IOWR(DRM_IOCTL_MODE_GETGAMMA, struct drm_mode_crtc_lut), lut);
}

static int
drm_set_gamma(int fd, struct drm_mode_crtc_lut *lut)
{
    return ioctl(fd, DRM_IOWR(DRM_IOCTL_MODE_SETGAMMA, struct drm_mode_crtc_lut), lut);
}

/* ============================================================
 * Public API
 * ============================================================ */

meridian_error_t
meridian_drm_init(int card_num, meridian_drm_state_t **state_out)
{
    meridian_drm_state_t *state = calloc(1, sizeof(meridian_drm_state_t));
    if (!state) return MERIDIAN_ERR_RESOURCES;

    state->card_num = card_num;
    state->fd = -1;

    /* Open DRM device */
    char path[64];
    snprintf(path, sizeof(path), "/dev/dri/card%d", card_num);

    state->fd = open(path, O_RDWR | O_CLOEXEC);
    if (state->fd < 0) {
        free(state);
        return (errno == EACCES) ? MERIDIAN_ERR_PERMISSION : MERIDIAN_ERR_OPEN;
    }

    /* First call: get count of CRTCs */
    struct drm_mode_card_res res;
    if (drm_get_resources(state->fd, &res) < 0) {
        close(state->fd);
        free(state);
        return MERIDIAN_ERR_RESOURCES;
    }

    if (res.count_crtcs == 0) {
        close(state->fd);
        free(state);
        return MERIDIAN_ERR_NO_CRTC;
    }

    /* Allocate array for CRTC IDs */
    state->crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
    if (!state->crtc_ids) {
        close(state->fd);
        free(state);
        return MERIDIAN_ERR_RESOURCES;
    }

    /* Second call: actually get CRTC IDs */
    res.crtc_id_ptr = (uint64_t)(uintptr_t)state->crtc_ids;
    if (drm_get_resources(state->fd, &res) < 0) {
        free(state->crtc_ids);
        close(state->fd);
        free(state);
        return MERIDIAN_ERR_RESOURCES;
    }

    state->crtc_count = res.count_crtcs;

    /* Allocate CRTC state array */
    state->crtcs = calloc(state->crtc_count, sizeof(crtc_state_t));
    if (!state->crtcs) {
        free(state->crtc_ids);
        close(state->fd);
        free(state);
        return MERIDIAN_ERR_RESOURCES;
    }

    /* Initialize each CRTC and save original gamma */
    for (int i = 0; i < state->crtc_count; i++) {
        crtc_state_t *crtc = &state->crtcs[i];
        crtc->crtc_id = state->crtc_ids[i];

        /* Get CRTC info for gamma size */
        struct drm_mode_crtc crtc_info = { .crtc_id = crtc->crtc_id };
        if (drm_get_crtc(state->fd, &crtc_info) < 0) {
            crtc->gamma_size = 0;
            continue;
        }

        crtc->gamma_size = crtc_info.gamma_size;

        if (crtc->gamma_size <= 1) {
            crtc->gamma_size = 0;
            continue;
        }

        /* Allocate and save original gamma ramps */
        size_t ramp_bytes = crtc->gamma_size * sizeof(uint16_t);
        crtc->saved_r = malloc(ramp_bytes);
        crtc->saved_g = malloc(ramp_bytes);
        crtc->saved_b = malloc(ramp_bytes);

        if (!crtc->saved_r || !crtc->saved_g || !crtc->saved_b) {
            free(crtc->saved_r);
            free(crtc->saved_g);
            free(crtc->saved_b);
            crtc->saved_r = crtc->saved_g = crtc->saved_b = nullptr;
            crtc->gamma_size = 0;
            continue;
        }

        /* Read current gamma via kernel ioctl */
        struct drm_mode_crtc_lut lut = {
            .crtc_id = crtc->crtc_id,
            .gamma_size = crtc->gamma_size,
            .red = (uint64_t)(uintptr_t)crtc->saved_r,
            .green = (uint64_t)(uintptr_t)crtc->saved_g,
            .blue = (uint64_t)(uintptr_t)crtc->saved_b,
        };

        if (drm_get_gamma(state->fd, &lut) < 0) {
            free(crtc->saved_r);
            free(crtc->saved_g);
            free(crtc->saved_b);
            crtc->saved_r = crtc->saved_g = crtc->saved_b = nullptr;
            crtc->gamma_size = 0;
        }
    }

    *state_out = state;
    return MERIDIAN_OK;
}

void
meridian_drm_free(meridian_drm_state_t *state)
{
    if (!state) return;

    /* Restore original gamma on all CRTCs (ignore errors during cleanup) */
    (void)meridian_drm_restore(state);

    /* Free CRTC state */
    for (int i = 0; i < state->crtc_count; i++) {
        free(state->crtcs[i].saved_r);
        free(state->crtcs[i].saved_g);
        free(state->crtcs[i].saved_b);
    }
    free(state->crtcs);
    free(state->crtc_ids);

    /* Close device */
    if (state->fd >= 0) {
        close(state->fd);
    }

    free(state);
}

int
meridian_drm_get_crtc_count(const meridian_drm_state_t *state)
{
    return state ? state->crtc_count : 0;
}

int
meridian_drm_get_gamma_size(const meridian_drm_state_t *state, int crtc_idx)
{
    if (!state || crtc_idx < 0 || crtc_idx >= state->crtc_count) {
        return 0;
    }
    return state->crtcs[crtc_idx].gamma_size;
}

meridian_error_t
meridian_drm_set_temperature_crtc(meridian_drm_state_t *state, int crtc_idx,
                                int temp, float brightness)
{
    if (!state || crtc_idx < 0 || crtc_idx >= state->crtc_count) {
        return MERIDIAN_ERR_CRTC;
    }

    crtc_state_t *crtc = &state->crtcs[crtc_idx];
    if (crtc->gamma_size <= 1) {
        return MERIDIAN_ERR_CRTC;
    }

    /* Allocate temporary ramps */
    size_t ramp_bytes = crtc->gamma_size * sizeof(uint16_t);
    uint16_t *r = malloc(ramp_bytes);
    uint16_t *g = malloc(ramp_bytes);
    uint16_t *b = malloc(ramp_bytes);

    if (!r || !g || !b) {
        free(r); free(g); free(b);
        return MERIDIAN_ERR_RESOURCES;
    }

    /* Fill ramps with color temperature */
    meridian_error_t err = meridian_fill_gamma_ramps(temp, crtc->gamma_size, r, g, b, brightness);
    if (err != MERIDIAN_OK) {
        free(r); free(g); free(b);
        return err;
    }

    /* Set gamma via raw kernel ioctl */
    struct drm_mode_crtc_lut lut = {
        .crtc_id = crtc->crtc_id,
        .gamma_size = crtc->gamma_size,
        .red = (uint64_t)(uintptr_t)r,
        .green = (uint64_t)(uintptr_t)g,
        .blue = (uint64_t)(uintptr_t)b,
    };

    int ret = drm_set_gamma(state->fd, &lut);

    free(r); free(g); free(b);

    return (ret < 0) ? MERIDIAN_ERR_GAMMA : MERIDIAN_OK;
}

meridian_error_t
meridian_drm_set_temperature(meridian_drm_state_t *state, int temp, float brightness)
{
    if (!state) return MERIDIAN_ERR_RESOURCES;

    meridian_error_t last_err = MERIDIAN_OK;
    int success_count = 0;

    for (int i = 0; i < state->crtc_count; i++) {
        if (state->crtcs[i].gamma_size > 1) {
            meridian_error_t err = meridian_drm_set_temperature_crtc(state, i, temp, brightness);
            if (err == MERIDIAN_OK) {
                success_count++;
            } else {
                last_err = err;
            }
        }
    }

    /* Return success if at least one CRTC was set */
    return (success_count > 0) ? MERIDIAN_OK : last_err;
}

meridian_error_t
meridian_drm_restore(meridian_drm_state_t *state)
{
    if (!state) return MERIDIAN_ERR_RESOURCES;

    for (int i = 0; i < state->crtc_count; i++) {
        crtc_state_t *crtc = &state->crtcs[i];
        if (crtc->gamma_size > 1 && crtc->saved_r) {
            /* Restore via raw kernel ioctl */
            struct drm_mode_crtc_lut lut = {
                .crtc_id = crtc->crtc_id,
                .gamma_size = crtc->gamma_size,
                .red = (uint64_t)(uintptr_t)crtc->saved_r,
                .green = (uint64_t)(uintptr_t)crtc->saved_g,
                .blue = (uint64_t)(uintptr_t)crtc->saved_b,
            };
            drm_set_gamma(state->fd, &lut);
        }
    }

    return MERIDIAN_OK;
}
