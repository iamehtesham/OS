#include <stddef.h>
#include <stdint.h>

#include "arch/io.h"
#include "cpu/irq.h"
#include "cpu/isr.h"
#include "drivers/pit.h"

/* Command byte: channel 0, access lobyte-then-hibyte, mode 3 (square wave),
 * binary counting. Bits are 00 11 011 0. */
#define PIT_CMD_CHANNEL0_SQUAREWAVE 0x36

static volatile uint32_t tick_count;
static uint32_t          configured_frequency;
static pit_tick_t        tick_handler;

static void pit_irq(struct registers *regs)
{
    /* Bump the counter BEFORE the hook: the scheduler's hook switches stacks
     * and does not return, so anything after it would be skipped. */
    tick_count++;

    if (tick_handler != NULL) {
        tick_handler(regs);
    }
}

void pit_init(uint32_t frequency)
{
    if (frequency == 0) {
        return;
    }

    uint32_t divisor = PIT_BASE_FREQUENCY / frequency;

    /* The divisor register is 16 bits, and 0 is read as 65536. Clamp rather
     * than truncate silently. */
    if (divisor == 0) {
        divisor = 1;
    } else if (divisor > 0xFFFFu) {
        divisor = 0xFFFFu;
    }

    configured_frequency = PIT_BASE_FREQUENCY / divisor;

    outb(PIT_COMMAND, PIT_CMD_CHANNEL0_SQUAREWAVE);
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFFu));
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFFu));

    irq_install_handler(IRQ_TIMER, pit_irq);
}

void pit_set_tick_handler(pit_tick_t handler)
{
    tick_handler = handler;
}

uint32_t pit_ticks(void)
{
    return tick_count;
}

uint32_t pit_frequency(void)
{
    return configured_frequency;
}
