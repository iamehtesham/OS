#ifndef DRIVERS_PIT_H
#define DRIVERS_PIT_H

#include <stdint.h>

#include "cpu/isr.h"

/* The 8253/8254 counts down from a divisor at a fixed input frequency; the
 * output frequency is this divided by the value we program. */
#define PIT_BASE_FREQUENCY 1193182u

/* Channel 0 is wired to IRQ0. 0x43 is the mode/command register. */
#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND       0x43

typedef void (*pit_tick_t)(struct registers *regs);

/* Programs channel 0 to fire at the requested frequency and installs the IRQ0
 * handler. Frequencies below about 19 Hz cannot be represented, since the
 * divisor is 16 bits. */
void pit_init(uint32_t frequency);

/* Registers a hook run on every tick, after the tick counter is bumped. The
 * scheduler uses this; the hook may switch stacks and never return, which is
 * why the counter is updated first. */
void pit_set_tick_handler(pit_tick_t handler);

uint32_t pit_ticks(void);
uint32_t pit_frequency(void);

#endif /* DRIVERS_PIT_H */
