#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu/gdt.h"
#include "cpu/tss.h"
#include "mm/kheap.h"
#include "mm/paging.h"
#include "task/task.h"
#include "utils/stdio.h"

/* Emitted by boot.S. Declared as an array so the symbol's address is the
 * value, which is what a stack top is. */
extern uint8_t stack_top[];

static task_t  *current;
static uint32_t next_id;
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

void task_exit(void)
{
    /* A task function returned. There is no reaping yet, so park it: hlt wakes
     * on the next tick, the scheduler moves on, and this task simply burns its
     * slices doing nothing rather than returning into unmapped stack bytes. */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

bool tasking_init(void)
{
    task_t *const kernel_task = kmalloc((uint32_t)sizeof(task_t));

    if (kernel_task == NULL) {
        kprintf("task: could not allocate the kernel task\n");
        return false;
    }

    kernel_task->id  = next_id++;
    kernel_task->cr3 = paging_directory_physical();

    /* Left zero deliberately: the first switch AWAY from this task is what
     * fills it in, and nothing reads it before then. */
    kernel_task->esp = 0;

    /* The boot stack came from boot.S, not the heap, so there is nothing here
     * that could ever be freed. */
    kernel_task->stack_base       = NULL;
    kernel_task->kernel_stack_top = (uint32_t)(uintptr_t)stack_top;

    kernel_task->next = kernel_task; /* a circular list of one */

    current    = kernel_task;
    task_total = 1;

    return true;
}

task_t *create_task(void (*entry_point)(void))
{
    if (current == NULL || entry_point == NULL) {
        return NULL;
    }

    task_t *const task = kmalloc((uint32_t)sizeof(task_t));

    if (task == NULL) {
        return NULL;
    }

    uint8_t *const stack = kmalloc(TASK_STACK_SIZE);

    if (stack == NULL) {
        kfree(task);
        return NULL;
    }

    /* Stacks grow down, so the frame is built at the high end and each entry
     * is written by pre-decrementing -- which means the writes below appear in
     * reverse of the layout, exactly as real pushes would. */
    uint32_t *sp = (uint32_t *)(void *)(stack + TASK_STACK_SIZE);

    /* Where the task function's own `ret` would land if it ever returned. */
    *--sp = (uint32_t)(uintptr_t)task_exit;

    /* The iret frame task_bootstrap ends on. IF is set here, which is what
     * re-enables interrupts for a task entered from inside the timer handler. */
    *--sp = TASK_INITIAL_EFLAGS;
    *--sp = GDT_KERNEL_CODE_SELECTOR;
    *--sp = (uint32_t)(uintptr_t)entry_point;

    /* switch_task's `ret` target. Not the task itself: a first-run task has to
     * acknowledge the PIC and restore IF before it can be allowed to run. */
    *--sp = (uint32_t)(uintptr_t)task_bootstrap;

    /* Eight zeros for popa. A task that has never executed has no register
     * state worth preserving; the values are consumed and discarded. */
    for (uint32_t i = 0; i < 8; i++) {
        *--sp = 0;
    }

    task->id         = next_id++;
    task->esp        = (uint32_t)(uintptr_t)sp;
    task->cr3        = paging_directory_physical();
    task->stack_base       = stack;
    task->kernel_stack_top = (uint32_t)(uintptr_t)(stack + TASK_STACK_SIZE);

    /* Append rather than insert after current, so tasks run in creation
     * order. */
    const bool were_enabled = interrupts_disable();

    task_t *tail = current;

    while (tail->next != current) {
        tail = tail->next;
    }

    task->next = current;
    tail->next = task;
    task_total++;

    interrupts_restore(were_enabled);

    return task;
}

task_t *task_current(void)
{
    return current;
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
