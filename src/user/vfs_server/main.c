/* The VFS server.
 *
 * This is the whole point of the exercise: the filesystem used to be kernel
 * code, reachable by a direct call from anything running in ring 0. Now it is
 * an ordinary ELF binary in an address space of its own, and the only way to
 * reach it is to send it a message. A bug in the initrd parser can no longer
 * corrupt the kernel, because the parser cannot address the kernel.
 *
 * It owns exactly one privilege the client does not: a grant covering the
 * physical range where the boot loader left the initrd image. The kernel
 * recorded that grant when it created this process, from the multiboot module
 * list. The server cannot widen it, cannot ask for a different one, and cannot
 * discover any other physical address -- SYS_GRANT_INFO tells it only what it
 * already holds. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fs/initrd.h"
#include "fs/vfs.h"
#include "ipc/vfs_proto.h"
#include "user/ulib.h"

/* How many times a reply will be retried before it is dropped.
 *
 * Bounded deliberately. A client's mailbox holds one message, so a client that
 * stops collecting its replies leaves that slot full forever; retrying without
 * a bound would park the server inside the send and end the file service for
 * every other client, which is one unprivileged process taking down a system
 * service. A client that has not drained its previous reply is by definition
 * not waiting for this one, so dropping it costs that client alone. The few
 * retries absorb an ordinary scheduling hiccup. */
#define REPLY_ATTEMPTS 8u

static char line[192];

static void reply(uint32_t to_pid, uint32_t type, const uint8_t *payload, uint32_t length)
{
    ipc_message_t out;

    /* The kernel overwrites both pid fields, so what is put here is only ever
     * a placeholder. Filling the payload by hand rather than with an
     * initialiser keeps GCC from reaching for a memset that does not exist. */
    out.sender_pid   = 0;
    out.receiver_pid = 0;
    out.type         = type;

    u_memset(out.data, 0, IPC_PAYLOAD_SIZE);

    if (payload != NULL && length > 0) {
        u_memcpy(out.data, payload, length > IPC_PAYLOAD_SIZE ? IPC_PAYLOAD_SIZE : length);
    }

    /* The result is deliberately ignored: an undeliverable reply is the
     * client's problem, and the server's job is to get back to recv. */
    (void)u_send_bounded(to_pid, &out, REPLY_ATTEMPTS);
}

static void reply_error(uint32_t to_pid, uint32_t type, uint32_t reason)
{
    uint8_t payload[4];

    u_store32(payload, reason);
    reply(to_pid, type, payload, sizeof(payload));
}

/* A filename arrives as raw payload bytes from another process. It is only a
 * string if it carries a terminator inside the payload, so that is checked
 * before anything treats it as one. */
static bool name_is_terminated(const uint8_t *data)
{
    for (uint32_t i = 0; i < IPC_PAYLOAD_SIZE; i++) {
        if (data[i] == '\0') {
            return true;
        }
    }

    return false;
}

static void handle_open(const ipc_message_t *msg)
{
    if (fs_root == NULL) {
        reply_error(msg->sender_pid, MSG_OPEN_FAIL, VFS_ERR_NOT_MOUNTED);
        return;
    }

    if (!name_is_terminated(msg->data)) {
        reply_error(msg->sender_pid, MSG_OPEN_FAIL, VFS_ERR_BAD_NAME);
        return;
    }

    const char *const name = (const char *)msg->data;
    fs_node_t *const  node = vfs_finddir(fs_root, name);

    if (node == NULL) {
        reply_error(msg->sender_pid, MSG_OPEN_FAIL, VFS_ERR_NOT_FOUND);
        return;
    }

    vfs_open(node);

    /* The client is handed an inode, not a pointer. A pointer into this
     * server's address space would be meaningless in the client's, and taking
     * one back from a client later would be trusting it to hand back something
     * it could just as easily invent. */
    uint8_t payload[8];

    u_store32(payload, node->length);
    u_store32(payload + 4, node->inode);

    reply(msg->sender_pid, MSG_OPEN_SUCCESS, payload, sizeof(payload));
}

static void handle_read(const ipc_message_t *msg)
{
    if (fs_root == NULL) {
        reply_error(msg->sender_pid, MSG_READ_FAIL, VFS_ERR_NOT_MOUNTED);
        return;
    }

    const uint32_t   inode  = u_load32(msg->data);
    const uint32_t   offset = u_load32(msg->data + 4);
    fs_node_t *const node   = initrd_node_by_inode(inode);

    if (node == NULL) {
        reply_error(msg->sender_pid, MSG_READ_FAIL, VFS_ERR_BAD_REQUEST);
        return;
    }

    /* One byte of count, then the bytes themselves. A short read is normal --
     * it is how the client learns it has reached the end. */
    uint8_t payload[IPC_PAYLOAD_SIZE];

    u_memset(payload, 0, sizeof(payload));

    const uint32_t got = vfs_read(node, offset, VFS_MSG_READ_MAX, payload + 1);

    payload[0] = (uint8_t)got;

    reply(msg->sender_pid, MSG_READ_REPLY, payload, sizeof(payload));
}

/* Maps the initrd and mounts it. Returns false if this process holds no grant,
 * which is what a client would see if it somehow ran this code. */
static bool mount_initrd(void)
{
    struct sys_grant grant;

    if (!u_grant_info(&grant) || grant.length == 0) {
        u_print("  [vfs] no physical grant: nothing to serve\n");
        return false;
    }

    const uint32_t image = u_map_physical(grant.base, grant.length);

    if (image == 0) {
        u_print("  [vfs] the kernel refused to map the granted range\n");
        return false;
    }

    char *p = line;

    p = u_append(p, U_LIMIT(line), "  [vfs] granted phys ");
    p = u_append_hex(p, U_LIMIT(line), grant.base);
    p = u_append(p, U_LIMIT(line), " + ");
    p = u_append_dec(p, U_LIMIT(line), grant.length);
    p = u_append(p, U_LIMIT(line), " B, mapped at ");
    p = u_append_hex(p, U_LIMIT(line), image);
    p = u_append(p, U_LIMIT(line), "\n");
    *p = '\0';
    u_print(line);

    fs_root = initrd_init(image, grant.length);

    if (fs_root == NULL) {
        return false;
    }

    p = line;
    p = u_append(p, U_LIMIT(line), "  [vfs] mounted '");
    p = u_append(p, U_LIMIT(line), fs_root->name);
    p = u_append(p, U_LIMIT(line), "' with ");
    p = u_append_dec(p, U_LIMIT(line), initrd_file_count());
    p = u_append(p, U_LIMIT(line), " files\n");
    *p = '\0';
    u_print(line);

    /* One print per entry rather than one accumulated line. The driver admits
     * up to INITRD_MAX_FILES names of up to VFS_NAME_MAX bytes, which is far
     * more text than any fixed buffer here would hold -- and building it in
     * one is what previously ran off the end of this very buffer and
     * overwrote fs_root, killing the server before it ever served a request.
     * A loop that prints as it goes cannot overflow anything. */
    for (uint32_t i = 0;; i++) {
        struct dirent *const entry = vfs_readdir(fs_root, i);

        if (entry == NULL) {
            break;
        }

        p = line;
        p = u_append(p, U_LIMIT(line), "  [vfs]   ");
        p = u_append(p, U_LIMIT(line), entry->name);
        p = u_append(p, U_LIMIT(line), "\n");
        *p = '\0';
        u_print(line);
    }

    return true;
}

void _start(void)
{
    const bool mounted = mount_initrd();

    if (mounted) {
        u_print("  [vfs] serving requests\n");
    } else {
        u_print("  [vfs] degraded: every request will be refused\n");
    }

    ipc_message_t msg;

    for (;;) {
        if (u_recv(&msg) != IPC_OK) {
            /* A rejected receive means our own buffer was unreadable, which
             * cannot happen for a stack local -- but a server does not exit on
             * a surprise, it goes back to waiting. */
            continue;
        }

        switch (msg.type) {
        case MSG_OPEN:
            handle_open(&msg);
            break;

        case MSG_READ:
            handle_read(&msg);
            break;

        default:
            /* sender_pid was stamped by the kernel, so the reply cannot be
             * misdirected by a client lying about who it is. */
            reply_error(msg.sender_pid, MSG_OPEN_FAIL, VFS_ERR_BAD_REQUEST);
            break;
        }
    }
}
