#include <stddef.h>
#include <stdint.h>

#include "cpu/isr.h"
#include "drivers/pit.h"
#include "task/scheduler.h"
#include "task/task.h"

static volatile uint32_t switch_count;

/* Round robin: hand the CPU to whoever is next in the ring. Runs from the
 * timer interrupt, so interrupts are already masked by the gate and the list
 * cannot change underneath us.
 *
 * This does not return during the switch -- task_switch_to swaps stacks, so
 * control resumes inside whatever the incoming task was last doing. Everything
 * that must happen for this tick therefore has to happen before that call.
 * Acknowledging the PIC is the notable one, and it is handled by whatever we
 * switch to: a first-run task does it in task_bootstrap, and a resumed task
 * does it in the irq_handler frame parked on its stack. */
static void scheduler_tick(struct registers *regs)
{
    (void)regs;

    task_t *const running = task_current();

    if (running == NULL || running->next == running) {
        return;
    }

    switch_count++;

    task_switch_to(running->next);
}

void scheduler_init(void)
{
    pit_set_tick_handler(scheduler_tick);
}

uint32_t scheduler_switches(void)
{
    return switch_count;
}
