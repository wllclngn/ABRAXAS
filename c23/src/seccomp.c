/*
 * seccomp.c - BPF syscall whitelist for ABRAXAS daemon
 *
 * Restricts the process to only the syscalls needed for the event loop.
 * Uses raw BPF instructions + prctl(PR_SET_SECCOMP). No libseccomp.
 *
 * SECCOMP_RET_KILL_PROCESS on any syscall not in the whitelist.
 * This is defense-in-depth: even if an attacker gets code execution,
 * they can't call dangerous syscalls like ptrace, mount, etc.
 */

#define _GNU_SOURCE

#include "seccomp.h"

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

/* Architecture audit value for x86_64 */
#ifndef AUDIT_ARCH_X86_64
#define AUDIT_ARCH_X86_64 (EM_X86_64 | __AUDIT_ARCH_64BIT | __AUDIT_ARCH_LE)
#endif

/* Allow a syscall: if nr == syscall_nr, return ALLOW */
#define ALLOW_SYSCALL(nr) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

bool seccomp_install_filter(void)
{
    struct sock_filter filter[] = {
        /* Load architecture */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 (offsetof(struct seccomp_data, arch))),

        /* Verify x86_64 -- kill if wrong arch */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),

        /* Load syscall number */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 (offsetof(struct seccomp_data, nr))),

        /* --- Whitelist: core I/O --- */
        ALLOW_SYSCALL(__NR_read),
        ALLOW_SYSCALL(__NR_write),
        ALLOW_SYSCALL(__NR_openat),
        ALLOW_SYSCALL(__NR_close),
        ALLOW_SYSCALL(__NR_fstat),
        ALLOW_SYSCALL(__NR_newfstatat),
        ALLOW_SYSCALL(__NR_lseek),
        ALLOW_SYSCALL(__NR_pread64),

        /* --- Whitelist: memory --- */
        ALLOW_SYSCALL(__NR_mmap),
        ALLOW_SYSCALL(__NR_munmap),
        ALLOW_SYSCALL(__NR_mprotect),
        ALLOW_SYSCALL(__NR_brk),
        ALLOW_SYSCALL(__NR_mremap),

        /* --- Whitelist: io_uring --- */
        ALLOW_SYSCALL(__NR_io_uring_setup),
        ALLOW_SYSCALL(__NR_io_uring_enter),
        ALLOW_SYSCALL(__NR_io_uring_register),

        /* --- Whitelist: time --- */
        ALLOW_SYSCALL(__NR_clock_gettime),
        ALLOW_SYSCALL(__NR_clock_nanosleep),
        ALLOW_SYSCALL(__NR_nanosleep),
        ALLOW_SYSCALL(__NR_gettimeofday),

        /* --- Whitelist: inotify + ioctl --- */
        ALLOW_SYSCALL(__NR_ioctl),

        /* --- Whitelist: process spawn (weather via curl) --- */
        ALLOW_SYSCALL(__NR_clone3),
        ALLOW_SYSCALL(__NR_clone),
        ALLOW_SYSCALL(__NR_execve),
        ALLOW_SYSCALL(__NR_pipe2),
        ALLOW_SYSCALL(__NR_dup2),
        ALLOW_SYSCALL(__NR_dup3),
        ALLOW_SYSCALL(__NR_wait4),
        ALLOW_SYSCALL(__NR_set_robust_list),
        ALLOW_SYSCALL(__NR_rseq),
        ALLOW_SYSCALL(__NR_prlimit64),
        ALLOW_SYSCALL(__NR_arch_prctl),
        ALLOW_SYSCALL(__NR_set_tid_address),

        /* --- Whitelist: signals --- */
        ALLOW_SYSCALL(__NR_rt_sigprocmask),
        ALLOW_SYSCALL(__NR_rt_sigaction),
        ALLOW_SYSCALL(__NR_rt_sigreturn),
        ALLOW_SYSCALL(__NR_sigaltstack),

        /* --- Whitelist: file ops --- */
        ALLOW_SYSCALL(__NR_unlink),
        ALLOW_SYSCALL(__NR_unlinkat),
        ALLOW_SYSCALL(__NR_mkdir),
        ALLOW_SYSCALL(__NR_mkdirat),
        ALLOW_SYSCALL(__NR_access),
        ALLOW_SYSCALL(__NR_faccessat2),
        ALLOW_SYSCALL(__NR_fcntl),
        ALLOW_SYSCALL(__NR_getcwd),
        ALLOW_SYSCALL(__NR_readlink),
        ALLOW_SYSCALL(__NR_readlinkat),
        ALLOW_SYSCALL(__NR_statx),
        ALLOW_SYSCALL(__NR_getrandom),

        /* --- Whitelist: process info --- */
        ALLOW_SYSCALL(__NR_getpid),
        ALLOW_SYSCALL(__NR_getuid),
        ALLOW_SYSCALL(__NR_geteuid),
        ALLOW_SYSCALL(__NR_getgid),
        ALLOW_SYSCALL(__NR_getegid),
        ALLOW_SYSCALL(__NR_kill),
        ALLOW_SYSCALL(__NR_prctl),
        ALLOW_SYSCALL(__NR_futex),

        /* --- Whitelist: exit --- */
        ALLOW_SYSCALL(__NR_exit),
        ALLOW_SYSCALL(__NR_exit_group),

        /* --- Whitelist: select fallback --- */
        ALLOW_SYSCALL(__NR_select),
        ALLOW_SYSCALL(__NR_pselect6),
        ALLOW_SYSCALL(__NR_timerfd_create),
        ALLOW_SYSCALL(__NR_timerfd_settime),
        ALLOW_SYSCALL(__NR_signalfd4),
        ALLOW_SYSCALL(__NR_inotify_init1),
        ALLOW_SYSCALL(__NR_inotify_add_watch),

        /* --- Whitelist: socket I/O (X11/Wayland backend, curl child) --- */
        ALLOW_SYSCALL(__NR_socket),
        ALLOW_SYSCALL(__NR_connect),
        ALLOW_SYSCALL(__NR_sendto),
        ALLOW_SYSCALL(__NR_sendmsg),
        ALLOW_SYSCALL(__NR_recvfrom),
        ALLOW_SYSCALL(__NR_recvmsg),
        ALLOW_SYSCALL(__NR_getpeername),
        ALLOW_SYSCALL(__NR_getsockname),
        ALLOW_SYSCALL(__NR_poll),
        ALLOW_SYSCALL(__NR_ppoll),
        ALLOW_SYSCALL(__NR_writev),
        ALLOW_SYSCALL(__NR_uname),

        /* --- Whitelist: dlopen (backend loading) --- */
        ALLOW_SYSCALL(__NR_getdents64),

        /* Default: KILL */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
    };

    struct sock_fprog prog = {
        .len    = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) < 0)
        return false;

    return true;
}
