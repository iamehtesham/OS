#include <stddef.h>
#include <stdint.h>

#include "cpu/irq.h"
#include "cpu/isr.h"
#include "cpu/pic.h"

static irq_handler_t irq_handlers[IRQ_COUNT];

void irq_install_handler(uint8_t irq, irq_handler_t handler)
{
    if (irq >= IRQ_COUNT) {
        return;
    }

    irq_handlers[irq] = handler;

    /* Everything is masked after pic_init, so the line has to be opened now
     * that something is listening on it. */
    pic_clear_mask(irq);

    /* A slave line only physically reaches the CPU through the master's
     * cascade input, so that has to be open too. */
    if (irq >= 8) {
        pic_clear_mask(PIC_CASCADE_IRQ);
    }
}

void irq_uninstall_handler(uint8_t irq)
{
    if (irq >= IRQ_COUNT) {
        return;
    }

    pic_set_mask(irq);
    irq_handlers[irq] = NULL;
}

void irq_handler(struct registers *regs)
{
    const uint32_t irq = regs->int_no - IRQ_VECTOR_BASE;

    if (irq >= IRQ_COUNT) {
        return;
    }

    /* Acknowledge BEFORE dispatching, and centrally rather than in each
     * callback, so an unhandled line is still acknowledged and no handler can
     * forget to.
     *
     * Before is the important word. A callback can switch stacks and never
     * return -- the scheduler does exactly that -- and a task can also park
     * itself from a system call, whose frame has no EOI in it at all. Sending
     * the EOI after the callback would make each tick's acknowledgement depend
     * on whichever task happens to resume next unwinding the right kind of
     * frame. Sending it first means every tick acknowledges itself and no such
     * bookkeeping exists. Re-entry is not a concern: the gate cleared IF, so no
     * interrupt can be delivered until the iret regardless of when the PIC was
     * told. */
    pic_send_eoi((uint8_t)irq);

    if (irq_handlers[irq] != NULL) {
        irq_handlers[irq](regs);
    }
}
