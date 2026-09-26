#ifndef TASK_TASK_H
#define TASK_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "ipc/ipc.h"

/* Per-task kernel stack. Every task's interrupt frames, its parked
 * irq_handler frame and its C call chain all live here. */
#define TASK_STACK_SIZE 8192u

/* How many ring-3 processes may exist at once, the idle task not counted. The
 * identity window is sized for this many at boot (see paging_init's reserve),
 * so it is also what keeps a process that spawns in a loop from taking every
 * frame the kernel can reach. Creation is refused past it. */
#define TASK_MAX_PROCESSES 16u

/* EFLAGS a new task starts with: bit 1 is reserved and always set, bit 9 is
 * IF. Restoring this via iret is what turns interrupts back on for a task
 * entered from inside the timer handler. */
#define TASK_INITIAL_EFLAGS 0x202u

typedef enum {
    TASK_RUNNING,       /* eligible to be scheduled */
    TASK_BLOCKED,       /* waiting in ipc_recv for a message; skipped by the scheduler */
    TASK_WAITING_CHILD, /* parked in sys_waitpid until wait_target exits; skipped too */
    TASK_ZOMBIE,        /* exited or faulted: user memory freed, corpse awaiting collection */
} task_state_t;

/* A zombie's exit_status is the value it passed to sys_exit for a clean exit,
 * which the shell keeps to 0..255 by convention; a task killed by a fault gets
 * this base ORed with the trap vector (e.g. 0x10E for a page fault), so a
 * parent can tell an orderly exit from a crash. The two ranges overlap only if
 * a program deliberately exits with a status above 255. */
#define TASK_EXIT_FAULT_BASE 0x100

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

    /* A second slot, reserved for the one sender this task has named with
     * SYS_TRUST_SENDER: the device driver a server exists to serve, or the
     * server an application takes its input from. Ordinary sends contend for
     * the slot above first come first served, and any process that knows a
     * well-known pid can keep that slot full from a tight loop -- measured, an
     * unprivileged flooder aimed at the input server cost every keystroke on
     * the machine. Nothing it does can touch this slot, and recv drains this
     * one before the ordinary one. The same idea as pending_irqs, one hop
     * further from the hardware. */
    uint32_t      trusted_sender_pid; /* 0: no reservation */
    ipc_message_t trusted_mailbox;
    bool          trusted_mailbox_full;

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

    /* May this task raise its own IOPL to 3, and so execute in/out from ring 3.
     *
     * The same shape as may_map_physical and for the same reason: a bit on a
     * supervisor page that no system call sets. It is a far broader privilege
     * than a physical grant, though. IOPL gates every one of the 65536 ports,
     * not one device, and also cli/sti -- a task holding it can stop preemption
     * and reprogram the interrupt controller or the timer. So it goes to the
     * one driver the kernel started for the purpose and to nothing else. */
    bool may_use_io;

    /* May this task map the VGA text buffer. One page, writable, for the one
     * process that is the console. Same shape as the two bits above. */
    bool may_map_vga;

    /* Hardware interrupts forwarded to this task and not yet collected, one bit
     * per line. Kept apart from the mailbox on purpose: a line is masked from
     * the moment it is forwarded until the driver re-opens it, so at most one
     * event per line can be outstanding, and a bit is exactly the right size
     * for that. It also means no other process can make the driver miss an
     * interrupt by filling its single message slot. */
    uint16_t pending_irqs;

    /* The process that created this one with SYS_SPAWN, or 0 for a process
     * the kernel started at boot. The console puts a child's output where its
     * parent's goes, and sys_waitpid uses it to enforce that only a parent may
     * reap its own child -- and the reaper uses it to spot an orphan, whose
     * parent is gone and who would otherwise never be collected. */
    uint32_t parent_pid;

    /* Set when this task exits or faults: the status a waiting parent reads. */
    int32_t exit_status;

    /* While this task is TASK_WAITING_CHILD, the pid it is waiting for. A child
     * that exits wakes its parent only if the parent's wait_target names it, so
     * a parent waiting on one child is not woken by a different one exiting. */
    uint32_t wait_target;

    /* Next free page in this process's shared-memory window. A bump allocator:
     * it only ever moves forward, so an address handed out once is never handed
     * out again and an attach can never land on top of a mapping the process is
     * already using. The cost is that detaching reclaims no address space,
     * which is bounded by the window and is the right trade at this size. */
    uint32_t shm_next_vaddr;

    /* The same, for the DMA window: where SYS_ALLOC_DMA maps the next buffer. */
    uint32_t dma_next_vaddr;

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

/* Consumes a pid without creating a task. Every attempt to start a process
 * must cost exactly one pid, success or failure: a well-known pid is a
 * compiled-in address, and if the module that should hold it fails to load,
 * the next process created would otherwise inherit that address and receive
 * everything sent to it -- silently, since it is not the process the check at
 * boot is looking for. Sends to a burned pid fail with IPC_ERR_NO_TASK. */
uint32_t task_burn_pid(void);

/* Privileges a task to map one physical range, and only that range.
 *
 * Callable only from ring 0, and only with a range the kernel itself learned
 * from the boot loader -- that is the whole security property. The range is
 * stored exactly as given; SYS_MAP_PHYSICAL rounds outward to pages when it
 * maps, so a server sees whole pages and the slack either side of a module,
 * never anything outside it. */
void task_grant_physical(task_t *task, uint32_t base, uint32_t length);

/* Privileges a task to raise its IOPL. Ring 0 only, and only for a task the
 * kernel has decided is a device driver: with IOPL=3 a process can reach every
 * I/O port on the machine, so this is not a privilege to hand out on request. */
void task_grant_io(task_t *task);

/* Privileges a task to map the VGA text buffer. Ring 0 only, for the console
 * server: whoever holds this owns the screen. */
void task_grant_vga(task_t *task);

/* Reserves the next `pages` consecutive pages of a task's shared-memory window
 * and returns the first, or 0 when the window is exhausted. */
uint32_t task_reserve_shm_vaddr(task_t *task, uint32_t pages);

/* The same for the DMA window (SYS_ALLOC_DMA). */
uint32_t task_reserve_dma_vaddr(task_t *task, uint32_t pages);

/* Loads an ELF image from kernel-readable memory and starts it as a ring-3
 * process, logging the outcome under `label`. The single path by which every
 * process comes to exist -- boot modules and SYS_SPAWN alike -- so the checks
 * happen once and identically: elf_load validates the image and builds the
 * address space, create_user_process gives it a stack and a pid. Every call
 * consumes one pid, success or failure. Returns NULL and runs nothing if any
 * check fails. */
task_t *process_spawn(const void *image, uint32_t size, const char *label);

/* Turns a task into a zombie: records its exit status, frees its user memory
 * now (paging_release_user_space -- the bulk of what it held), and wakes its
 * parent if that parent is waiting for exactly this child. Leaves the page
 * tables, directory, kernel stack and control block for collection, so a
 * parent can still read the status. Called from sys_exit and from the ring-3
 * fault path, both of which run as the task itself; freeing its user frames
 * there is safe because neither reads user memory again before yielding. */
void task_zombify(task_t *task, int32_t status);

/* Collects one zombie: unlinks it, frees its page tables and directory, its
 * kernel stack and its control block. MUST NOT be the current task, and its
 * directory must not be the active CR3 -- both hold, because a parent collects
 * a child and the reaper collects an orphan, never themselves. */
void task_collect_zombie(task_t *task);

/* Collects every ORPHAN zombie -- one whose parent is gone (dead, collected,
 * or the kernel) and so will never call sys_waitpid on it. A zombie whose
 * parent is still alive is left for that parent to reap. This is the role Unix
 * gives to init: without it an orphan's corpse would leak forever. Called from
 * the timer tick, on whichever task was interrupted, which it never collects.
 * Returns how many were collected. */
uint32_t task_reap_orphans(void);

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
