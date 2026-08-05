/* Kernel entry point and Multiboot 1 header.
 *
 * Assembled with GNU as via the GCC driver (.S -> cpp -> as). */

/* Multiboot Specification 0.6.96, section 3.1.2. The bootloader searches the
 * first 8 KiB of the image for MB_MAGIC on a 4-byte boundary; linker.ld emits
 * .multiboot at the very front of .text to guarantee it finds it. The three
 * fields must sum to zero modulo 2^32, which is what MB_CHECKSUM encodes. */
.set MB_ALIGN,    1 << 0                    /* load modules on 4 KiB page boundaries */
.set MB_MEMINFO,  1 << 1                    /* ask the loader for a memory map */
.set MB_FLAGS,    MB_ALIGN | MB_MEMINFO
.set MB_MAGIC,    0x1BADB002
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM


/* Multiboot hands control over with no usable stack, so the kernel supplies its
 * own. 16-byte aligned per the System V i386 ABI. */
.section .bss
.align 16
stack_bottom:
.skip 16384                                 /* 16 KiB */
stack_top:


.section .text
.global _start
.type _start, @function
_start:
    /* x86 stacks grow downwards, so ESP starts at the high end of the block. */
    mov $stack_top, %esp

    /* The ABI wants ESP 16-byte aligned at the call site; the two argument
     * pushes below eat 8 bytes, so open a matching 8-byte hole first. */
    sub $8, %esp

    /* The loader leaves the Multiboot info structure's address in EBX and the
     * magic 0x2BADB002 in EAX. Hand both to C (cdecl: rightmost arg first)
     * before any other code can clobber them. */
    push %ebx
    push %eax
    call kernel_main

    /* kernel_main is not expected to return. If it does, mask interrupts and
     * park the CPU rather than let it execute whatever follows in memory. The
     * jmp catches an NMI waking us back out of hlt. */
    cli
1:  hlt
    jmp 1b

.size _start, . - _start


/* Marks the stack as non-executable; without it ld warns that a missing
 * .note.GNU-stack implies an executable stack. */
.section .note.GNU-stack, "", @progbits
