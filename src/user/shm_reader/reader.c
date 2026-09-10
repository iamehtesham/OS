/* Receives a shared-memory id over IPC, attaches it, and reads what another
 * process wrote -- then writes back into the same page to prove the sharing
 * runs both ways rather than being a copy made at attach time. */

#include <stdbool.h>
#include <stdint.h>

#include "ipc/shm_proto.h"
#include "user/ulib.h"

static char line[192];

void _start(void)
{
    ipc_message_t msg;

    u_print("  [reader] waiting for someone to offer a shared page\n");

    for (;;) {
        if (u_recv(&msg) != IPC_OK) {
            continue;
        }

        if (msg.type == MSG_SHM_OFFER) {
            break;
        }
    }

    const uint32_t id = u_load32(msg.data);

    /* The id came from another process, so it is checked by the kernel against
     * the registry rather than trusted. A number that names nothing fails. */
    const uint32_t vaddr = u_shm_attach(id);

    if (vaddr == 0) {
        u_print("  [reader] the kernel refused to attach that id\n");
        goto park;
    }

    char *const page = (char *)(uintptr_t)vaddr;
    char       *p    = line;

    p = u_append(p, U_LIMIT(line), "  [reader] pid ");
    p = u_append_dec(p, U_LIMIT(line), msg.sender_pid);
    p = u_append(p, U_LIMIT(line), " offered shm id ");
    p = u_append_dec(p, U_LIMIT(line), id);
    p = u_append(p, U_LIMIT(line), "; attached HERE at ");
    p = u_append_hex(p, U_LIMIT(line), vaddr);
    p = u_append(p, U_LIMIT(line), "\n");
    *p = '\0';
    u_print(line);

    p = line;
    p = u_append(p, U_LIMIT(line), "  [reader] read from the shared page: \"");
    p = u_append(p, U_LIMIT(line), page + SHM_WRITER_OFFSET);
    p = u_append(p, U_LIMIT(line), "\"\n");
    *p = '\0';
    u_print(line);

    if (u_strcmp(page + SHM_WRITER_OFFSET, SHM_WRITER_TEXT) != 0) {
        u_print("  [reader] FAILED: that is not what the writer wrote\n");
        goto park;
    }

    /* Write back through the same page. The mapping carries USER and RW, so
     * this is allowed; without RW it would fault here. */
    u_strncpy(page + SHM_READER_OFFSET, SHM_READER_TEXT, 48u);

    u_print("  [reader] wrote a reply into the same page, telling the writer\n");

    ipc_message_t reply;

    reply.sender_pid   = 0;
    reply.receiver_pid = 0;
    reply.type         = MSG_SHM_REPLY;

    u_memset(reply.data, 0, IPC_PAYLOAD_SIZE);

    (void)u_send_bounded(msg.sender_pid, &reply, 1000u);

park:
    for (;;) {
        ipc_message_t never;
        u_recv(&never);
    }
}
