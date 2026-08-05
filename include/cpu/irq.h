#ifndef CPU_IRQ_H
#define CPU_IRQ_H

#include <stdint.h>

#include "cpu/isr.h"
#include "cpu/pic.h"

#define IRQ_COUNT       16
#define IRQ_VECTOR_BASE PIC_MASTER_VECTOR_OFFSET

#define IRQ_TIMER    0
#define IRQ_KEYBOARD 1

/* Handlers run with interrupts masked, inside the interrupt frame, and must
 * return promptly. EOI is sent centrally by irq_handler once the callback
 * returns, so a handler must not send it itself. */
typedef void (*irq_handler_t)(struct registers *regs);

void irq_install_handler(uint8_t irq, irq_handler_t handler);
void irq_uninstall_handler(uint8_t irq);

/* Entry point called from the shared interrupt stub for vectors 32-47. */
void irq_handler(struct registers *regs);

/* Per-line entry points defined in irq_stubs.S. */
extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

#endif /* CPU_IRQ_H */
