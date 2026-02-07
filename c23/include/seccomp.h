/*
 * seccomp.h - seccomp-bpf syscall whitelist
 *
 * Installs a BPF filter that restricts the daemon to only the syscalls
 * needed for its event loop. Any unexpected syscall kills the process.
 * No libseccomp dependency -- uses raw prctl + BPF instructions.
 */

#ifndef ABRAXAS_SECCOMP_H
#define ABRAXAS_SECCOMP_H

#include <stdbool.h>

/* Install seccomp-bpf filter. Returns true on success.
 * Must be called after all fds are created and init is complete.
 * Requires PR_SET_NO_NEW_PRIVS to be set first. */
bool seccomp_install_filter(void);

#endif /* ABRAXAS_SECCOMP_H */
