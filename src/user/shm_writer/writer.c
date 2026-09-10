/* Creates a shared page, writes into it, and hands the id to another process.
 *
 * The point of the exercise is what does NOT happen: the seventeen bytes below
 * are written once, into one physical frame, and the other process reads those
 * same bytes out of the same frame. The IPC message carries four bytes naming
 * the page, not the page. */

#include <stdbool.h>
#include <stdint.h>

#include "ipc/shm_proto.h"
#include "user/ulib.h"

static char line[192];

void _start(void)
{
    /* A scratch segment first, purely so the shared one does not land at the
     * same address in both processes. Each process allocates from its own
     * window independently, so identical addresses would be a coincidence that
     * makes the demonstration look like it proves less than it does. */
    struct sys_shm scratch;

    (void)u_shm_map(&scratch, SHM_READER_PID);

    struct sys_shm shm;

    if (!u_shm_map(&shm, SHM_READER_PID)) {
        u_print("  [writer] the kernel refused to create a shared page\n");
        goto park;
    }

    char *p = line;

    p = u_append(p, U_LIMIT(line), "  [writer] created shm id ");
    p = u_append_dec(p, U_LIMIT(line), shm.id);
    p = u_append(p, U_LIMIT(line), " mapped here at ");
    p = u_append_hex(p, U_LIMIT(line), shm.vaddr);
    p = u_append(p, U_LIMIT(line), "\n");
    *p = '\0';
    u_print(line);

    /* The write that the other process will read. Nothing copies it anywhere. */
    char *const page = (char *)(uintptr_t)shm.vaddr;

    u_strncpy(page + SHM_WRITER_OFFSET, SHM_WRITER_TEXT, 48u);

    p = line;
    p = u_append(p, U_LIMIT(line), "  [writer] wrote \"");
    p = u_append(p, U_LIMIT(line), page + SHM_WRITER_OFFSET);
    p = u_append(p, U_LIMIT(line), "\" into the page, sending only the id\n");
    *p = '\0';
    u_print(line);

    ipc_message_t msg;

    msg.sender_pid   = 0; /* stamped by the kernel */
    msg.receiver_pid = 0;
    msg.type         = MSG_SHM_OFFER;

    u_memset(msg.data, 0, IPC_PAYLOAD_SIZE);
    u_store32(msg.data, shm.id);

    if (u_send_bounded(SHM_READER_PID, &msg, 1000u) != IPC_OK) {
        u_print("  [writer] could not hand the id to the reader\n");
        goto park;
    }

    /* Wait for the reader to say it has written its half. */
    for (;;) {
        if (u_recv(&msg) != IPC_OK) {
            continue;
        }

        if (msg.type == MSG_SHM_REPLY) {
            break;
        }
    }

    /* The proof that this is one page and not two copies: these bytes were
     * written by another process, into its own address space, at an address
     * that is not this one. */
    p = line;
    p = u_append(p, U_LIMIT(line), "  [writer] reading back what pid ");
    p = u_append_dec(p, U_LIMIT(line), msg.sender_pid);
    p = u_append(p, U_LIMIT(line), " wrote: \"");
    p = u_append(p, U_LIMIT(line), page + SHM_READER_OFFSET);
    p = u_append(p, U_LIMIT(line), "\"\n");
    *p = '\0';
    u_print(line);

    const bool shared = u_strcmp(page + SHM_READER_OFFSET, SHM_READER_TEXT) == 0;

    u_print(shared ? "  [writer] ZERO-COPY CONFIRMED: one frame, two address spaces\n"
                   : "  [writer] FAILED: the other process's bytes are not here\n");

park:
    for (;;) {
        ipc_message_t never;
        u_recv(&never);
    }
}
