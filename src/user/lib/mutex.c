#include "user/mutex.h"
#include "user/ulib.h"

/* Counts failed acquisitions, so a test can prove the lock was under real
 * contention rather than merely present. */
static uint32_t contention;

uint32_t mutex_contention_count(void)
{
    return contention;
}

uint32_t mutex_try_lock(mutex_t *m)
{
    return compare_and_swap(&m->locked, MUTEX_UNLOCKED, MUTEX_LOCKED);
}

void mutex_lock(mutex_t *m)
{
    while (compare_and_swap(&m->locked, MUTEX_UNLOCKED, MUTEX_LOCKED) == 0) {
        contention++;

        /* The lock is held, and on one core the holder is by definition NOT
         * running -- we are. Spinning here would burn the rest of the timeslice
         * to discover, at the end of it, that nothing changed; the holder can
         * only make progress once we stop. So the failed attempt hands the CPU
         * back immediately rather than waiting for the timer.
         *
         * This is what makes a spinlock tolerable on a uniprocessor at all. A
         * pure spin would still be correct, just wasteful: correctness comes
         * from the compare and swap, liveness comes from the yield. */
        u_yield();
    }
}

void mutex_unlock(mutex_t *m)
{
    /* A plain store is enough for the hardware. An aligned 32-bit store is
     * atomic on x86, and stores are not reordered with earlier stores, so
     * everything written inside the critical section is visible before the
     * release is.
     *
     * The COMPILER is the one that needs telling. Without this barrier it may
     * sink a write from inside the critical section past the release, which
     * would let the next holder observe a half-finished update -- exactly the
     * corruption the lock exists to prevent, reintroduced by the optimizer
     * after the lock did its job. */
    __asm__ volatile ("" ::: "memory");

    m->locked = MUTEX_UNLOCKED;
}
