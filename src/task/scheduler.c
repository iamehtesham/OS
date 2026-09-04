#include <stddef.h>
#include <stdint.h>

#include "cpu/isr.h"
#include "drivers/pit.h"
#include "task/scheduler.h"
#include "task/task.h"

static volatile uint32_t switch_count;

/* Walks the ring from the task after `from` and returns the next task that
 * should run, or null if `from` should simply keep the CPU.
 *
 * The idle task is skipped during the walk: it is a fallback, not a peer. Let
 * it take turns like any other task and it wins a full slice in every round,
 * which with one runnable user task means the CPU halts for half of all wall
 * time, and a yield hands the CPU to `hlt` instead of to the task that was
 * just woken. It is returned only when the caller can no longer run and
 * nothing else can either -- and it always can, which is what makes blocking
 * with everything else blocked safe rather than a deadlock. */
static task_t *next_runnable(task_t *from)
{
    task_t *const idle = task_idle();

    for (task_t *task = from->next; task != from; task = task->next) {
        if (task->state == TASK_RUNNING && task != idle) {
            return task;
        }
    }

    if (from->state == TASK_RUNNING) {
        return NULL; /* still runnable and alone: keep going */
    }

    return idle;
}

void schedule(void)
{
    task_t *const running = task_current();

    if (running == NULL) {
        return;
    }

    task_t *const next = next_runnable(running);

    /* The current task is the only runnable one; it keeps the CPU. A task
     * that has just blocked never lands here, because next_runnable hands it
     * the idle task instead. */
    if (next == NULL) {
        return;
    }

    switch_count++;

    /* Does not return until something switches back here. For a task that
     * just blocked in ipc_recv, that is after a sender has filled its mailbox
     * and marked it runnable again. */
    task_switch_to(next);
}

/* Round robin on every tick. Runs from the timer interrupt, so interrupts are
 * already masked by the gate and the list cannot change underneath us. The
 * PIC has already been acknowledged by irq_handler, so it does not matter that
 * the switch inside may not return for a long time. */
static void scheduler_tick(struct registers *regs)
{
    (void)regs;
    schedule();
}

void scheduler_init(void)
{
    pit_set_tick_handler(scheduler_tick);
}

uint32_t scheduler_switches(void)
{
    return switch_count;
}
