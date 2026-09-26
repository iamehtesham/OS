#ifndef CPU_IRQ_H
#define CPU_IRQ_H

#include <stdint.h>

#include "cpu/isr.h"
#include "cpu/pic.h"

#define IRQ_COUNT       16
#define IRQ_VECTOR_BASE PIC_MASTER_VECTOR_OFFSET

#define IRQ_TIMER    0
#define IRQ_KEYBOARD 1

/* Handlers run with interrupts masked, inside the interrupt frame, and must
 * return promptly. EOI is sent centrally by irq_handler once the callback
 * returns, so a handler must not send it itself.
 *
 * `regs` is NULL when the kernel invokes a handler outside any interrupt, to
 * re-service a device whose ring-3 driver has died (irq_release_owner). Only
 * handlers for lines that can be routed to ring 3 ever see that. */
typedef void (*irq_handler_t)(struct registers *regs);

void irq_install_handler(uint8_t irq, irq_handler_t handler);
void irq_uninstall_handler(uint8_t irq);

/* Names the ring-3 task that serves a line. From then on that task is the one
 * irq_forward_to_owner notifies and the only one SYS_UNMASK_IRQ will accept
 * for the line. A pid rather than a task pointer, so a driver that dies and is
 * reaped leaves nothing dangling: task_find simply stops finding it. Pid 0 --
 * the kernel -- means the line has no ring-3 owner. */
void     irq_route_to_task(uint8_t irq, uint32_t pid);
uint32_t irq_owner(uint8_t irq);

/* Returns every line `pid` owned to the kernel, for a task that has just died.
 * Ownership is cleared, the line's kernel handler runs once so a device left
 * half-serviced is drained, and the line is reopened. Without this a driver
 * that faults while handling an interrupt dies holding a masked line with the
 * device's byte still in the controller, and the device is silent until reboot:
 * the kernel's fallback can only run for a line that is open. */
void irq_release_owner(uint32_t pid);

/* Hands a maskable line to a ring-3 driver that discovered its device at run
 * time -- a PCI NIC whose IRQ is not known until the bus is scanned. Installs a
 * generic forwarding handler, opens the line, and routes it to `pid`. Refuses
 * IRQ 0, 1 and 2 (the kernel's timer, the keyboard, and the cascade through
 * which every slave line reaches the CPU), a line another driver already owns,
 * and a line the kernel already has a handler on. Returns false on any of
 * those. The mirror of the boot-time irq_route_to_task the keyboard
 * uses, for a device the kernel could not have wired at boot. */
bool irq_claim(uint8_t irq, uint32_t pid);

/* The kernel half of a user-space driver. Masks the line at the PIC so it
 * cannot fire again until the owner asks, and marks the interrupt pending on
 * the owner, waking it if it was blocked in recv. Returns the owner so the
 * caller can switch to it, or NULL if the line has no live owner -- in which
 * case the line is left open and the caller must deal with the device itself,
 * because a byte left in a controller's buffer is what wedges a keyboard.
 *
 * Runs inside the interrupt handler: IF is clear, the EOI has already gone
 * out, and none of this touches user memory. */
struct task *irq_forward_to_owner(uint8_t irq);

/* Entry point called from the shared interrupt stub for vectors 32-47. */
void irq_handler(struct registers *regs);

/* Per-line entry points defined in irq_stubs.S. */
extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

#endif /* CPU_IRQ_H */
