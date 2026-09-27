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

void ipc_expire_alarms(uint32_t now)
{
    task_t *const first = task_current();

    if (first == NULL) {
        return;
    }

    /* Every task in the ring, once. The ring cannot change underneath: this
     * runs from the timer interrupt, whose gate cleared IF, and every unlink
     * happens with interrupts off. Zombies and tasks that cannot receive are
     * skipped for the reason ipc_notify_irq skips them -- nothing would ever
     * collect the flag. */
    task_t *t = first;

    do {
        /* Signed difference, so the tick counter wrapping (after about 497
         * days at 100 Hz) does not make a deadline past the wrap look already
         * due: compared unsigned, a deadline that wrapped to a small number is
         * behind every tick before the wrap. Valid because no alarm is ever
         * armed for 2^31 ticks or more. make dhcp boots a kernel whose counter
         * starts just short of the wrap to prove it. */
        if (t->alarm_armed && t->can_receive && t->state != TASK_ZOMBIE &&
            (int32_t)(now - t->alarm_deadline) >= 0) {
            t->alarm_armed   = false;
            t->alarm_pending = true;

            if (t->state == TASK_BLOCKED) {
                t->state = TASK_RUNNING;
            }
        }

        t = t->next;
    } while (t != first);
}

/* Builds a message the kernel itself sends -- a forwarded interrupt or an
 * expired alarm. Zeroed whole first: it is built on the kernel stack and copied
 * out to ring 3, and every byte of the payload not set here would otherwise be
 * whatever the kernel stack held. The kernel is the sender, and says so with a
 * pid no user program can be stamped with. */
static void kernel_message(ipc_message_t *note, uint32_t receiver_pid, uint32_t type,
                           uint8_t low_byte)
{
    kmemset(note, 0, sizeof(*note));

    note->sender_pid   = IPC_KERNEL_PID;
    note->receiver_pid = receiver_pid;
    note->type         = type;
    note->data[0]      = low_byte; /* the low byte of a little-endian word; the rest is zero */
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
     * to this task, and only a sender, an interrupt or an alarm is a reason to
     * stop waiting. Anything else re-blocks. */
    while (!self->mailbox_full && !self->trusted_mailbox_full && self->pending_irqs == 0 &&
           !self->alarm_pending) {
        self->state = TASK_BLOCKED;
        schedule();
    }

    /* An expired alarm before anything else, interrupts included. It is
     * one-shot and costs its owner one pass of the loop, so it can never starve
     * a device; the other way round, a busy device re-raising its line would
     * postpone the alarm for as long as traffic kept arriving. This order only
     * helps a task that comes back to recv, though: one kept away from it --
     * the network server, draining a ring a flood keeps full -- has to ask for
     * itself, and does, against SYS_TICKS. */
    if (self->alarm_pending) {
        self->alarm_pending = false;

        ipc_message_t note;

        kernel_message(&note, self->pid, MSG_TIMER, 0);

        return copy_to_user(user_msg, &note, (uint32_t)sizeof(note)) ? IPC_OK : IPC_ERR_FAULT;
    }

    /* Then hardware. A driver that has both a message and an interrupt
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

        kernel_message(&note, self->pid, MSG_HARDWARE_INTERRUPT, irq);

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
