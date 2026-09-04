#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ipc/ipc.h"
#include "sys/uaccess.h"
#include "task/scheduler.h"
#include "task/task.h"
#include "utils/string.h"

/* Both entry points run inside the int 0x80 handler: ring 0, interrupts
 * masked by the gate, on the calling task's kernel stack. That is what makes
 * the read-modify-write on another task's mailbox safe without a lock -- no
 * other task can run between the check and the write. */

int32_t ipc_send(uint32_t target_pid, uint32_t user_msg)
{
    task_t *const sender = task_current();
    task_t *const target = task_find(target_pid);

    /* No task, a dead one, or one that will never call recv (the idle task):
     * all the same to a sender, whose message could never be collected. */
    if (sender == NULL || target == NULL || target->state == TASK_DEAD ||
        !target->can_receive) {
        return IPC_ERR_NO_TASK;
    }

    /* A single slot: refuse rather than overwrite a message the receiver has
     * not collected yet. The sender can retry after yielding. */
    if (target->mailbox_full) {
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

    kmemcpy(&target->mailbox, &staged, sizeof(target->mailbox));
    target->mailbox_full = true;

    /* Only a receiver that is actually waiting gets woken. One that has not
     * called recv yet will simply find the slot full when it does. */
    if (target->state == TASK_BLOCKED) {
        target->state = TASK_RUNNING;
    }

    return IPC_OK;
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
     * to this task, and only a sender setting mailbox_full is a reason to
     * stop waiting. Anything else re-blocks. */
    while (!self->mailbox_full) {
        self->state = TASK_BLOCKED;
        schedule();
    }

    /* Copy out into our OWN user memory. The sender never touched it; the only
     * thing that crossed between tasks was the kernel-owned mailbox. */
    const bool delivered =
        copy_to_user(user_msg, &self->mailbox, (uint32_t)sizeof(self->mailbox));

    /* Consumed either way: a failed copy-out is the receiver's fault, and
     * leaving the slot full would wedge every future sender. */
    self->mailbox_full = false;

    return delivered ? IPC_OK : IPC_ERR_FAULT;
}
