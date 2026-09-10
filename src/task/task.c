#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu/gdt.h"
#include "cpu/tss.h"
#include "mm/kheap.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "task/task.h"
#include "utils/stdio.h"
#include "utils/string.h"

/* Emitted by boot.S. Declared as an array so the symbol's address is the
 * value, which is what a stack top is. */
extern uint8_t stack_top[];

static task_t  *current;
static task_t  *idle_task;
static uint32_t next_pid;
static uint32_t task_total;

/* The run list is walked from an interrupt, so it must never be observed
 * half-linked. These bracket the few instructions that mutate it. */
static bool interrupts_disable(void)
{
    uint32_t flags;

    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) : : "memory");

    return (flags & 0x200u) != 0;
}

static void interrupts_restore(bool were_enabled)
{
    if (were_enabled) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

/* Appends to the ring so tasks run in creation order. */
static void task_link(task_t *task)
{
    const bool were_enabled = interrupts_disable();

    task_t *tail = current;

    while (tail->next != current) {
        tail = tail->next;
    }

    task->next = current;
    tail->next = task;
    task_total++;

    interrupts_restore(were_enabled);
}

static task_t *task_alloc(void)
{
    task_t *const task = kmalloc((uint32_t)sizeof(task_t));

    if (task == NULL) {
        return NULL;
    }

    task->pid          = next_pid++;
    task->state        = TASK_RUNNING;
    task->cr3          = paging_directory_physical();
    task->mailbox_full = false;
    task->can_receive  = true;
    task->stack_base   = NULL;
    task->esp          = 0;
    task->next         = NULL;

    /* Default-deny, like the physical allocator: a task holds no privilege
     * until the kernel deliberately hands it one. */
    task->may_map_physical = false;
    task->grant_base       = 0;
    task->grant_length     = 0;

    task->shm_next_vaddr = SHM_WINDOW_BASE;

    return task;
}

uint32_t task_reserve_shm_vaddr(task_t *task)
{
    if (task == NULL || task->shm_next_vaddr >= SHM_WINDOW_LIMIT) {
        return 0;
    }

    const uint32_t reserved = task->shm_next_vaddr;

    task->shm_next_vaddr += PAGE_SIZE;

    return reserved;
}

uint32_t task_reap_dead(void)
{
    uint32_t reaped = 0;

    for (;;) {
        const bool were_enabled = interrupts_disable();

        /* Walk from the caller, so the caller itself is never a candidate --
         * which is what keeps this from freeing the stack it is standing on. */
        task_t *previous = current;
        task_t *victim   = NULL;

        while (previous->next != current) {
            if (previous->next->state == TASK_DEAD) {
                victim = previous->next;
                break;
            }

            previous = previous->next;
        }

        if (victim == NULL) {
            interrupts_restore(were_enabled);
            return reaped;
        }

        /* Off the ring before anything is freed. After this the scheduler
         * cannot reach it and ipc_send cannot find it, so the teardown below
         * races with nothing. */
        previous->next = victim->next;
        task_total--;

        interrupts_restore(were_enabled);

        /* Every present page in that address space is one reference. Dropping
         * them frees the private ones outright and leaves a shared frame for
         * whoever else still maps it -- the same walk, either way, because the
         * count is what decides. */
        if (victim->cr3 != paging_directory_physical()) {
            paging_destroy_address_space(victim->cr3);
        }

        kfree(victim->stack_base);
        kfree(victim);

        reaped++;
    }
}

void task_grant_physical(task_t *task, uint32_t base, uint32_t length)
{
    if (task == NULL || length == 0) {
        return;
    }

    task->grant_base       = base;
    task->grant_length     = length;
    task->may_map_physical = true;
}

bool tasking_init(void)
{
    task_t *const kernel_task = task_alloc();

    if (kernel_task == NULL) {
        kprintf("task: could not allocate the kernel task\n");
        return false;
    }

    /* esp stays zero deliberately: the first switch AWAY from this task fills
     * it in, and nothing reads it before then. The boot stack came from
     * boot.S, not the heap, so stack_base stays null. */
    kernel_task->kernel_stack_top = (uint32_t)(uintptr_t)stack_top;
    kernel_task->next             = kernel_task; /* a circular list of one */

    /* The idle task never calls recv, so it must not be a valid target: a
     * message sent to it would be accepted and then never collected. */
    kernel_task->can_receive = false;

    current    = kernel_task;
    idle_task  = kernel_task;
    task_total = 1;

    return true;
}

/* Pushes the eight zeros popa will consume and the bootstrap address ret will
 * jump to. Everything above sp is whatever iret frame the caller built. */
static uint32_t *forge_switch_frame(uint32_t *sp, void (*bootstrap)(void))
{
    *--sp = (uint32_t)(uintptr_t)bootstrap;

    for (uint32_t i = 0; i < 8; i++) {
        *--sp = 0;
    }

    return sp;
}

/* Allocates and maps the ring-3 stack of a process, inside its own address
 * space. Every page a process can reach exists in exactly one page directory,
 * which is what makes two processes unable to see each other. */
static bool map_process_stack(uint32_t directory_phys)
{
    for (uint32_t i = 0; i < USER_STACK_PAGES; i++) {
        void *const frame = pmm_alloc_block();

        if (frame == NULL) {
            return false;
        }

        if (!paging_frame_is_reachable((uint32_t)(uintptr_t)frame)) {
            pmm_free_block(frame);
            return false;
        }

        /* A recycled frame still holds the last owner's bytes, and this one is
         * about to become readable from ring 3. */
        kmemset(frame, 0, PAGE_SIZE);

        if (!paging_map_in(directory_phys, (uint32_t)(uintptr_t)frame,
                           USER_STACK_TOP - (i + 1u) * PAGE_SIZE,
                           PAGE_USER | PAGE_WRITABLE)) {
            pmm_free_block(frame);
            return false;
        }
    }

    return true;
}

task_t *create_user_process(uint32_t entry, uint32_t directory_phys)
{
    /* Every failure below frees the address space: the caller handed it over,
     * and a directory with no task to run it is just leaked frames. */
    if (current == NULL || entry == 0 || directory_phys == 0) {
        paging_destroy_address_space(directory_phys);
        return NULL;
    }

    task_t *const task = task_alloc();

    if (task == NULL) {
        paging_destroy_address_space(directory_phys);
        return NULL;
    }

    /* Two stacks, but only one of them is in the kernel's address space. The
     * kernel stack must be: the CPU switches to it via the TSS on every trap,
     * including traps taken while this process's CR3 is loaded, so it has to
     * be mapped in every directory. It is, because it comes from the heap and
     * the heap's directory entry is shared into each address space. */
    uint8_t *const kernel_stack = kmalloc(TASK_STACK_SIZE);

    if (kernel_stack == NULL) {
        kfree(task);
        paging_destroy_address_space(directory_phys);
        return NULL;
    }

    if (!map_process_stack(directory_phys)) {
        kfree(kernel_stack);
        kfree(task);
        paging_destroy_address_space(directory_phys);
        return NULL;
    }

    uint32_t *sp = (uint32_t *)(void *)(kernel_stack + TASK_STACK_SIZE);

    /* A five-word iret frame, because the privilege level changes: the RPL 3
     * in the CS is what tells iret to pop the ss:esp pair as well. It points
     * at an entry and a stack that exist in this process's address space and
     * nowhere else. There is no return sentinel -- a user function that
     * returns pops from its own stack, and the top of that page is unmapped,
     * so it faults and the process is marked dead. */
    *--sp = GDT_USER_DATA_SELECTOR_RPL3; /* ss  */
    *--sp = USER_STACK_TOP;              /* esp */
    *--sp = TASK_INITIAL_EFLAGS;         /* IF set, so the timer preempts it */
    *--sp = GDT_USER_CODE_SELECTOR_RPL3; /* cs  */
    *--sp = entry;                       /* eip */

    sp = forge_switch_frame(sp, task_bootstrap_user);

    task->esp              = (uint32_t)(uintptr_t)sp;
    task->stack_base       = kernel_stack;
    task->kernel_stack_top = (uint32_t)(uintptr_t)(kernel_stack + TASK_STACK_SIZE);

    /* The whole point: switch_task reloads CR3 when it differs from the
     * outgoing task's, so scheduling this task changes address space. */
    task->cr3 = directory_phys;

    task_link(task);

    return task;
}

task_t *task_current(void)
{
    return current;
}

task_t *task_idle(void)
{
    return idle_task;
}

task_t *task_find(uint32_t pid)
{
    if (current == NULL) {
        return NULL;
    }

    task_t *task = current;

    do {
        if (task->pid == pid) {
            return task;
        }

        task = task->next;
    } while (task != current);

    return NULL;
}

uint32_t task_count(void)
{
    return task_total;
}

void task_switch_to(task_t *next)
{
    if (current == NULL || next == NULL || next == current) {
        return; /* nothing to do; switching to self would corrupt the frame */
    }

    task_t *const previous = current;

    /* Published before the switch, so the incoming task sees itself as current
     * the moment it resumes. */
    current = next;

    /* Point the TSS at the incoming task's ring-0 stack. The CPU reads esp0
     * fresh on every ring 3 -> ring 0 entry, so this must be right before that
     * task can run; leaving it stale would land a user trap on some other
     * task's stack, on top of whatever that task had parked there. */
    tss_set_kernel_stack(next->kernel_stack_top);

    switch_task(&previous->esp, next->esp, next->cr3);

    /* Reached again only when something switches back to `previous`. */
}
