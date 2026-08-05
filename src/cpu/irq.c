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

    if (irq < IRQ_COUNT && irq_handlers[irq] != NULL) {
        irq_handlers[irq](regs);
    }

    /* The EOI is sent here rather than inside each callback for two reasons:
     * an unhandled line still has to be acknowledged or the PIC never delivers
     * it again, and no individual handler can forget to do it. Sending it after
     * the callback keeps the line's in-service bit set for the duration, so the
     * same device cannot stack a second interrupt on top of the first. */
    if (irq < IRQ_COUNT) {
        pic_send_eoi((uint8_t)irq);
    }
}
