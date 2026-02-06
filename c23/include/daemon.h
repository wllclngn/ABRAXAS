/*
 * daemon.h - Main daemon event loop
 *
 * Uses Linux kernel timerfd, inotify, and signalfd with select().
 * Zero polling. Handles solar transitions, manual overrides, weather refresh.
 */

#ifndef DAEMON_H
#define DAEMON_H

#include "abraxas.h"

/* Run the daemon event loop. Does not return until signal received. */
void daemon_run(daemon_state_t *state);

#endif /* DAEMON_H */
