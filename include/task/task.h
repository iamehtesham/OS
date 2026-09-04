#ifndef TASK_TASK_H
#define TASK_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "ipc/ipc.h"

/* Per-task kernel stack. Every task's interrupt frames, its parked
 * irq_handler frame and its C call chain all live here. */
#define TASK_STACK_SIZE 8192u

/* EFLAGS a new task starts with: bit 1 is reserved and always set, bit 9 is
 * IF. Restoring this via iret is what turns interrupts back on for a task
 * entered from inside the timer handler. */
#define TASK_INITIAL_EFLAGS 0x202u

typedef enum {
    TASK_RUNNING, /* eligible to be scheduled */
    TASK_BLOCKED, /* waiting in ipc_recv for a message; skipped by the scheduler */
    TASK_DEAD,    /* returned from its entry point; never scheduled again */
} task_state_t;

typedef struct task {
    uint32_t     pid;
    task_state_t state;

    /* Saved stack pointer. Everything else -- the general-purpose registers,
     * the return address, the interrupt frame -- lives ON that stack, which is
     * why the control block needs only this one word to resume a task. */
    uint32_t esp;

    /* Physical address of the task's page directory, loaded into CR3 on a
     * switch. All tasks currently share the kernel directory, so the switch
     * skips the reload and avoids flushing the TLB for nothing. */
    uint32_t cr3;

    /* Base of the kmalloc'd stack, kept so it could be freed. Null for the
     * original kernel thread, whose stack came from boot.S. */
    void *stack_base;

    /* Top of this task's ring-0 stack, loaded into the TSS on every switch.
     * The CPU reads esp0 out of the TSS on each ring 3 -> ring 0 entry, so it
     * has to name the running task's own stack; a single fixed value would
     * make two user tasks share one kernel stack. */
    uint32_t kernel_stack_top;

    /* Single-slot mailbox. This lives in the TCB -- kernel memory on a
     * supervisor page -- so the only path in or out is a system call, and a
     * message is staged here between the sender's copy-in and the receiver's
     * copy-out rather than ever moving user-to-user in one motion. */
    ipc_message_t mailbox;
    bool          mailbox_full;

    /* False for the kernel idle task, which never calls recv. Without this a
     * send to pid 0 would be accepted, report success, and leave the message
     * parked in the kernel task's mailbox forever. */
    bool can_receive;

    struct task *next; /* circular, so round-robin is just ->next */
} task_t;

/* Turns the currently executing kernel thread into a task so the scheduler has
 * something to switch away from and back to. Must run before create_task. */
bool tasking_init(void);

/* Allocates a stack, forges an initial frame on it that switch_task can resume,
 * and appends the task to the run list. Returns null if memory ran out. */
task_t *create_task(void (*entry_point)(void));

/* Like create_task, but the forged frame drops into ring 3: the entry runs
 * with the user code selector on a freshly mapped user stack. The entry must
 * live in the .utext section so its page can be made user-accessible. */
task_t *create_user_task(void (*entry_point)(void));

task_t  *task_current(void);
task_t  *task_find(uint32_t pid);

/* The kernel thread from tasking_init. It never blocks and never dies, which
 * is what lets the scheduler treat it as the fallback when every other task
 * is waiting -- and only then: it is not a round-robin peer. */
task_t  *task_idle(void);
uint32_t task_count(void);

/* Switches to `next`, saving the outgoing task's state on its own stack. The
 * caller chooses the task; policy lives in the scheduler. Returns to the
 * caller only when something later switches back. */
void task_switch_to(task_t *next);

/* Defined in switch.S. Saves the live register state onto the current stack,
 * records ESP through save_esp, switches to new_esp, reloads CR3 when it
 * differs, and resumes whatever that stack was doing. */
void switch_task(uint32_t *save_esp, uint32_t new_esp, uint32_t new_cr3);

/* Landing pad for a task function that returns. Also in switch.S is
 * task_bootstrap, which is where a brand-new task's forged frame begins. */
void task_exit(void);
void task_bootstrap(void);
void task_bootstrap_user(void);

#endif /* TASK_TASK_H */
