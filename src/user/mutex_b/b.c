/* Task B: attaches to the page A offers, then races A for it.
 *
 * Same two phases as A, driven off the same shared code, so any difference in
 * the results is a difference in synchronisation and not in what the two
 * programs do. */

#include <stdbool.h>
#include <stdint.h>

#include "ipc/mutex_proto.h"
#include "user/contend.h"
#include "user/mutex.h"
#include "user/ulib.h"

static char     line[192];
static uint32_t partner_pid;

/* The mirror of A's barrier: wait, then answer. */
static void barrier(void)
{
    ipc_message_t msg;

    for (;;) {
        if (u_recv(&msg) == IPC_OK && msg.type == MSG_MUTEX_SYNC) {
            break;
        }
    }

    msg.sender_pid   = 0;
    msg.receiver_pid = 0;
    msg.type         = MSG_MUTEX_SYNC;
    u_memset(msg.data, 0, IPC_PAYLOAD_SIZE);

    (void)u_send_bounded(partner_pid, &msg, 1000u);
}

void _start(void)
{
    ipc_message_t msg;

    u_print("  [B] waiting for A to offer a page to fight over\n");

    for (;;) {
        if (u_recv(&msg) == IPC_OK && msg.type == MSG_MUTEX_OFFER) {
            break;
        }
    }

    /* Stamped by the kernel, so it is A's real pid rather than one A claimed. */
    partner_pid = msg.sender_pid;

    const uint32_t id    = u_load32(msg.data);
    const uint32_t vaddr = u_shm_attach(id);

    if (vaddr == 0) {
        u_print("  [B] the kernel refused to attach that id\n");
        goto park;
    }

    struct mutex_shared *const shared = (struct mutex_shared *)(uintptr_t)vaddr;

    char *p = line;

    p = u_append(p, U_LIMIT(line), "  [B] attached id ");
    p = u_append_dec(p, U_LIMIT(line), id);
    p = u_append(p, U_LIMIT(line), " from pid ");
    p = u_append_dec(p, U_LIMIT(line), partner_pid);
    p = u_append(p, U_LIMIT(line), " at ");
    p = u_append_hex(p, U_LIMIT(line), vaddr);
    p = u_append(p, U_LIMIT(line), " -- same frame, different address\n");
    *p = '\0';
    u_print(line);

    barrier();
    mutex_report("B", mutex_contend(shared, MUTEX_TEXT_B, false), false);

    barrier();
    mutex_report("B", mutex_contend(shared, MUTEX_TEXT_B, true), true);

park:
    for (;;) {
        ipc_message_t never;
        u_recv(&never);
    }
}
