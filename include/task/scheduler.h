#ifndef TASK_SCHEDULER_H
#define TASK_SCHEDULER_H

#include <stdint.h>

struct task;

/* Hooks the scheduler onto the PIT tick. Call after tasking_init and pit_init;
 * preemption begins with the first tick once interrupts are enabled. */
void scheduler_init(void);

/* Picks the next runnable task and switches to it. Callable from the timer
 * hook and from a system call that has just blocked the current task. Returns
 * without switching if nothing else is runnable. Must be called with interrupts
 * masked, which both of those contexts guarantee. */
void schedule(void);

/* Switches to one specific runnable task now, ahead of its turn in the ring.
 * For a task that an interrupt has just made runnable: a device driver should
 * answer its device when the event happens, not up to a full tick later. The
 * interrupted task keeps its place and resumes when round robin reaches it
 * again. A no-op if `next` is the caller or is not runnable. Same preconditions
 * as schedule(). */
void schedule_to(struct task *next);

/* Number of times the scheduler has actually moved to a different task. */
uint32_t scheduler_switches(void);

#endif /* TASK_SCHEDULER_H */
