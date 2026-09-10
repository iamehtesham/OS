/* A client of the VFS server.
 *
 * It has no filesystem code, no grant, and no way to reach the initrd's
 * physical memory. Everything it learns about the file it gets by asking
 * another ring-3 process over IPC. That is the microkernel arrangement in one
 * program: two user processes in separate address spaces, a kernel that knows
 * nothing about files, and a message in between.
 *
 * It also does not trust the answers. The server is another unprivileged
 * process, and on this machine so is everyone else who can send a message, so
 * a reply is input rather than truth: its sender is checked, and every length
 * in it is checked against the buffer it will be copied into. */

#include <stdbool.h>
#include <stdint.h>

#include "ipc/vfs_proto.h"
#include "user/ulib.h"

#define WANTED_FILE "hello.txt"

/* Generous, but finite. The server's mailbox may be briefly full while it
 * answers someone else; it should never be full forever, and a client that
 * spins without limit is a client that can be wedged by a third party. */
#define REQUEST_ATTEMPTS 1000u

static char line[192];

/* Sends a request and waits for the server's answer.
 *
 * Anything arriving from another pid is discarded rather than mistaken for the
 * reply. The kernel stamps sender_pid, so that check cannot be defeated by a
 * process claiming to be the server. */
static bool request(uint32_t type, const uint8_t *payload, uint32_t length,
                    ipc_message_t *answer)
{
    ipc_message_t out;

    out.sender_pid   = 0; /* stamped by the kernel */
    out.receiver_pid = 0;
    out.type         = type;

    u_memset(out.data, 0, IPC_PAYLOAD_SIZE);

    if (payload != NULL && length > 0) {
        u_memcpy(out.data, payload, length > IPC_PAYLOAD_SIZE ? IPC_PAYLOAD_SIZE : length);
    }

    if (u_send_bounded(VFS_SERVER_PID, &out, REQUEST_ATTEMPTS) != IPC_OK) {
        return false;
    }

    for (;;) {
        if (u_recv(answer) != IPC_OK) {
            return false;
        }

        if (answer->sender_pid == VFS_SERVER_PID) {
            return true;
        }

        u_print("  [client] ignoring a message from someone other than the server\n");
    }
}

static void report_open_failure(const ipc_message_t *answer)
{
    const uint32_t reason = u_load32(answer->data);
    char          *p      = line;

    p = u_append(p, U_LIMIT(line), "  [client] open failed, reason ");
    p = u_append_dec(p, U_LIMIT(line), reason);
    p = u_append(p, U_LIMIT(line), reason == VFS_ERR_NOT_FOUND ? " (no such file)\n" : "\n");
    *p = '\0';
    u_print(line);
}

/* Pulls the file down in payload-sized chunks and prints it. A short chunk is
 * the end of the file, which is why the loop stops on a count of zero rather
 * than tracking the length it was told. */
static void read_and_print(uint32_t inode, uint32_t length)
{
    char *p = line;

    p = u_append(p, U_LIMIT(line), "  [client] reading ");
    p = u_append_dec(p, U_LIMIT(line), length);
    p = u_append(p, U_LIMIT(line), " bytes from inode ");
    p = u_append_dec(p, U_LIMIT(line), inode);
    p = u_append(p, U_LIMIT(line), " -----\n");
    *p = '\0';
    u_print(line);

    uint32_t offset = 0;

    for (;;) {
        uint8_t       payload[8];
        ipc_message_t answer;

        u_store32(payload, inode);
        u_store32(payload + 4, offset);

        if (!request(MSG_READ, payload, sizeof(payload), &answer)) {
            u_print("  [client] read request was not answered\n");
            return;
        }

        if (answer.type != MSG_READ_REPLY) {
            u_print("  [client] server refused the read\n");
            return;
        }

        const uint32_t count = answer.data[0];

        /* The count is one byte, so it can say up to 255 while the payload
         * holds at most VFS_MSG_READ_MAX. Refusing rather than clamping: a
         * count that large means the peer is not speaking this protocol, and
         * the rest of its message cannot be trusted either. Without this the
         * copy below overruns a 32-byte stack buffer on the say-so of whoever
         * sent the message. */
        if (count > VFS_MSG_READ_MAX) {
            u_print("  [client] malformed reply: byte count exceeds the payload\n");
            return;
        }

        if (count == 0) {
            break;
        }

        /* The chunk is bytes, not a string, so it is copied into a buffer this
         * side terminates rather than printed straight out of the payload. */
        char chunk[VFS_MSG_READ_MAX + 1u];

        u_memcpy(chunk, answer.data + 1, count);
        chunk[count] = '\0';
        u_print(chunk);

        offset += count;

        if (count < VFS_MSG_READ_MAX) {
            break;
        }
    }

    p = line;
    p = u_append(p, U_LIMIT(line), "\n  [client] ----- read ");
    p = u_append_dec(p, U_LIMIT(line), offset);
    p = u_append(p, U_LIMIT(line), " of ");
    p = u_append_dec(p, U_LIMIT(line), length);
    p = u_append(p, U_LIMIT(line), " bytes\n");
    *p = '\0';
    u_print(line);
}

/* Nothing will ever send to this pid, so this parks the process: the scheduler
 * skips it and the machine can reach hlt. A yield loop would keep the CPU busy
 * forever to accomplish nothing. */
static void park_forever(void)
{
    ipc_message_t never;

    for (;;) {
        u_recv(&never);
    }
}

void _start(void)
{
    u_print("  [client] asking the VFS server to open " WANTED_FILE "\n");

    uint8_t       name[IPC_PAYLOAD_SIZE];
    ipc_message_t answer;

    u_memset(name, 0, sizeof(name));
    u_strncpy((char *)name, WANTED_FILE, VFS_MSG_NAME_MAX + 1u);

    if (!request(MSG_OPEN, name, sizeof(name), &answer)) {
        u_print("  [client] the server did not answer\n");
        park_forever();
    }

    if (answer.type != MSG_OPEN_SUCCESS) {
        report_open_failure(&answer);
    } else {
        const uint32_t length = u_load32(answer.data);
        const uint32_t inode  = u_load32(answer.data + 4);

        char *p = line;

        p = u_append(p, U_LIMIT(line), "  [client] opened " WANTED_FILE ": ");
        p = u_append_dec(p, U_LIMIT(line), length);
        p = u_append(p, U_LIMIT(line), " bytes, inode ");
        p = u_append_dec(p, U_LIMIT(line), inode);
        p = u_append(p, U_LIMIT(line), ", served by pid ");
        p = u_append_dec(p, U_LIMIT(line), answer.sender_pid);
        p = u_append(p, U_LIMIT(line), "\n");
        *p = '\0';
        u_print(line);

        read_and_print(inode, length);
    }

    /* Ask for something that is not there, so the failure path is exercised
     * every boot rather than only when something is already wrong. */
    u_memset(name, 0, sizeof(name));
    u_strncpy((char *)name, "absent.txt", VFS_MSG_NAME_MAX + 1u);

    if (request(MSG_OPEN, name, sizeof(name), &answer) && answer.type == MSG_OPEN_FAIL) {
        report_open_failure(&answer);
    }

    /* Prove the client holds no grant: the same call the server relies on must
     * fail here, and mapping physical memory must be refused. */
    struct sys_grant grant;

    const bool     has_grant = u_grant_info(&grant);
    const uint32_t mapped    = u_map_physical(0x00100000u, 4096u);

    char *p = line;

    p = u_append(p, U_LIMIT(line), "  [client] own grant: ");
    p = u_append(p, U_LIMIT(line), has_grant ? "GRANTED (unexpected)" : "none");
    p = u_append(p, U_LIMIT(line), ", map of kernel physical 0x100000: ");
    p = u_append(p, U_LIMIT(line), mapped == 0 ? "refused" : "ALLOWED (unexpected)");
    p = u_append(p, U_LIMIT(line), "\n");
    *p = '\0';
    u_print(line);

    park_forever();
}
