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

/* Bracket the .utext and .urodata sections that linker.ld lays out on their
 * own pages, so ring-3 code can be mapped without exposing kernel bytes. */
extern uint8_t __user_text_start[];
extern uint8_t __user_text_end[];
extern uint8_t __user_rodata_start[];
extern uint8_t __user_rodata_end[];

static task_t  *current;
static task_t  *idle_task;
static uint32_t next_pid;
static uint32_t task_total;
static bool     user_pages_mapped;

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

void task_exit(void)
{
    /* A ring-0 task function returned. Mark it so the scheduler never picks it
     * again, then wait to be switched away from. There is no reaping yet: the
     * stack stays allocated, and a send to this pid is refused. */
    if (current != NULL) {
        current->state = TASK_DEAD;
    }

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

/* Maps the user code and constants sections user-accessible, once. Neither
 * gets the writable bit: ring 3 may execute and read them, nothing more. The
 * linker page-aligns both, so no kernel code or data shares the pages. */
static bool user_pages_init(void)
{
    if (user_pages_mapped) {
        return true;
    }

    const uint32_t ranges[2][2] = {
        { (uint32_t)(uintptr_t)__user_text_start, (uint32_t)(uintptr_t)__user_text_end },
        { (uint32_t)(uintptr_t)__user_rodata_start, (uint32_t)(uintptr_t)__user_rodata_end },
    };

    for (uint32_t r = 0; r < 2; r++) {
        for (uint32_t page = ranges[r][0]; page < ranges[r][1]; page += PAGE_SIZE) {
            if (!map_page(page, page, PAGE_USER)) {
                return false;
            }
        }
    }

    user_pages_mapped = true;

    return true;
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

    return task;
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

task_t *create_task(void (*entry_point)(void))
{
    if (current == NULL || entry_point == NULL) {
        return NULL;
    }

    task_t *const task = task_alloc();

    if (task == NULL) {
        return NULL;
    }

    uint8_t *const stack = kmalloc(TASK_STACK_SIZE);

    if (stack == NULL) {
        kfree(task);
        return NULL;
    }

    /* Stacks grow down, so the frame is built at the high end by
     * pre-decrementing, which makes the writes appear in reverse of the
     * layout -- exactly as real pushes would. */
    uint32_t *sp = (uint32_t *)(void *)(stack + TASK_STACK_SIZE);

    /* Where the task function's own `ret` lands if it ever returns. */
    *--sp = (uint32_t)(uintptr_t)task_exit;

    /* Three-word iret frame: same privilege level, so no ss:esp pair. */
    *--sp = TASK_INITIAL_EFLAGS;
    *--sp = GDT_KERNEL_CODE_SELECTOR;
    *--sp = (uint32_t)(uintptr_t)entry_point;

    sp = forge_switch_frame(sp, task_bootstrap);

    task->esp              = (uint32_t)(uintptr_t)sp;
    task->stack_base       = stack;
    task->kernel_stack_top = (uint32_t)(uintptr_t)(stack + TASK_STACK_SIZE);

    task_link(task);

    return task;
}

task_t *create_user_task(void (*entry_point)(void))
{
    if (current == NULL || entry_point == NULL) {
        return NULL;
    }

    if (!user_pages_init()) {
        kprintf("task: could not map the user sections\n");
        return NULL;
    }

    /* Anything outside .utext sits on a supervisor page and would fault on
     * the very first instruction fetch, so refuse it here with a message
     * rather than there with a parked task. */
    const uint32_t entry = (uint32_t)(uintptr_t)entry_point;

    if (entry < (uint32_t)(uintptr_t)__user_text_start ||
        entry >= (uint32_t)(uintptr_t)__user_text_end) {
        kprintf("task: entry %p is not in .utext\n", (void *)(uintptr_t)entry);
        return NULL;
    }

    task_t *const task = task_alloc();

    if (task == NULL) {
        return NULL;
    }

    /* Two stacks. The kernel one is where interrupts and system calls land
     * (via the TSS), the user one is what ring 3 actually runs on. */
    uint8_t *const kernel_stack = kmalloc(TASK_STACK_SIZE);

    if (kernel_stack == NULL) {
        kfree(task);
        return NULL;
    }

    void *const user_frame = pmm_alloc_block();

    if (user_frame == NULL) {
        kfree(kernel_stack);
        kfree(task);
        return NULL;
    }

    const uint32_t user_stack = (uint32_t)(uintptr_t)user_frame;

    if (!map_page(user_stack, user_stack, PAGE_USER | PAGE_WRITABLE)) {
        pmm_free_block(user_frame);
        kfree(kernel_stack);
        kfree(task);
        return NULL;
    }

    uint32_t *sp = (uint32_t *)(void *)(kernel_stack + TASK_STACK_SIZE);

    /* Five-word iret frame for a privilege change. The RPL 3 in the CS is
     * what tells iret to pop the ss:esp pair as well. No task_exit sentinel:
     * a user function that returns pops from its own stack, and the top of
     * that page is unmapped, so it faults and is parked. */
    *--sp = GDT_USER_DATA_SELECTOR_RPL3; /* ss  */
    *--sp = user_stack + PAGE_SIZE;      /* esp */
    *--sp = TASK_INITIAL_EFLAGS;         /* IF set, so the timer still preempts ring 3 */
    *--sp = GDT_USER_CODE_SELECTOR_RPL3; /* cs  */
    *--sp = entry;                       /* eip */

    sp = forge_switch_frame(sp, task_bootstrap_user);

    task->esp              = (uint32_t)(uintptr_t)sp;
    task->stack_base       = kernel_stack;
    task->kernel_stack_top = (uint32_t)(uintptr_t)(kernel_stack + TASK_STACK_SIZE);

    task_link(task);

    return task;
}

/* Allocates and maps the ring-3 stack of a process, inside its own address
 * space. Unlike the stacks create_user_task hands out -- single frames in
 * identity-mapped low memory, reachable from any task that shares the kernel
 * directory -- these pages exist in exactly one page directory. */
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

    /* Two stacks again, but this time only one of them is in the kernel's
     * address space. The kernel stack must be: the CPU switches to it via the
     * TSS on every trap, including traps taken while this process's CR3 is
     * loaded, so it has to be mapped in every directory. It is, because it
     * comes from the heap and the heap's directory entry is shared into each
     * address space. */
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

    /* The same five-word ring-3 frame create_user_task forges. What differs is
     * only where it points: an entry and a stack that exist in this process's
     * address space and nowhere else. */
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
