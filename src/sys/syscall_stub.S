/* Entry point for int 0x80.
 *
 * Identical in shape to the exception and IRQ stubs: a software interrupt
 * pushes no error code, so a dummy zero keeps the frame the same as every
 * other vector and lets struct registers describe all of them.
 *
 * Note there is no cli here, and none is needed -- the IDT gate is an
 * interrupt gate, so the CPU has already cleared IF. It matters more than
 * usual for this vector: the caller is ring 3, and a handler that ran with
 * interrupts on could be preempted while holding the user's register frame. */

.section .text
.global isr128
.type isr128, @function
isr128:
    pushl $0    /* dummy error code */
    pushl $128  /* vector number, read back as regs->int_no */
    jmp interrupt_common_stub

.size isr128, . - isr128

.section .note.GNU-stack, "", @progbits
