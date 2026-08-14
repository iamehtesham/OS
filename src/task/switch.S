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
 * create_task forges that same shape by hand, which is what lets a task that
 * has never executed be resumed by the identical code path. */
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


/* Entry point for a task being scheduled for the very first time.
 *
 * switch_task's `ret` arrives here still inside the timer interrupt: IF is
 * clear because the gate cleared it, and the PIC has not been acknowledged
 * because irq_handler only does that after its callback returns -- and that
 * callback never returned, it switched stacks. Left alone, the task would run
 * forever with interrupts disabled and the timer line still in service.
 *
 * So: acknowledge the PIC, then iret off the frame create_task forged. The iret
 * restores EIP, CS and EFLAGS together, and it is the EFLAGS that turns
 * interrupts back on -- atomically, in the same instruction that enters the
 * task. */
.global task_bootstrap
.type task_bootstrap, @function
task_bootstrap:
    pushl $0              /* IRQ 0 */
    call pic_send_eoi
    addl $4, %esp         /* balanced, so the iret frame is where we left it */
    iret

.size task_bootstrap, . - task_bootstrap

.section .note.GNU-stack, "", @progbits
