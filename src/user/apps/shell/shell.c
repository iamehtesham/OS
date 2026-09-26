/* The shell.
 *
 * The first program on the machine that a person talks to, and the program
 * that starts every other one. It holds no privilege: it subscribes to the
 * input server for keys like any application, it asks the VFS server for a
 * file like any client, and it asks the kernel to spawn what came back. The
 * kernel decides whether that is a program, not the shell.
 *
 * A command is either a built-in -- help, ls, clear -- or the name of a file
 * in the filesystem image. To run a file the shell opens it to learn its size
 * and inode, has the VFS server copy it into a shared segment the shell
 * created for the purpose with the VFS server as the one permitted peer, and
 * calls SYS_SPAWN with the segment id and the length. One segment serves every
 * command, so a shell that runs a thousand programs still holds one. */

#include <stdbool.h>
#include <stdint.h>

#include "ipc/input_proto.h"
#include "ipc/ipc.h"
#include "ipc/vfs_proto.h"
#include "ipc/vga_proto.h"
#include "sys/syscall_abi.h"
#include "user/ulib.h"

#define PROMPT "user@microos:~$ "

/* One screen row, less room for the prompt and the terminator. */
#define COMMAND_MAX 62u

/* How big a program the shell can load: sixteen pages, the largest segment the
 * kernel allows, and five times the size of anything built here. */
#define IMAGE_PAGES SHM_MAX_PAGES

static char     command[COMMAND_MAX + 1u];
static uint32_t command_length;

static struct sys_shm image;      /* the load segment; id 0 until created */
static bool           can_spawn;

/* Keys that arrive while the shell is waiting for the VFS server's reply are
 * held here and handled afterwards, so typing during a slow command is not
 * lost. Keys and replies come through different slots -- the input server
 * holds the shell's reserved one -- so a reply can never be stuck behind a
 * key, only interleaved with one. */
#define PENDING_KEYS 16u

static char     pending[PENDING_KEYS];
static uint32_t pending_count;

static void prompt(void)
{
    u_print(PROMPT);
}

/* ---- talking to the VFS server ---------------------------------------- */

/* Sends a request and waits for the VFS server's reply, queueing any keys
 * that arrive meanwhile. False if the request could not be sent or the
 * server did not answer in a reasonable number of turns. */
static bool request(uint32_t type, const uint8_t *payload, uint32_t length, ipc_message_t *reply)
{
    ipc_message_t msg;

    msg.sender_pid   = 0;
    msg.receiver_pid = 0;
    msg.type         = type;
    u_memset(msg.data, 0, IPC_PAYLOAD_SIZE);

    if (payload != NULL && length > 0) {
        u_memcpy(msg.data, payload, length > IPC_PAYLOAD_SIZE ? IPC_PAYLOAD_SIZE : length);
    }

    if (u_send_bounded(VFS_SERVER_PID, &msg, 1000u) != IPC_OK) {
        return false;
    }

    for (uint32_t turns = 0; turns < 1000u; turns++) {
        if (u_recv(reply) != IPC_OK) {
            continue;
        }

        if (reply->sender_pid == VFS_SERVER_PID) {
            return true;
        }

        /* A keystroke the input server routed here while we waited. Keep it
         * for after the command, in order. Anything else is noise. */
        if (reply->sender_pid == INPUT_SERVER_PID && reply->type == MSG_KEYPRESS &&
            pending_count < PENDING_KEYS) {
            pending[pending_count++] = (char)reply->data[0];
        }
    }

    return false;
}

/* ---- built-ins --------------------------------------------------------- */

static void builtin_help(void)
{
    u_print("built-in: help  ls  clear\n"
            "anything else is run as a program from the filesystem image, e.g. calc.elf\n"
            "Tab moves focus between programs that take input; Esc shows the system console\n");
}

static void builtin_ls(void)
{
    for (uint32_t index = 0;; index++) {
        uint8_t       payload[4];
        ipc_message_t reply;

        u_store32(payload, index);

        if (!request(MSG_LIST, payload, sizeof(payload), &reply) || reply.type != MSG_LIST_REPLY) {
            break;
        }

        /* The name is whatever the server sent; make sure it ends before the
         * payload does, then print it as one line. */
        reply.data[IPC_PAYLOAD_SIZE - 1u] = 0;
        u_printf("  %s\n", (const char *)reply.data);
    }
}

static void builtin_clear(void)
{
    ipc_message_t msg;

    msg.sender_pid   = 0;
    msg.receiver_pid = 0;
    msg.type         = MSG_CLEAR_VT;
    u_memset(msg.data, 0, IPC_PAYLOAD_SIZE);

    (void)u_send_bounded(VGA_SERVER_PID, &msg, 1000u);
}

/* ---- running a program ------------------------------------------------- */

static void run(const char *name)
{
    if (!can_spawn) {
        u_print("shell: no load segment; cannot start programs\n");
        return;
    }

    if (u_strlen(name) > VFS_MSG_NAME_MAX) {
        u_print("shell: name too long\n");
        return;
    }

    /* Open: learn the size and the inode. */
    uint8_t       payload[IPC_PAYLOAD_SIZE];
    ipc_message_t reply;

    u_memset(payload, 0, sizeof(payload));
    u_strncpy((char *)payload, name, sizeof(payload));

    if (!request(MSG_OPEN, payload, sizeof(payload), &reply)) {
        u_print("shell: the filesystem server did not answer\n");
        return;
    }

    if (reply.type != MSG_OPEN_SUCCESS) {
        u_printf("shell: %s: no such program\n", name);
        return;
    }

    const uint32_t length = u_load32(reply.data);
    const uint32_t inode  = u_load32(reply.data + 4);

    if (length == 0 || length > image.pages * 4096u) {
        u_printf("shell: %s: %u bytes does not fit the %u-page load segment\n", name, length,
                 image.pages);
        return;
    }

    /* Load: the server copies the file into our segment. */
    u_memset(payload, 0, sizeof(payload));
    u_store32(payload, inode);
    u_store32(payload + 4, image.id);

    if (!request(MSG_LOAD, payload, 8u, &reply) || reply.type != MSG_LOAD_REPLY) {
        u_printf("shell: %s: the filesystem server could not load it\n", name);
        return;
    }

    if (u_load32(reply.data) != length) {
        u_printf("shell: %s: short load\n", name);
        return;
    }

    /* Spawn: the kernel copies the image out of the segment, validates it and
     * starts it, or refuses. The reason for a refusal is in the kernel log,
     * on the system console. */
    const int32_t pid = u_spawn(image.id, length);

    if (pid < 0) {
        u_printf("shell: %s: the kernel refused to run it; Esc shows why\n", name);
        return;
    }

    /* Wait for it. The shell parks in the kernel until the child exits or
     * faults, so the next prompt does not appear until the command is done,
     * and the child's own output -- routed to this terminal because the child
     * is ours -- lands in between. A program that never exits blocks the shell
     * until it does: there is no backgrounding and no way to interrupt it. */
    int32_t status = 0;

    (void)u_waitpid((uint32_t)pid, &status);

    /* A clean exit (0) is the quiet, ordinary case and says nothing. A
     * non-zero status, including a fault (>= TASK_EXIT_FAULT_BASE, e.g. 270
     * for a page fault), is worth a line. */
    if (status != 0) {
        u_printf("[%s exited with status %d]\n", name, status);
    }
}

static void execute(void)
{
    /* Trim leading and trailing blanks; a bare Enter is not a command. */
    char *start = command;

    while (*start == ' ') {
        start++;
    }

    char *end = start + u_strlen(start);

    while (end > start && end[-1] == ' ') {
        *--end = '\0';
    }

    if (*start == '\0') {
        return;
    }

    if (u_strcmp(start, "help") == 0) {
        builtin_help();
    } else if (u_strcmp(start, "ls") == 0) {
        builtin_ls();
    } else if (u_strcmp(start, "clear") == 0) {
        builtin_clear();
    } else {
        run(start);
    }
}

/* ---- keys --------------------------------------------------------------- */

static void handle_key(char c)
{
    if (c == '\n') {
        u_print("\n");
        command[command_length] = '\0';
        execute();
        command_length = 0;
        prompt();
        return;
    }

    if (c == '\b') {
        /* Destructive backspace: step back, blank the cell, step back again.
         * Nothing to erase if the line is empty -- the prompt is not ours to
         * eat. */
        if (command_length > 0) {
            command_length--;
            u_print("\b \b");
        }

        return;
    }

    if (c < 0x20 || c > 0x7E) {
        return; /* not printable: not part of a command */
    }

    if (command_length < COMMAND_MAX) {
        command[command_length++] = c;

        char echo[2];

        echo[0] = c;
        echo[1] = '\0';
        u_print(echo);
    }
}

void _start(void)
{
    /* Keys come through the reserved slot, so nothing that prints at the
     * shell -- a child's output does not; replies do -- can crowd one out. */
    (void)u_trust_sender(INPUT_SERVER_PID);

    ipc_message_t msg;

    msg.sender_pid   = 0;
    msg.receiver_pid = 0;
    msg.type         = MSG_SUBSCRIBE_INPUT;
    u_memset(msg.data, 0, IPC_PAYLOAD_SIZE);

    if (u_send_bounded(INPUT_SERVER_PID, &msg, 1000u) != IPC_OK) {
        u_print("shell: could not reach the input server; no keys will arrive\n");
    }

    /* The load segment. The VFS server is the one process allowed to attach,
     * which is what lets the shell hand it the id without handing it to
     * everyone. */
    can_spawn = u_shm_map_pages(&image, VFS_SERVER_PID, IMAGE_PAGES);

    if (!can_spawn) {
        u_print("shell: could not create the load segment; programs cannot be started\n");
    }

    /* Nothing is printed until the input server says this process has focus.
     * Focus is also the screen: by the time that note is sent, the input
     * server has already told the console to give this process a terminal,
     * so what is printed after it lands there. What was printed before it --
     * the banner and the first prompt, once -- landed on the system console,
     * because the console had not yet been told who this process was (found
     * by review). Keys that arrive meanwhile are kept, in order. A dead input
     * server leaves this wait blocked, but a shell without keys has nothing
     * to say anyway. */
    for (;;) {
        if (u_recv(&msg) != IPC_OK || msg.sender_pid != INPUT_SERVER_PID) {
            continue;
        }

        if (msg.type == MSG_FOCUS_SWITCH && msg.data[0] == INPUT_FOCUS_GAINED) {
            break;
        }

        if (msg.type == MSG_KEYPRESS && pending_count < PENDING_KEYS) {
            pending[pending_count++] = (char)msg.data[0];
        }
    }

    u_print("microos shell. Type help.\n");
    prompt();

    for (;;) {
        /* Keys queued during a command come first, in order. */
        while (pending_count > 0) {
            const char c = pending[0];

            for (uint32_t i = 1; i < pending_count; i++) {
                pending[i - 1u] = pending[i];
            }

            pending_count--;
            handle_key(c);
        }

        if (u_recv(&msg) != IPC_OK) {
            continue;
        }

        /* Input is truth only from the focus manager. */
        if (msg.sender_pid != INPUT_SERVER_PID) {
            continue;
        }

        if (msg.type == MSG_KEYPRESS) {
            handle_key((char)msg.data[0]);
        }

        /* MSG_FOCUS_SWITCH: nothing to do. The prompt is already on our
         * terminal, and the console shows it when we are focused. */
    }
}
