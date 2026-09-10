#ifndef IPC_MUTEX_PROTO_H
#define IPC_MUTEX_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "ipc/ipc.h"
#include "user/mutex.h"

/* The contract between the two contending processes.
 *
 * Both map the same physical frame and both cast it to this struct, so the
 * layout is shared between two separately compiled binaries and cannot drift.
 * The lock is the first member because it must live inside the shared frame
 * itself -- a lock in either process's private memory would be two locks, each
 * of which always looks free to its owner. */

/* Fixed by creation order. B is started first so it is already blocked in recv
 * when A offers it the segment. */
#define MUTEX_B_PID 5u

#define MSG_MUTEX_OFFER 30u /* A -> B: data = { u32 shm id }              */
#define MSG_MUTEX_SYNC  31u /* rendezvous, so both phases actually contend */

#define MUTEX_ROUNDS   50u /* writes each process performs */
#define MUTEX_TEXT_MAX 72u

struct mutex_shared {
    mutex_t lock; /* offset 0, inside the shared frame, per the constraint */

    /* The contended resource: one buffer both processes write in full and then
     * read back. A write that gets interleaved with the other process's leaves
     * a mixture of both strings, which is exactly what the read-back detects. */
    char text[MUTEX_TEXT_MAX];
};

_Static_assert(sizeof(struct mutex_shared) <= 4096, "the shared view must fit one page");

/* The constraint that makes this a real mutex rather than two private ones. */
_Static_assert(offsetof(struct mutex_shared, lock) == 0,
               "the lock must sit at the start of the shared frame");

/* Same length on purpose: two equal-length strings interleave into something
 * the length alone would not reveal, so the check has to compare content. */
#define MUTEX_TEXT_A "AAAA-TASK-A-WROTE-THIS-WHOLE-STRING-WITHOUT-BEING-INTERRUPTED-AAAA"
#define MUTEX_TEXT_B "BBBB-TASK-B-WROTE-THIS-WHOLE-STRING-WITHOUT-BEING-INTERRUPTED-BBBB"

_Static_assert(sizeof(MUTEX_TEXT_A) == sizeof(MUTEX_TEXT_B),
               "the two strings must be the same length for interleaving to be the only difference");
_Static_assert(sizeof(MUTEX_TEXT_A) <= MUTEX_TEXT_MAX, "text does not fit the shared buffer");

#endif /* IPC_MUTEX_PROTO_H */
