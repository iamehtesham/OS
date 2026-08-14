#ifndef TASK_SCHEDULER_H
#define TASK_SCHEDULER_H

#include <stdint.h>

/* Hooks the scheduler onto the PIT tick. Call after tasking_init and pit_init;
 * preemption begins with the first tick once interrupts are enabled. */
void scheduler_init(void);

/* Number of times the scheduler has actually moved to a different task. */
uint32_t scheduler_switches(void);

#endif /* TASK_SCHEDULER_H */
