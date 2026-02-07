/*
 * uring.h - Thin io_uring wrapper using raw syscalls
 *
 * No liburing dependency. Uses linux/io_uring.h kernel headers and
 * syscall(__NR_io_uring_*) directly. Designed for ABRAXAS's simple
 * use case: poll 2 fds + timeout, single io_uring_enter per tick.
 */

#ifndef ABRAXAS_URING_H
#define ABRAXAS_URING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/io_uring.h>

typedef struct {
    int ring_fd;

    /* Submission ring */
    void     *sq_ring_ptr;
    size_t    sq_ring_size;
    uint32_t *sq_head;
    uint32_t *sq_tail;
    uint32_t *sq_mask;
    uint32_t *sq_array;
    uint32_t  sq_entries;
    struct io_uring_sqe *sqes;
    size_t    sqes_size;

    /* Completion ring */
    void     *cq_ring_ptr;
    size_t    cq_ring_size;
    uint32_t *cq_head;
    uint32_t *cq_tail;
    uint32_t *cq_mask;
    uint32_t  cq_entries;
    struct io_uring_cqe *cqes;
} abraxas_ring_t;

/* Initialize io_uring with given queue depth. Returns false on failure. */
bool uring_init(abraxas_ring_t *ring, uint32_t entries);

/* Tear down io_uring (munmap + close). */
void uring_destroy(abraxas_ring_t *ring);

/* Prepare a multi-shot POLL_ADD SQE. fd stays monitored until closed/cancelled. */
void uring_prep_poll(abraxas_ring_t *ring, int fd, uint64_t user_data);

/* Prepare a TIMEOUT SQE. ts is relative time. user_data identifies the event. */
void uring_prep_timeout(abraxas_ring_t *ring,
                        struct __kernel_timespec *ts, uint64_t user_data);

/* Submit all prepared SQEs and wait for at least 1 completion. Returns CQE count or -1. */
int uring_submit_and_wait(abraxas_ring_t *ring);

/* Peek at next CQE without consuming. Returns false if no CQEs available. */
bool uring_peek_cqe(abraxas_ring_t *ring, struct io_uring_cqe **cqe_out);

/* Mark the current CQE as consumed (advance cq_head). */
void uring_cqe_seen(abraxas_ring_t *ring);

/* Cancel a pending operation by user_data. Used to cancel timeout on early wake. */
void uring_prep_cancel(abraxas_ring_t *ring, uint64_t target_user_data,
                       uint64_t user_data);

#endif /* ABRAXAS_URING_H */
