/*
 * landlock.c - Landlock filesystem sandbox for ABRAXAS daemon
 *
 * After init, restricts filesystem access to only what the daemon needs.
 * Uses raw landlock_* syscalls via <linux/landlock.h>. No library.
 *
 * Gracefully fails on kernels without landlock support (pre-5.13).
 */

#define _GNU_SOURCE

#include "landlock.h"

#include <fcntl.h>
#include <linux/landlock.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Syscall wrappers -- not in glibc yet */
static inline int ll_create_ruleset(const struct landlock_ruleset_attr *attr,
                                    size_t size, uint32_t flags)
{
    return (int)syscall(__NR_landlock_create_ruleset, attr, size, flags);
}

static inline int ll_add_rule(int fd, enum landlock_rule_type type,
                              const void *attr, uint32_t flags)
{
    return (int)syscall(__NR_landlock_add_rule, fd, type, attr, flags);
}

static inline int ll_restrict_self(int fd, uint32_t flags)
{
    return (int)syscall(__NR_landlock_restrict_self, fd, flags);
}

/* Add a path rule to the ruleset */
static bool add_path_rule(int ruleset_fd, const char *path, uint64_t access)
{
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return false;

    struct landlock_path_beneath_attr rule = {
        .allowed_access = access,
        .parent_fd      = fd,
    };

    int ret = ll_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &rule, 0);
    close(fd);
    return ret == 0;
}

bool landlock_install_sandbox(const char *config_dir)
{
    /* Check kernel support */
    int abi = ll_create_ruleset(nullptr, 0,
                                LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0) return false;

    /* Define handled access types */
    struct landlock_ruleset_attr attr = {
        .handled_access_fs =
            LANDLOCK_ACCESS_FS_READ_FILE |
            LANDLOCK_ACCESS_FS_READ_DIR |
            LANDLOCK_ACCESS_FS_WRITE_FILE |
            LANDLOCK_ACCESS_FS_REMOVE_FILE |
            LANDLOCK_ACCESS_FS_MAKE_REG |
            LANDLOCK_ACCESS_FS_MAKE_DIR |
            LANDLOCK_ACCESS_FS_EXECUTE,
    };

    int ruleset_fd = ll_create_ruleset(&attr, sizeof(attr), 0);
    if (ruleset_fd < 0) return false;

    /* ~/.config/abraxas/ -- full read/write */
    uint64_t config_access =
        LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR |
        LANDLOCK_ACCESS_FS_WRITE_FILE |
        LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_MAKE_DIR;
    add_path_rule(ruleset_fd, config_dir, config_access);

    /* /dev -- read for DRM ioctls */
    add_path_rule(ruleset_fd, "/dev",
                  LANDLOCK_ACCESS_FS_READ_FILE |
                  LANDLOCK_ACCESS_FS_READ_DIR);

    /* /proc -- read for process info */
    add_path_rule(ruleset_fd, "/proc",
                  LANDLOCK_ACCESS_FS_READ_FILE |
                  LANDLOCK_ACCESS_FS_READ_DIR);

    /* /usr -- execute for curl, read for shared libs */
    add_path_rule(ruleset_fd, "/usr",
                  LANDLOCK_ACCESS_FS_READ_FILE |
                  LANDLOCK_ACCESS_FS_READ_DIR |
                  LANDLOCK_ACCESS_FS_EXECUTE);

    /* /etc -- read for timezone, resolver, etc */
    add_path_rule(ruleset_fd, "/etc",
                  LANDLOCK_ACCESS_FS_READ_FILE |
                  LANDLOCK_ACCESS_FS_READ_DIR);

    /* /lib, /lib64 -- shared libraries for dlopen backends */
    add_path_rule(ruleset_fd, "/lib",
                  LANDLOCK_ACCESS_FS_READ_FILE |
                  LANDLOCK_ACCESS_FS_READ_DIR);
    add_path_rule(ruleset_fd, "/lib64",
                  LANDLOCK_ACCESS_FS_READ_FILE |
                  LANDLOCK_ACCESS_FS_READ_DIR);

    /* /tmp -- curl may need temp files */
    add_path_rule(ruleset_fd, "/tmp",
                  LANDLOCK_ACCESS_FS_READ_FILE |
                  LANDLOCK_ACCESS_FS_WRITE_FILE |
                  LANDLOCK_ACCESS_FS_MAKE_REG);

    /* Enforce */
    int ret = ll_restrict_self(ruleset_fd, 0);
    close(ruleset_fd);

    return ret == 0;
}
