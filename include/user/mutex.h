#ifndef USER_MUTEX_H
#define USER_MUTEX_H

#include <stdint.h>

/* A mutex two processes can share.
 *
 * The whole struct is one word, deliberately: it is meant to live INSIDE the
 * shared page, so both processes contend on the same physical memory rather
 * than on two private copies that would each look unlocked. Its layout is
 * therefore part of the contract between separately compiled programs, which
 * is why the size is asserted rather than assumed. */
typedef struct {
    /* volatile because the other process changes it and this one's compiler
     * cannot see that happening. Without it the retry loop is entitled to read
     * the word once, keep it in a register, and spin forever on a stale copy. */
    volatile uint32_t locked;
} mutex_t;

_Static_assert(sizeof(mutex_t) == 4, "mutex_t must stay one word: it is shared between programs");

#define MUTEX_UNLOCKED 0u
#define MUTEX_LOCKED   1u

/* Returns 1 if *ptr was `expected` and is now `desired`, 0 otherwise. In
 * assembly because the load, compare and store must be one instruction; see
 * src/user/lib/atomic.S. */
uint32_t compare_and_swap(volatile uint32_t *ptr, uint32_t expected, uint32_t desired);

/* Spins until the lock is taken, yielding the CPU on every failed attempt
 * rather than burning the rest of the timeslice on a lock the holder needs to
 * run in order to release. */
void mutex_lock(mutex_t *m);

void mutex_unlock(mutex_t *m);

/* How many times an acquisition has found the lock already held and had to
 * yield. A locked run that reports zero corruptions AND zero contention has not
 * demonstrated anything -- the two processes simply never overlapped. */
uint32_t mutex_contention_count(void);

/* True if the lock was free and is now held; never blocks. */
uint32_t mutex_try_lock(mutex_t *m);

#endif /* USER_MUTEX_H */
