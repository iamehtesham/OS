/* void switch_to_user_mode(uint32_t entry, uint32_t user_stack_top);
 *
 * Drops the caller into ring 3. There is no instruction that lowers privilege
 * directly: the only way down is to return from an interrupt that came from
 * ring 3. So we forge the frame such a return would have left behind and
 * execute iret against it.
 *
 * The frame iret consumes for an inter-privilege return is five words rather
 * than three, because the CPU has to restore the user stack as well:
 *
 *     ESP -> [eip]      where ring 3 starts executing
 *            [cs]       user code selector, RPL 3
 *            [eflags]   IF set, so the timer can still preempt ring 3
 *            [esp]      user stack pointer
 *            [ss]       user data selector, RPL 3
 *
 * iret decides this is an inter-privilege return by looking at the RPL of the
 * CS it pops. With RPL 0 it would treat the frame as three words, resume in
 * ring 0, and interpret the remaining two as something else entirely. */

.section .text
.global switch_to_user_mode
.type switch_to_user_mode, @function
switch_to_user_mode:
    movl 4(%esp), %edx   /* entry           */
    movl 8(%esp), %ecx   /* user_stack_top  */

    /* iret restores CS and SS but not DS, ES, FS or GS. Worse, on a return to
     * a less privileged level the CPU NULLS any of those still holding a
     * descriptor ring 3 may not use, so leaving the kernel data selector in
     * them would fault on the first memory access. Load them now, while still
     * at CPL 0 where loading an RPL-3 selector is permitted. */
    movw $0x23, %ax      /* GDT_USER_DATA_SELECTOR_RPL3 */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    pushl $0x23          /* ss     */
    pushl %ecx           /* esp    */
    pushl $0x202         /* eflags: bit 1 reserved, bit 9 IF */
    pushl $0x1B          /* cs: GDT_USER_CODE_SELECTOR_RPL3 */
    pushl %edx           /* eip    */

    iret

.size switch_to_user_mode, . - switch_to_user_mode

.section .note.GNU-stack, "", @progbits
