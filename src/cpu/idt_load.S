/* void idt_load(const struct idt_ptr *ptr);
 *
 * Points the CPU's IDTR at our table. Unlike the GDT there is no segment
 * register to reload afterwards -- the next interrupt simply uses the new
 * table. */

.section .text
.global idt_load
.type idt_load, @function
idt_load:
    movl 4(%esp), %eax
    lidt (%eax)
    ret

.size idt_load, . - idt_load

.section .note.GNU-stack, "", @progbits
