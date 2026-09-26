#include <stddef.h>
#include <stdint.h>

#include "cpu/irq.h"
#include "cpu/isr.h"
#include "cpu/pic.h"
#include "ipc/ipc.h"
#include "task/scheduler.h"
#include "task/task.h"

static irq_handler_t irq_handlers[IRQ_COUNT];

/* Which ring-3 task, if any, serves each line. IPC_KERNEL_PID (0) means none:
 * the line is the kernel's own, as the timer is. */
static uint32_t irq_owner_pid[IRQ_COUNT];

/* True for a line claimed at run time by a ring-3 driver (irq_claim), as
 * opposed to one the kernel wired at boot with its own handler (the keyboard).
 * The two differ only when the owner dies: a boot line is drained and reopened
 * for the kernel fallback, a dynamic line is uninstalled and left masked --
 * there is no generic way to drain an arbitrary device, and reopening it with
 * nobody to service it would let it storm the CPU. */
static bool irq_dynamic[IRQ_COUNT];

/* The handler installed on a dynamically-claimed line. Every such line does
 * the same thing the keyboard's own callback does: forward the interrupt to
 * the owning driver and switch to it at once, since a device answered a tick
 * late feels broken. The line number comes from the frame, so one function
 * serves every claimed line. regs == NULL is the owner-died path, where there
 * is nothing generic to drain. */
static void irq_generic_forward(struct registers *regs)
{
    if (regs == NULL) {
        return;
    }

    const uint8_t irq = (uint8_t)(regs->int_no - IRQ_VECTOR_BASE);
    task_t *const owner = irq_forward_to_owner(irq);

    if (owner != NULL) {
        schedule_to(owner);
    }
}

void irq_install_handler(uint8_t irq, irq_handler_t handler)
{
    if (irq >= IRQ_COUNT) {
        return;
    }

    irq_handlers[irq] = handler;

    /* Everything is masked after pic_init, so the line has to be opened now
     * that something is listening on it. */
    pic_clear_mask(irq);

    /* A slave line only physically reaches the CPU through the master's
     * cascade input, so that has to be open too. */
    if (irq >= 8) {
        pic_clear_mask(PIC_CASCADE_IRQ);
    }
}

void irq_uninstall_handler(uint8_t irq)
{
    if (irq >= IRQ_COUNT) {
        return;
    }

    pic_set_mask(irq);
    irq_handlers[irq] = NULL;
}

void irq_route_to_task(uint8_t irq, uint32_t pid)
{
    if (irq < IRQ_COUNT) {
        irq_owner_pid[irq] = pid;
    }
}

bool irq_claim(uint8_t irq, uint32_t pid)
{
    /* Never the timer, the keyboard, or the cascade: the first is the kernel's
     * heartbeat, the second is already owned, and the third is how every slave
     * line (8-15) physically reaches the CPU -- letting a driver take IRQ 2
     * would cut off the very lines it might be trying to claim. None is a PCI
     * device to be reassigned. */
    if (irq >= IRQ_COUNT || irq == IRQ_TIMER || irq == IRQ_KEYBOARD || irq == PIC_CASCADE_IRQ) {
        return false;
    }

    /* A line another driver already holds, or one the kernel already has a
     * handler on, is not free to claim -- silently stealing it would break
     * whoever has it. */
    if (irq_owner_pid[irq] != IPC_KERNEL_PID || irq_handlers[irq] != NULL) {
        return false;
    }

    irq_dynamic[irq] = true;
    irq_install_handler(irq, irq_generic_forward); /* installs and opens the line */
    irq_route_to_task(irq, pid);

    return true;
}

uint32_t irq_owner(uint8_t irq)
{
    return irq < IRQ_COUNT ? irq_owner_pid[irq] : IPC_KERNEL_PID;
}

void irq_release_owner(uint32_t pid)
{
    if (pid == IPC_KERNEL_PID) {
        return; /* the kernel's own lines are never released */
    }

    for (uint8_t irq = 0; irq < IRQ_COUNT; irq++) {
        if (irq_owner_pid[irq] != pid) {
            continue;
        }

        /* Ownership first, so the handler below takes its no-owner path and
         * drains the device rather than trying to forward to a corpse. Then
         * the drain, while the line is still masked, so a fresh edge cannot
         * arrive halfway through. Then, and only then, the line reopens. */
        irq_owner_pid[irq] = IPC_KERNEL_PID;

        if (irq_dynamic[irq]) {
            /* A run-time-claimed device line: no generic drain and nobody left
             * to service it, so take the handler off and leave the line masked
             * rather than reopen it into a storm. */
            irq_uninstall_handler(irq);
            irq_dynamic[irq] = false;
            continue;
        }

        if (irq_handlers[irq] != NULL) {
            irq_handlers[irq](NULL);
        }

        pic_clear_mask(irq);
    }
}

struct task *irq_forward_to_owner(uint8_t irq)
{
    if (irq >= IRQ_COUNT || irq_owner_pid[irq] == IPC_KERNEL_PID) {
        return NULL;
    }

    /* Mask BEFORE notifying. Nothing can observe the order -- the gate cleared
     * IF, so no interrupt is delivered until the iret whichever comes first --
     * but this is the order that reads as what it means: the line is closed,
     * then the owner is told, and the owner's unmask is what reopens it. The
     * EOI already went out at the top of irq_handler, which is fine for the
     * same reason: an acknowledged PIC can raise the line again all it likes,
     * and the CPU will not look until the mask is in place. */
    pic_set_mask(irq);

    task_t *const owner = task_find(irq_owner_pid[irq]);

    if (!ipc_notify_irq(owner, irq)) {
        /* Dead or gone. Reopen the line, so the kernel-side fallback keeps
         * seeing the device, and let the caller drain it. */
        pic_clear_mask(irq);
        return NULL;
    }

    return owner;
}

void irq_handler(struct registers *regs)
{
    const uint32_t irq = regs->int_no - IRQ_VECTOR_BASE;

    if (irq >= IRQ_COUNT) {
        return;
    }

    /* Acknowledge BEFORE dispatching, and centrally rather than in each
     * callback, so an unhandled line is still acknowledged and no handler can
     * forget to.
     *
     * Before is the important word. A callback can switch stacks and never
     * return -- the scheduler does exactly that -- and a task can also park
     * itself from a system call, whose frame has no EOI in it at all. Sending
     * the EOI after the callback would make each tick's acknowledgement depend
     * on whichever task happens to resume next unwinding the right kind of
     * frame. Sending it first means every tick acknowledges itself and no such
     * bookkeeping exists. Re-entry is not a concern: the gate cleared IF, so no
     * interrupt can be delivered until the iret regardless of when the PIC was
     * told. */
    pic_send_eoi((uint8_t)irq);

    if (irq_handlers[irq] != NULL) {
        irq_handlers[irq](regs);
    }
}
