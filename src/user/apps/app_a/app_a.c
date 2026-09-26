/* Application A: subscribes to the input server and prints whatever it is
 * given. It never sees the keyboard driver and could not address it usefully
 * if it did -- keys reach it only through the focus manager, and only while
 * it holds focus. */

#include <stdint.h>

#include "ipc/input_proto.h"
#include "ipc/ipc.h"
#include "user/ulib.h"

#define NAME "App A"

static char line[64];

static void report(const char *what, char c)
{
    char *p = line;

    p = u_append(p, U_LIMIT(line), NAME);
    p = u_append(p, U_LIMIT(line), what);

    /* A newline would print as a blank line and a backspace would eat the
     * label, so the two control characters that can arrive are named. */
    if (c == '\n') {
        p = u_append(p, U_LIMIT(line), "<Enter>");
    } else if (c == '\b') {
        p = u_append(p, U_LIMIT(line), "<Backspace>");
    } else if (c != '\0' && p < U_LIMIT(line)) {
        *p++ = c;
    }

    p = u_append(p, U_LIMIT(line), "\n");
    *p = '\0';

    u_print(line);
}

void _start(void)
{
    /* Before subscribing, so even the first focus note lands in the slot only
     * the input server can fill. Whatever anyone else sends this process
     * contends for the ordinary slot and cannot crowd a key out. */
    (void)u_trust_sender(INPUT_SERVER_PID);

    ipc_message_t msg;

    msg.sender_pid   = 0;
    msg.receiver_pid = 0;
    msg.type         = MSG_SUBSCRIBE_INPUT;
    u_memset(msg.data, 0, IPC_PAYLOAD_SIZE);

    /* Generous, because several applications subscribe at once and the
     * server's mailbox holds one message. */
    if (u_send_bounded(INPUT_SERVER_PID, &msg, 1000u) != IPC_OK) {
        u_print(NAME " could not reach the input server\n");
    }

    for (;;) {
        if (u_recv(&msg) != IPC_OK) {
            continue;
        }

        /* Input is truth only from the focus manager. Another application
         * could send this process a MSG_KEYPRESS directly; it is ignored. */
        if (msg.sender_pid != INPUT_SERVER_PID) {
            continue;
        }

        if (msg.type == MSG_FOCUS_SWITCH) {
            u_print(msg.data[0] == INPUT_FOCUS_GAINED ? NAME " has focus\n" : NAME " lost focus\n");
        } else if (msg.type == MSG_KEYPRESS) {
            report(" received: ", (char)msg.data[0]);
        }
    }
}
