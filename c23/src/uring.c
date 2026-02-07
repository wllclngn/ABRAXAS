/*
 * uring.c - Raw io_uring implementation via syscall()
 *
 * No liburing dependency. Talks directly to the kernel through:
 *   - syscall(__NR_io_uring_setup, entries, &params)
 *   - syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, ...)
 *   - mmap for SQ/CQ ring buffers and SQE array
 *
 * Memory ordering: read_barrier/write_barrier around shared ring indices.
 * The kernel writes cq_tail and reads sq_tail; userspace does the inverse.
 */

#define _GNU_SOURCE

#include "uring.h"

#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Memory barriers for ring buffer synchronization */
#define read_barrier()  atomic_thread_fence(memory_order_acquire)
#define write_barrier() atomic_thread_fence(memory_order_release)

static inline int sys_io_uring_setup(uint32_t entries,
                                     struct io_uring_params *p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static inline int sys_io_uring_enter(int fd, uint32_t to_submit,
                                     uint32_t min_complete, uint32_t flags,
                                     void *sig, size_t sigsz)
{
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete,
                        flags, sig, sigsz);
}

bool uring_init(abraxas_ring_t *ring, uint32_t entries)
{
    memset(ring, 0, sizeof(*ring));
    ring->ring_fd = -1;

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    int fd = sys_io_uring_setup(entries, &params);
    if (fd < 0) return false;

    ring->ring_fd = fd;
    ring->sq_entries = params.sq_entries;
    ring->cq_entries = params.cq_entries;

    /* Map SQ ring */
    ring->sq_ring_size = params.sq_off.array +
                         params.sq_entries * sizeof(uint32_t);
    ring->sq_ring_ptr = mmap(nullptr, ring->sq_ring_size,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_POPULATE,
                             fd, IORING_OFF_SQ_RING);
    if (ring->sq_ring_ptr == MAP_FAILED) goto fail;

    char *sq = ring->sq_ring_ptr;
    ring->sq_head  = (uint32_t *)(sq + params.sq_off.head);
    ring->sq_tail  = (uint32_t *)(sq + params.sq_off.tail);
    ring->sq_mask  = (uint32_t *)(sq + params.sq_off.ring_mask);
    ring->sq_array = (uint32_t *)(sq + params.sq_off.array);

    /* Map SQE array */
    ring->sqes_size = params.sq_entries * sizeof(struct io_uring_sqe);
    ring->sqes = mmap(nullptr, ring->sqes_size,
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE,
                      fd, IORING_OFF_SQES);
    if (ring->sqes == MAP_FAILED) goto fail;

    /* Map CQ ring */
    ring->cq_ring_size = params.cq_off.cqes +
                         params.cq_entries * sizeof(struct io_uring_cqe);
    ring->cq_ring_ptr = mmap(nullptr, ring->cq_ring_size,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_POPULATE,
                             fd, IORING_OFF_CQ_RING);
    if (ring->cq_ring_ptr == MAP_FAILED) goto fail;

    char *cq = ring->cq_ring_ptr;
    ring->cq_head = (uint32_t *)(cq + params.cq_off.head);
    ring->cq_tail = (uint32_t *)(cq + params.cq_off.tail);
    ring->cq_mask = (uint32_t *)(cq + params.cq_off.ring_mask);
    ring->cqes    = (struct io_uring_cqe *)(cq + params.cq_off.cqes);

    return true;

fail:
    uring_destroy(ring);
    return false;
}

void uring_destroy(abraxas_ring_t *ring)
{
    if (ring->sqes && ring->sqes != MAP_FAILED)
        munmap(ring->sqes, ring->sqes_size);
    if (ring->sq_ring_ptr && ring->sq_ring_ptr != MAP_FAILED)
        munmap(ring->sq_ring_ptr, ring->sq_ring_size);
    if (ring->cq_ring_ptr && ring->cq_ring_ptr != MAP_FAILED)
        munmap(ring->cq_ring_ptr, ring->cq_ring_size);
    if (ring->ring_fd >= 0)
        close(ring->ring_fd);

    memset(ring, 0, sizeof(*ring));
    ring->ring_fd = -1;
}

/* Get next SQE slot */
static struct io_uring_sqe *get_sqe(abraxas_ring_t *ring)
{
    uint32_t tail = *ring->sq_tail;
    uint32_t head = *ring->sq_head;

    /* Ring full? */
    if (tail - head >= ring->sq_entries)
        return nullptr;

    uint32_t idx = tail & *ring->sq_mask;
    ring->sq_array[idx] = idx;

    struct io_uring_sqe *sqe = &ring->sqes[idx];
    memset(sqe, 0, sizeof(*sqe));

    /* Advance tail AFTER filling the SQE (caller does this) */
    return sqe;
}

/* Commit one SQE by advancing sq_tail */
static void commit_sqe(abraxas_ring_t *ring)
{
    write_barrier();
    (*ring->sq_tail)++;
}

/* Multi-shot POLL_ADD: fd stays monitored until closed or cancelled. */
void uring_prep_poll(abraxas_ring_t *ring, int fd, uint64_t user_data)
{
    struct io_uring_sqe *sqe = get_sqe(ring);
    if (!sqe) return;

    sqe->opcode        = IORING_OP_POLL_ADD;
    sqe->fd            = fd;
    sqe->len           = IORING_POLL_ADD_MULTI;
    sqe->poll32_events = POLLIN;
    sqe->user_data     = user_data;

    commit_sqe(ring);
}

void uring_prep_timeout(abraxas_ring_t *ring,
                        struct __kernel_timespec *ts, uint64_t user_data)
{
    struct io_uring_sqe *sqe = get_sqe(ring);
    if (!sqe) return;

    sqe->opcode    = IORING_OP_TIMEOUT;
    sqe->fd        = -1;
    sqe->addr      = (uint64_t)(uintptr_t)ts;
    sqe->len       = 1;  /* 1 timespec entry; event count is sqe->off (0 = pure timeout) */
    sqe->user_data = user_data;

    commit_sqe(ring);
}

void uring_prep_cancel(abraxas_ring_t *ring, uint64_t target_user_data,
                       uint64_t user_data)
{
    struct io_uring_sqe *sqe = get_sqe(ring);
    if (!sqe) return;

    sqe->opcode    = IORING_OP_ASYNC_CANCEL;
    sqe->fd        = -1;
    sqe->addr      = target_user_data;
    sqe->user_data = user_data;

    commit_sqe(ring);
}

int uring_submit_and_wait(abraxas_ring_t *ring)
{
    uint32_t tail = *ring->sq_tail;
    uint32_t head;
    read_barrier();
    head = *ring->sq_head;

    uint32_t to_submit = tail - head;
    if (to_submit == 0) return 0;

    int ret = sys_io_uring_enter(ring->ring_fd, to_submit, 1,
                                 IORING_ENTER_GETEVENTS, nullptr, 0);
    if (ret < 0 && errno == EINTR)
        return 0;

    return ret;
}

bool uring_peek_cqe(abraxas_ring_t *ring, struct io_uring_cqe **cqe_out)
{
    uint32_t head = *ring->cq_head;
    read_barrier();
    uint32_t tail = *ring->cq_tail;

    if (head == tail) {
        *cqe_out = nullptr;
        return false;
    }

    *cqe_out = &ring->cqes[head & *ring->cq_mask];
    return true;
}

void uring_cqe_seen(abraxas_ring_t *ring)
{
    write_barrier();
    (*ring->cq_head)++;
}
