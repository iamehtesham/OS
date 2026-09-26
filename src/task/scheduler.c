#include <stddef.h>
#include <stdint.h>

#include "cpu/isr.h"
#include "drivers/pit.h"
#include "mm/pmm.h"
#include "mm/shm.h"
#include "task/scheduler.h"
#include "task/task.h"
#include "utils/klog.h"

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

void schedule_to(struct task *next)
{
    task_t *const running = task_current();

    /* Not runnable covers a target that died between being named and now, and
     * a target that is the caller covers an interrupt landing on the very task
     * it is for -- that task will find the pending bit at its next recv, and
     * switching to self would corrupt the frame. */
    if (running == NULL || next == NULL || next == running || next->state != TASK_RUNNING) {
        return;
    }

    switch_count++;
    task_switch_to(next);
}

/* Round robin on every tick. Runs from the timer interrupt, so interrupts are
 * already masked by the gate and the list cannot change underneath us. The
 * PIC has already been acknowledged by irq_handler, so it does not matter that
 * the switch inside may not return for a long time. */
static void scheduler_tick(struct registers *regs)
{
    (void)regs;

    /* Reaping happens HERE, once per tick, on whichever task was interrupted.
     *
     * It lived in the idle task, and the reason was sound: a process cannot
     * free the page directory it is executing on, and the idle task is never
     * that process. But the idle task runs only when nothing else can, and
     * once SYS_SPAWN let any program create a task that never blocks -- two
     * bytes, jmp to self -- it never ran again, so no process that exited
     * afterwards was ever reclaimed and the process table filled for good
     * (found by review). The same safety argument holds here: task_reap_orphans
     * walks the ring from the interrupted task and never reaps it, so the
     * address space being torn down is never the one this handler runs on.
     * And the gate cleared IF, so the heap and the registry are not
     * mid-operation underneath it -- which is exactly why it must not ALSO run
     * from the idle loop with interrupts on: two reapers, one of them
     * interruptible by the other, would race inside kfree. */
    const uint32_t reaped = task_reap_orphans();

    if (reaped > 0) {
        const uint32_t retired = shm_collect();

        klog("Reap : collected %u orphan(s); %u shm segment(s) retired, %u frames free\n",
             reaped, retired, pmm_free_blocks());
    }

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
