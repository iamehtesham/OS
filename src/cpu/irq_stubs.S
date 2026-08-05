/* Hardware interrupt entry points for IRQ 0-15, which the PIC delivers on
 * vectors 32-47 after pic_remap.
 *
 * No IRQ ever pushes an error code, so every stub pushes a dummy zero to match
 * the frame the exception stubs build. They then join the shared tail in
 * isr_stubs.S: one copy of the save/restore sequence means one place where the
 * layout has to agree with struct registers. */

.macro IRQ index, vector
    .global irq\index
    .type irq\index, @function
irq\index:
    cli
    pushl $0
    pushl $\vector
    jmp interrupt_common_stub
    .size irq\index, . - irq\index
.endm

.section .text

IRQ 0,  32   /* programmable interval timer */
IRQ 1,  33   /* PS/2 keyboard               */
IRQ 2,  34   /* cascade from the slave PIC  */
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

.section .note.GNU-stack, "", @progbits
