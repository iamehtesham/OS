/* Task A: creates the shared page, hands B its id, then races B for it.
 *
 * Runs the identical contended write twice: once with no mutex and once with
 * one. Both phases are in the same boot on purpose. A synchronisation test that
 * only ever runs the safe path proves nothing, because "zero corruptions" is
 * also what a lock that does nothing looks like when the race never happened. */

#include <stdbool.h>
#include <stdint.h>

#include "ipc/mutex_proto.h"
#include "user/contend.h"
#include "user/mutex.h"
#include "user/ulib.h"

static char line[192];

/* Rendezvous with B. A sends first and then waits; B waits and then sends. With
 * one mailbox slot each and exactly two participants that cannot deadlock:
 * whoever arrives first is the one blocked in recv, so the other's send always
 * finds an empty slot. */
static void barrier(void)
{
    ipc_message_t msg;

    msg.sender_pid   = 0;
    msg.receiver_pid = 0;
    msg.type         = MSG_MUTEX_SYNC;
    u_memset(msg.data, 0, IPC_PAYLOAD_SIZE);

    (void)u_send_bounded(MUTEX_B_PID, &msg, 1000u);

    for (;;) {
        if (u_recv(&msg) == IPC_OK && msg.type == MSG_MUTEX_SYNC) {
            return;
        }
    }
}

void _start(void)
{
    /* A throwaway segment first, so the real one does not land at the same
     * address in both processes. Each process allocates from its own window
     * independently, so without this both would map at SHM_WINDOW_BASE and the
     * output would claim two different addresses while printing one -- which
     * is also exactly what a failure to share would look like. */
    struct sys_shm scratch;

    (void)u_shm_map(&scratch, MUTEX_B_PID);

    struct sys_shm shm;

    /* B is named as the one process allowed to attach: the id alone is not a
     * permission. */
    if (!u_shm_map(&shm, MUTEX_B_PID)) {
        u_print("  [A] could not create the shared page\n");
        goto park;
    }

    struct mutex_shared *const shared = (struct mutex_shared *)(uintptr_t)shm.vaddr;

    char *p = line;

    p = u_append(p, U_LIMIT(line), "  [A] shared page id ");
    p = u_append_dec(p, U_LIMIT(line), shm.id);
    p = u_append(p, U_LIMIT(line), " at ");
    p = u_append_hex(p, U_LIMIT(line), shm.vaddr);
    p = u_append(p, U_LIMIT(line), "; the lock lives at offset 0 inside it\n");
    *p = '\0';
    u_print(line);

    ipc_message_t offer;

    offer.sender_pid   = 0;
    offer.receiver_pid = 0;
    offer.type         = MSG_MUTEX_OFFER;
    u_memset(offer.data, 0, IPC_PAYLOAD_SIZE);
    u_store32(offer.data, shm.id);

    if (u_send_bounded(MUTEX_B_PID, &offer, 1000u) != IPC_OK) {
        u_print("  [A] could not hand the id to B\n");
        goto park;
    }

    /* ---- phase 1: no mutex ------------------------------------------------ */
    barrier();
    mutex_report("A", mutex_contend(shared, MUTEX_TEXT_A, false), false);

    /* ---- phase 2: the same code, holding the mutex ------------------------ */
    barrier();
    mutex_report("A", mutex_contend(shared, MUTEX_TEXT_A, true), true);

park:
    for (;;) {
        ipc_message_t never;
        u_recv(&never);
    }
}
