/* Context switch and new-task entry.
 *
 * A task is resumed purely by restoring its stack pointer: every register, the
 * return address and any interrupt frames are already sitting on that stack.
 * The control block therefore only needs to remember ESP. */

.section .text

/* void switch_task(uint32_t *save_esp, uint32_t new_esp, uint32_t new_cr3);
 *
 * Leaves the outgoing task's stack in exactly the shape this function expects
 * to find on the incoming one:
 *
 *     ESP -> [edi][esi][ebp][esp][ebx][edx][ecx][eax]   pusha, 32 bytes
 *            [return address]                           pushed by the call
 *
 * create_user_process forges that same shape by hand, which is what lets a
 * process that has never executed be resumed by the identical code path. */
.global switch_task
.type switch_task, @function
switch_task:
    pusha

    /* cdecl arguments sit above the 32 bytes just pushed and the 4-byte return
     * address, so each is 36 bytes further up than at entry. */
    movl 36(%esp), %eax   /* save_esp */
    movl %esp, (%eax)     /* the outgoing task is now fully described by this */

    /* Read new_cr3 while the OLD stack is still addressable -- one instruction
     * later the arguments are gone. */
    movl 44(%esp), %ecx   /* new_cr3 */
    movl 40(%esp), %esp   /* new_esp; from here on we are on the new stack */

    /* Writing CR3 flushes the entire TLB, so only do it when the address space
     * actually changes. Today every task shares the kernel directory and this
     * branch is always taken. */
    movl %cr3, %edx
    cmpl %edx, %ecx
    je 1f
    movl %ecx, %cr3
1:
    popa
    ret

.size switch_task, . - switch_task


/* Entry point for a process being scheduled for the very first time.
 *
 * switch_task's `ret` arrives here still inside the timer interrupt, with IF
 * clear because the gate cleared it. The PIC has already been acknowledged --
 * irq_handler does that before dispatching -- so all that remains is to iret
 * off the frame create_user_process forged. The iret restores EIP, CS and
 * EFLAGS together, and it is the EFLAGS that turns interrupts back on,
 * atomically, in the same instruction that enters the process.
 *
 * iret restores CS and SS but not the data segment registers, and on a return
 * to a less privileged level the CPU nulls any of those still holding a
 * descriptor ring 3 may not use. They have to be loaded with the user selector
 * here, while still at CPL 0. The frame beneath is five words -- eip, cs,
 * eflags, esp, ss -- and the RPL 3 in its CS is what tells iret to pop all
 * five. */
.global task_bootstrap_user
.type task_bootstrap_user, @function
task_bootstrap_user:
    movw $0x23, %ax       /* GDT_USER_DATA_SELECTOR_RPL3 */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    iret

.size task_bootstrap_user, . - task_bootstrap_user

.section .note.GNU-stack, "", @progbits
