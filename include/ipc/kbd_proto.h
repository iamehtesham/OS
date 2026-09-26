#ifndef IPC_KBD_PROTO_H
#define IPC_KBD_PROTO_H

/* The keyboard driver's identity, as far as the kernel is concerned.
 *
 * There is no client protocol here: the driver receives from the kernel alone
 * (MSG_HARDWARE_INTERRUPT, in ipc/ipc.h) and prints what it decodes. What this
 * header fixes is the pid the kernel expects the driver to land on, so the
 * boot-time check can say so when module order stops holding, and the IRQ
 * line the driver serves, spelled here so the ring-3 program does not have to
 * include the kernel's cpu/irq.h to learn it. */

/* Fixed by creation order, like every other well-known pid: the driver is the
 * second process the kernel starts, right after the VFS server, so the four
 * core servers hold pids 1-4 and every demo program comes after them. The
 * kernel checks this rather than relying on it -- interrupts are routed to the
 * task it actually created, so a shifted pid produces a warning, not a dead
 * keyboard. The input server, which is where this driver sends every key,
 * does rely on it: it accepts MSG_KEYPRESS from this pid alone. */
#define KBD_SERVER_PID 2u

/* The PS/2 keyboard's line on the master 8259. */
#define KBD_IRQ 1u

#endif /* IPC_KBD_PROTO_H */
