/* void paging_enable(uint32_t page_directory_physical);
 *
 * Points CR3 at the directory and sets CR0.PG. From the instruction after the
 * CR0 write onward every address the CPU touches goes through the MMU --
 * including EIP and ESP -- so the caller must already have mapped the kernel
 * text and its stack or this function never returns. */

.section .text
.global paging_enable
.type paging_enable, @function
paging_enable:
    movl 4(%esp), %eax

    /* CR3 takes the PHYSICAL address of the directory. Its low 12 bits are
     * cache-control flags rather than part of the address, which is why the
     * directory has to be page-aligned. */
    movl %eax, %cr3

    /* PG turns on translation; WP makes the R/W bit apply to ring 0 as well.
     * Without WP a supervisor write succeeds against any present page whatever
     * the R/W bits say (SDM Vol. 3A, 4.6.2), so a read-only mapping would be
     * silently writable -- and this kernel runs entirely in ring 0. */
    movl %cr0, %eax
    orl $0x80010000, %eax /* CR0.PG | CR0.WP */
    movl %eax, %cr0

    /* No pipeline flush needed: MOV to CR0 is a serializing instruction, so
     * nothing fetched under the old translation can still be in flight. */
    ret

.size paging_enable, . - paging_enable

.section .note.GNU-stack, "", @progbits
