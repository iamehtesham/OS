#ifndef IPC_VGA_PROTO_H
#define IPC_VGA_PROTO_H

#include <stdint.h>

#include "ipc/ipc.h"

/* The contract between the console server and everyone who prints.
 *
 * The screen belongs to one ring-3 process. It keeps a backing buffer per
 * terminal, decides which terminal is on the hardware, and is the only thing
 * with the VGA text buffer mapped. A process does not print; it sends the
 * console server a character, and the server puts it on that process's
 * terminal -- and on the hardware, if that terminal is the one showing. */

/* Fixed by creation order: the fourth core server, after VFS, keyboard driver
 * and input server. Every program is compiled to send its output here, so it
 * has to be a constant; the kernel checks it at boot like the other three. */
#define VGA_SERVER_PID 4u

/* Terminals. Number 0 is the system console: the kernel log, and the output
 * of every process that has no terminal of its own. A process is given one of
 * the others the first time the input server puts it on screen, and keeps it.
 * When none is left, a process stays on the console. */
#define VGA_VT_COUNT   4u
#define VGA_VT_CONSOLE 0u

/* MSG_PRINT_CHAR: data[0] is the character. The kernel-stamped sender pid is
 * what selects the terminal, so a process cannot write on another's. Output to
 * the shared console is held per sender until a newline, so lines from
 * different processes do not interleave character by character; a process's
 * own terminal shows each character as it arrives. */

/* MSG_SWITCH_VT: data[0..3] is the pid whose terminal to show, little-endian;
 * VGA_VT_CONSOLE (0) shows the system console. Honoured only from the input
 * server, which is the one process entitled to say what is on screen. */

/* MSG_PRINT_STR: data[0] is a count, 1 to VGA_PRINT_CHUNK, and data[1..count]
 * the characters. The console places a whole message before it looks at the
 * next, so text inside one message never interleaves with anyone else's. That
 * matters once a terminal has two writers -- a shell and the program it
 * started -- which is what turned a one-character message from slow into
 * wrong: a status line was arriving one character at a time with a child's
 * lines landing between them. */
#define VGA_PRINT_CHUNK (IPC_PAYLOAD_SIZE - 1u)

/* MSG_CLEAR_VT: no payload. Blanks the terminal the sender's characters land
 * on and homes its cursor. The sender picks nothing; it can only clear what it
 * can already write on. */

/* A process that has no terminal of its own prints on its PARENT's, if the
 * kernel says it has one and that parent has a terminal -- so a program the
 * shell starts prints where the command was typed. The console asks the kernel
 * (SYS_PARENT_OF) once per pid and remembers the answer. */

/* How many yields a printer spends waiting for the console's mailbox before a
 * character is dropped. Large: a console that is merely busy is worth waiting
 * for, and one that is dead answers IPC_ERR_NO_TASK at once, not FULL, so
 * nobody waits on a corpse. */
#define VGA_PRINT_ATTEMPTS 100000u

#endif /* IPC_VGA_PROTO_H */
