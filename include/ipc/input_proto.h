#ifndef IPC_INPUT_PROTO_H
#define IPC_INPUT_PROTO_H

#include <stdint.h>

#include "ipc/ipc.h"

/* The contract between the input server, the keyboard driver that feeds it,
 * and the applications it feeds.
 *
 * The input server is a focus manager: it receives every keystroke exactly
 * once from the driver and forwards each to the one process that currently
 * has focus. Applications never see the driver and the driver never sees an
 * application; the server is the only thing that knows both. */

/* Fixed by creation order, like every well-known pid. The four core servers
 * are started before anything else -- VFS at 1, keyboard driver at 2, input
 * server at 3, console at 4 -- so an application can address the input
 * server before it has spoken to anyone. Checked at boot, not assumed. */
#define INPUT_SERVER_PID 3u

/* Every hop of a keystroke uses a reserved mailbox slot (SYS_TRUST_SENDER): the
 * input server trusts the keyboard driver, and each application trusts the
 * input server. Ordinary sends -- a subscription, or anything a hostile
 * process cares to send -- contend for the ordinary slot and cannot crowd a
 * key out. Before this, one unprivileged process looping a send at
 * INPUT_SERVER_PID cost every keystroke on the machine. */

/* MSG_SUBSCRIBE_INPUT carries no payload: the kernel-stamped sender pid IS the
 * subscription. The first subscriber is focused immediately. */

/* MSG_KEYPRESS: data[0] is the ASCII character. From the driver to the server
 * and, unchanged apart from the sender the kernel stamps, from the server to
 * the focused application. */

/* MSG_FOCUS_SWITCH: data[0] is INPUT_FOCUS_GAINED or INPUT_FOCUS_LOST. Sent to
 * the application losing focus and then to the one gaining it. */
#define INPUT_FOCUS_LOST   0u
#define INPUT_FOCUS_GAINED 1u

/* The key the server consumes instead of forwarding: it moves focus to the
 * next subscriber in subscription order and wraps. Moving focus also puts
 * that process's terminal on screen. */
#define INPUT_FOCUS_KEY '\t'

/* Escape, also consumed: shows the system console -- the terminal the kernel
 * log and every process without a terminal of its own print to -- without
 * moving focus. Keys still go to the focused process; the next Tab brings its
 * terminal back. */
#define INPUT_CONSOLE_KEY '\x1b'

/* Subscription slots. Fixed, like every other table in this system. */
#define INPUT_MAX_SUBSCRIBERS 8u

/* How many times a forward is retried while the receiver's single mailbox
 * slot is still full before the keystroke is dropped. Bounded for the same
 * reason the VFS server bounds its replies: a server must not stake its
 * liveness on a client draining its mailbox. Eight yields absorbs a slow
 * application; anything longer is an application that has stopped reading. */
#define INPUT_SEND_ATTEMPTS 8u

#endif /* IPC_INPUT_PROTO_H */
