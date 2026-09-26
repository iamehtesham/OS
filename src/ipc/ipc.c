#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ipc/ipc.h"
#include "sys/uaccess.h"
#include "task/scheduler.h"
#include "task/task.h"
#include "utils/string.h"

/* Every entry point here runs with interrupts masked. send and recv are inside
 * the int 0x80 handler, whose gate clears IF; ipc_notify_irq is inside a
 * hardware interrupt handler, whose gate does the same. That is what makes the
 * read-modify-write on another task's mailbox and pending bits safe without a
 * lock -- no two of these can ever interleave, and no task can run between a
 * check and the write that follows it. */

/* Picks the slot a message from `sender_pid` lands in at `target`.
 *
 * A task may name one sender it trusts -- the driver it exists to serve, or
 * the server it takes its input from -- and that sender's messages go to a
 * reserved slot no other process can fill. Everyone else shares the ordinary
 * slot, first come first served. Without the reservation, any process that
 * knows a server's well-known pid can keep the ordinary slot full from a
 * tight loop and starve the one sender that matters: measured, an unprivileged
 * flooder aimed at the input server cost every keystroke on the machine. The
 * kernel's own pid is excluded because the kernel never sends this way; its
 * interrupt notifications are a bit, not a slot. */
static void select_slot(task_t *target, uint32_t sender_pid, ipc_message_t **slot, bool **full)
{
    if (sender_pid != IPC_KERNEL_PID && sender_pid == target->trusted_sender_pid) {
        *slot = &target->trusted_mailbox;
        *full = &target->trusted_mailbox_full;
    } else {
        *slot = &target->mailbox;
        *full = &target->mailbox_full;
    }
}

int32_t ipc_send(uint32_t target_pid, uint32_t user_msg)
{
    task_t *const sender = task_current();
    task_t *const target = task_find(target_pid);

    /* No task, a dead one, or one that will never call recv (the idle task):
     * all the same to a sender, whose message could never be collected. */
    if (sender == NULL || target == NULL || target->state == TASK_ZOMBIE ||
        !target->can_receive) {
        return IPC_ERR_NO_TASK;
    }

    ipc_message_t *slot;
    bool          *full;

    select_slot(target, sender->pid, &slot, &full);

    /* A single slot: refuse rather than overwrite a message the receiver has
     * not collected yet. The sender can retry after yielding. */
    if (*full) {
        return IPC_ERR_FULL;
    }

    /* Stage through the kernel stack first. copy_from_user validates every
     * page before it moves a byte, so a bad pointer fails here with the
     * target's mailbox untouched -- never half-written. */
    ipc_message_t staged;

    if (!copy_from_user(&staged, user_msg, (uint32_t)sizeof(staged))) {
        return IPC_ERR_FAULT;
    }

    /* Identity is the kernel's to assert. Whatever ring 3 wrote in these two
     * fields is overwritten, so a sender cannot impersonate another pid. */
    staged.sender_pid   = sender->pid;
    staged.receiver_pid = target->pid;

    kmemcpy(slot, &staged, sizeof(*slot));
    *full = true;

    /* Only a receiver that is actually waiting gets woken. One that has not
     * called recv yet will simply find the slot full when it does. */
    if (target->state == TASK_BLOCKED) {
        target->state = TASK_RUNNING;
    }

    return IPC_OK;
}

bool ipc_notify_irq(struct task *target, uint8_t irq)
{
    if (target == NULL || target->state == TASK_ZOMBIE || !target->can_receive ||
        irq >= 8u * sizeof(target->pending_irqs)) {
        return false;
    }

    /* A bit, not a slot. The line is masked until the driver collects this, so
     * a second event on the same line cannot arrive before then, and a bit
     * already set means exactly what a set bit should. */
    target->pending_irqs |= (uint16_t)(1u << irq);

    if (target->state == TASK_BLOCKED) {
        target->state = TASK_RUNNING;
    }

    return true;
}

/* Builds the message a pending interrupt is delivered as. The kernel is the
 * sender, and says so with a pid no user program can be stamped with. */
static void irq_message(ipc_message_t *note, uint32_t receiver_pid, uint8_t irq)
{
    kmemset(note, 0, sizeof(*note));

    note->sender_pid   = IPC_KERNEL_PID;
    note->receiver_pid = receiver_pid;
    note->type         = MSG_HARDWARE_INTERRUPT;
    note->data[0]      = irq; /* the low byte of a little-endian word; the rest is zero */
}

int32_t ipc_recv(uint32_t user_msg)
{
    task_t *const self = task_current();

    if (self == NULL) {
        return IPC_ERR_NO_TASK;
    }

    /* Vet the destination before blocking, so a bad pointer fails now rather
     * than after an indefinite wait. It is checked again at delivery, because
     * that is when the write actually happens. */
    if (!user_range_writable(user_msg, (uint32_t)sizeof(ipc_message_t))) {
        return IPC_ERR_FAULT;
    }

    /* A loop, not an if: schedule() returns whenever something switches back
     * to this task, and only a sender or an interrupt is a reason to stop
     * waiting. Anything else re-blocks. */
    while (!self->mailbox_full && !self->trusted_mailbox_full && self->pending_irqs == 0) {
        self->state = TASK_BLOCKED;
        schedule();
    }

    /* Hardware first. A driver that has both a message and an interrupt
     * waiting has a device sitting on a masked line, and the message can wait
     * a round trip; the device cannot be made to. Lowest line first, which is
     * also the 8259's own priority order. */
    if (self->pending_irqs != 0) {
        uint8_t irq = 0;

        while ((self->pending_irqs & (1u << irq)) == 0) {
            irq++;
        }

        /* Consumed before the copy, the same way the mailbox is below: a
         * failed copy-out is the receiver's fault, and re-arming the bit for
         * a buffer that cannot be written would just spin the same failure. */
        self->pending_irqs &= (uint16_t)~(1u << irq);

        ipc_message_t note;

        irq_message(&note, self->pid, irq);

        return copy_to_user(user_msg, &note, (uint32_t)sizeof(note)) ? IPC_OK : IPC_ERR_FAULT;
    }

    /* Then the trusted sender, before the crowd. */
    ipc_message_t *const slot = self->trusted_mailbox_full ? &self->trusted_mailbox : &self->mailbox;
    bool *const          full = self->trusted_mailbox_full ? &self->trusted_mailbox_full
                                                           : &self->mailbox_full;

    /* Copy out into our OWN user memory. The sender never touched it; the only
     * thing that crossed between tasks was the kernel-owned slot. */
    const bool delivered = copy_to_user(user_msg, slot, (uint32_t)sizeof(*slot));

    /* Consumed either way: a failed copy-out is the receiver's fault, and
     * leaving the slot full would wedge every future sender. */
    *full = false;

    return delivered ? IPC_OK : IPC_ERR_FAULT;
}
