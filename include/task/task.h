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

    /* May this task ask the kernel to map physical memory at all, and if so,
     * which physical memory.
     *
     * Two fields rather than one because they answer different questions and
     * neither alone is sufficient. The flag is WHO: a bit ring 3 cannot read
     * or write, since the control block sits on a supervisor page and no
     * system call sets it. The range is WHAT: a grant recorded by the kernel
     * from the boot loader's module list when the task is created, never
     * derived from anything the caller says. A flag on its own would make a
     * server as dangerous as the kernel, because one parser bug in ring 3
     * would reach kernel text. */
    bool     may_map_physical;
    uint32_t grant_base;   /* physical, exactly as the loader reported it */
    uint32_t grant_length; /* bytes; zero means no grant                  */

    /* Next free page in this process's shared-memory window. A bump allocator:
     * it only ever moves forward, so an address handed out once is never handed
     * out again and an attach can never land on top of a mapping the process is
     * already using. The cost is that detaching reclaims no address space,
     * which is bounded by the window and is the right trade at this size. */
    uint32_t shm_next_vaddr;

    struct task *next; /* circular, so round-robin is just ->next */
} task_t;

/* Turns the currently executing kernel thread into a task so the scheduler has
 * something to switch away from and back to. Must run before any process is
 * created: it is what makes the kernel thread the idle task. */
bool tasking_init(void);

/* Turns a loaded ELF image into a running ring-3 process: allocates a kernel
 * stack, maps a ring-3 stack inside the process's OWN address space, forges
 * the entry frame and links it into the run list.
 *
 * Takes ownership of directory_phys either way -- on failure the address space
 * is destroyed, since a caller holding a half-built process has nothing useful
 * left to do with it. */
task_t *create_user_process(uint32_t entry, uint32_t directory_phys);

/* Privileges a task to map one physical range, and only that range.
 *
 * Callable only from ring 0, and only with a range the kernel itself learned
 * from the boot loader -- that is the whole security property. The range is
 * stored exactly as given; SYS_MAP_PHYSICAL rounds outward to pages when it
 * maps, so a server sees whole pages and the slack either side of a module,
 * never anything outside it. */
void task_grant_physical(task_t *task, uint32_t base, uint32_t length);

/* Reserves the next page of a task's shared-memory window and returns it, or 0
 * when the window is exhausted. */
uint32_t task_reserve_shm_vaddr(task_t *task);

/* Unlinks every dead task and releases everything it held: its address space
 * -- which decrements a reference on every frame it had mapped, freeing the
 * private ones and leaving shared ones for their other holders -- then its
 * kernel stack and its control block. Returns how many were reaped.
 *
 * MUST be called from a task that is not itself being reaped, and whose address
 * space is not one being destroyed. The idle task is the one place both are
 * guaranteed: a process cannot free the page directory it is currently
 * executing on, so the work has to happen after something else is running. */
uint32_t task_reap_dead(void);

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

/* In switch.S: where a brand-new process's forged frame begins. It loads the
 * ring-3 data selectors and irets into user mode. */
void task_bootstrap_user(void);

#endif /* TASK_TASK_H */
