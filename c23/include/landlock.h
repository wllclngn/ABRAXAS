/*
 * landlock.h - Landlock filesystem sandbox
 *
 * Restricts the daemon's filesystem access to only the paths it needs:
 *   - config_dir (RW): config, override, weather cache, PID
 *   - /dev (RO): DRM device nodes for gamma ioctls
 *   - /usr/bin/curl (execute): weather fetch
 *   - /proc (RO): process info
 *
 * No library dependency -- uses raw landlock syscalls.
 */

#ifndef ABRAXAS_LANDLOCK_H
#define ABRAXAS_LANDLOCK_H

#include <stdbool.h>

/* Install landlock filesystem sandbox. Returns true on success.
 * Gracefully returns false if kernel doesn't support landlock. */
bool landlock_install_sandbox(const char *config_dir);

#endif /* ABRAXAS_LANDLOCK_H */
