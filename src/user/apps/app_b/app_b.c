/* Application B: identical to A apart from its name, which is the point. Two
 * programs that cannot tell each other apart by behaviour, and the only thing
 * deciding which one sees a key is the focus manager. */

#include <stdint.h>

#include "ipc/input_proto.h"
#include "ipc/ipc.h"
#include "user/ulib.h"

#define NAME "App B"

static char line[64];

static void report(const char *what, char c)
{
    char *p = line;

    p = u_append(p, U_LIMIT(line), NAME);
    p = u_append(p, U_LIMIT(line), what);

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
    (void)u_trust_sender(INPUT_SERVER_PID);

    ipc_message_t msg;

    msg.sender_pid   = 0;
    msg.receiver_pid = 0;
    msg.type         = MSG_SUBSCRIBE_INPUT;
    u_memset(msg.data, 0, IPC_PAYLOAD_SIZE);

    if (u_send_bounded(INPUT_SERVER_PID, &msg, 1000u) != IPC_OK) {
        u_print(NAME " could not reach the input server\n");
    }

    for (;;) {
        if (u_recv(&msg) != IPC_OK) {
            continue;
        }

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
