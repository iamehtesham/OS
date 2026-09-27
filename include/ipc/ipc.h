#ifndef IPC_IPC_H
#define IPC_IPC_H

#include <stdbool.h>
#include <stdint.h>

#define IPC_PAYLOAD_SIZE 32u

/* A message is a fixed-size value, never a pointer to something else. That is
 * what lets the kernel copy it between tasks without ever taking a length from
 * ring 3, and what lets a mailbox be a single embedded slot in the TCB. */
typedef struct {
    uint32_t sender_pid;   /* written by the kernel, never trusted from a sender */
    uint32_t receiver_pid;
    uint32_t type;
    uint8_t  data[IPC_PAYLOAD_SIZE];
} ipc_message_t;

_Static_assert(sizeof(ipc_message_t) == 44, "ipc_message_t layout changed");

/* The pid a message carries when the kernel itself is the sender. It is the
 * idle task's pid, and the idle task never makes a system call, so no ring-3
 * program can ever be stamped with it -- which is what lets a receiver trust
 * "this came from the kernel" by looking at one field. */
#define IPC_KERNEL_PID 0u

/* Kernel-originated message types live below 10; every user protocol starts
 * above. A hardware interrupt forwarded to a ring-3 driver arrives as this,
 * with the IRQ line number as a little-endian word in data[0..3]. */
#define MSG_HARDWARE_INTERRUPT 1u

/* A task's alarm (SYS_ALARM) has expired. The payload is all zero. */
#define MSG_TIMER 2u

/* The input protocol. Three programs speak it -- the keyboard driver, the input
 * server and every application that wants keys -- so it lives here rather than
 * in a pairwise header. Payload layouts are in ipc/input_proto.h. */
#define MSG_SUBSCRIBE_INPUT 40u /* app -> input server: route keys to me       */
#define MSG_KEYPRESS        41u /* driver -> input server -> focused app       */
#define MSG_FOCUS_SWITCH    42u /* input server -> app: you gained/lost focus  */

/* The console protocol: every character any process prints, and the input
 * server's say over which terminal is on screen. Layouts in ipc/vga_proto.h. */
#define MSG_PRINT_CHAR 50u /* anyone -> VGA server: data[0] = the character   */
#define MSG_SWITCH_VT  51u /* input server -> VGA server: data[0..3] = pid   */
#define MSG_CLEAR_VT   52u /* anyone -> VGA server: clear the sender's terminal */
#define MSG_PRINT_STR  53u /* anyone -> VGA server: data[0] = count, data[1..] = text */

/* Results returned in EAX. Negative is failure. */
#define IPC_OK           0
#define IPC_ERR_NO_TASK (-1) /* target pid missing, dead, or never receives  */
#define IPC_ERR_FULL    (-2) /* receiver's mailbox still holds an unread one */
#define IPC_ERR_FAULT   (-3) /* a user pointer was not accessible            */

/* Kernel-side entry points. Both take the user's buffer as a raw address
 * because it is untrusted: nothing dereferences it until every page it spans
 * has been checked for the access about to be made. */
int32_t ipc_send(uint32_t target_pid, uint32_t user_msg);
int32_t ipc_recv(uint32_t user_msg);

struct task;

/* Kernel-side: tells `target` that IRQ `irq` fired. Called from the interrupt
 * handler, so it never touches user memory and never blocks. Rather than
 * taking the mailbox slot, it sets a pending bit that recv turns into a
 * MSG_HARDWARE_INTERRUPT message ahead of anything in the mailbox. That is
 * what keeps a hardware event from being dropped because some other process
 * happened to fill the driver's single slot first. Wakes a blocked target.
 * Returns false if the target cannot receive. */
bool ipc_notify_irq(struct task *target, uint8_t irq);

/* Kernel-side, from the timer tick: expires every alarm whose deadline has
 * passed. Each becomes a pending flag -- like a forwarded interrupt, a flag and
 * not a message, so nothing another process sends can crowd it out -- which
 * recv turns into a MSG_TIMER, and a task blocked in recv is woken. Runs with
 * interrupts masked, touches only control blocks, never user memory. */
void ipc_expire_alarms(uint32_t now);

#endif /* IPC_IPC_H */
