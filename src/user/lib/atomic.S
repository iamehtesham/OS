/* Atomic primitives for ring 3.
 *
 * GAS, not NASM: there is no NASM in this toolchain and every other assembly
 * file in the tree is GAS, so the operand order here is source-then-destination.
 *
 * This is the only thing in userland that has to be assembly. A compare and
 * swap is a load, a compare and a store that must be indivisible, and there is
 * no way to say that in C without a compiler builtin -- and a plain C
 * `if (x == 0) x = 1;` is four instructions with three preemption points in
 * between. */

.section .text

/* uint32_t compare_and_swap(volatile uint32_t *ptr, uint32_t expected,
 *                           uint32_t desired);
 *
 * Returns 1 if *ptr was `expected` and is now `desired`, 0 otherwise.
 *
 * cmpxchg is hardwired to compare against EAX and to load the current value
 * back into EAX when it fails, which is why `expected` goes there specifically
 * rather than into any free register.
 *
 * The LOCK prefix is what makes the read-modify-write indivisible to any other
 * core: the cache line holding *ptr is held exclusively for the duration and
 * the coherency traffic that would steal it is deferred, so no other core can
 * slip a store in between the compare and the exchange. On one core it is
 * close to redundant -- interrupts are only taken at instruction boundaries,
 * so a single instruction cannot be split by the scheduler -- but it costs one
 * byte, it is what makes this correct the moment a second core exists, and
 * writing the unlocked form would leave a trap for whoever adds SMP. */
.global compare_and_swap
.type compare_and_swap, @function
compare_and_swap:
    movl 4(%esp), %edx        /* ptr                                        */
    movl 8(%esp), %eax        /* expected -- cmpxchg compares against EAX   */
    movl 12(%esp), %ecx       /* desired                                    */

    lock cmpxchgl %ecx, (%edx)

    /* ZF is set when the compare matched and the exchange happened. EAX holds
     * the value that was there, so the flag has to be captured before EAX is
     * reused for the return value. */
    setz %al
    movzbl %al, %eax
    ret

.size compare_and_swap, . - compare_and_swap

.section .note.GNU-stack, "", @progbits
