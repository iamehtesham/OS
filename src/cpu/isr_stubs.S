/* CPU exception entry points for vectors 0-31.
 *
 * Some exceptions push a 32-bit error code before control transfers and some do
 * not (Intel SDM Vol. 3A, Table 6-1). The no-error-code stubs push a dummy zero
 * so every path reaches interrupt_common_stub with an identical frame, which is
 * what lets struct registers in cpu/isr.h describe all of them at once. */

.macro ISR_NOERRCODE num
    .global isr\num
    .type isr\num, @function
isr\num:
    cli
    pushl $0
    pushl $\num
    jmp interrupt_common_stub
    .size isr\num, . - isr\num
.endm

.macro ISR_ERRCODE num
    .global isr\num
    .type isr\num, @function
isr\num:
    cli
    /* the CPU has already pushed the error code */
    pushl $\num
    jmp interrupt_common_stub
    .size isr\num, . - isr\num
.endm

.section .text

ISR_NOERRCODE 0     /* #DE divide error                 */
ISR_NOERRCODE 1     /* #DB debug                        */
ISR_NOERRCODE 2     /* NMI                              */
ISR_NOERRCODE 3     /* #BP breakpoint                   */
ISR_NOERRCODE 4     /* #OF overflow                     */
ISR_NOERRCODE 5     /* #BR bound range exceeded         */
ISR_NOERRCODE 6     /* #UD invalid opcode               */
ISR_NOERRCODE 7     /* #NM device not available         */
ISR_ERRCODE   8     /* #DF double fault                 */
ISR_NOERRCODE 9     /* coprocessor segment overrun      */
ISR_ERRCODE   10    /* #TS invalid TSS                  */
ISR_ERRCODE   11    /* #NP segment not present          */
ISR_ERRCODE   12    /* #SS stack-segment fault          */
ISR_ERRCODE   13    /* #GP general protection           */
ISR_ERRCODE   14    /* #PF page fault                   */
ISR_NOERRCODE 15    /* reserved                         */
ISR_NOERRCODE 16    /* #MF x87 floating-point           */
ISR_ERRCODE   17    /* #AC alignment check              */
ISR_NOERRCODE 18    /* #MC machine check                */
ISR_NOERRCODE 19    /* #XM SIMD floating-point          */
ISR_NOERRCODE 20    /* #VE virtualization               */
ISR_ERRCODE   21    /* #CP control protection           */
ISR_NOERRCODE 22    /* reserved                         */
ISR_NOERRCODE 23    /* reserved                         */
ISR_NOERRCODE 24    /* reserved                         */
ISR_NOERRCODE 25    /* reserved                         */
ISR_NOERRCODE 26    /* reserved                         */
ISR_NOERRCODE 27    /* reserved                         */
ISR_NOERRCODE 28    /* #HV hypervisor injection         */
ISR_ERRCODE   29    /* #VC VMM communication            */
ISR_ERRCODE   30    /* #SX security exception           */
ISR_NOERRCODE 31    /* reserved                         */

/* Shared tail for every vector, exceptions and IRQs alike: completes the
 * register snapshot, switches to kernel data segments, and calls into C with a
 * pointer to the frame. irq_stubs.S jumps here too, so this sequence -- and the
 * struct registers layout it has to agree with -- exists exactly once. */
.global interrupt_common_stub
.type interrupt_common_stub, @function
interrupt_common_stub:
    /* Pushes EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI -- so EDI lands lowest,
     * which is the order struct registers declares them in. */
    pusha

    /* Zero the register first: a segment register is 16 bits, and the frame
     * field is 32, so the upper half would otherwise be stale garbage. */
    xorl %eax, %eax
    movw %ds, %ax
    pushl %eax

    /* An interrupt gate does not clear DF, so the direction flag arrives
     * inherited from whatever was interrupted, while the SysV i386 ABI requires
     * DF=0 on entry to a C function. Nothing in the kernel sets it today, but
     * that stops being true the moment ring-3 or firmware code can run. */
    cld

    /* The fault may have arrived with user segments loaded; the handler needs
     * kernel data selectors to touch kernel memory. */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    /* ESP now points at the base of the frame. push %esp pushes the value
     * before the push on a 386 and later, so C receives the right address. */
    pushl %esp
    call interrupt_dispatch
    addl $4, %esp

    popl %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    popa
    addl $8, %esp   /* discard int_no and err_code */
    iret

.size interrupt_common_stub, . - interrupt_common_stub

.section .note.GNU-stack, "", @progbits
