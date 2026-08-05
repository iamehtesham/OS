/* void gdt_flush(const struct gdt_ptr *ptr);
 *
 * Installs the GDT and reloads every segment register so the new descriptors
 * actually take effect. Selector values match cpu/gdt.h: 0x08 kernel code,
 * 0x10 kernel data. */

.section .text
.global gdt_flush
.type gdt_flush, @function
gdt_flush:
    /* cdecl: the only argument sits just above the return address. */
    movl 4(%esp), %eax
    lgdt (%eax)

    /* A descriptor is only consulted when a selector is loaded, so the data
     * registers must be rewritten even though the value is unchanged. SS
     * matters as much as DS here because every push goes through it. */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* CS cannot be loaded by mov; only a far transfer replaces it. The far jump
     * also discards anything already decoded under the old code descriptor. */
    ljmp $0x08, $.Lreload_cs
.Lreload_cs:
    ret

.size gdt_flush, . - gdt_flush

/* Marks the stack non-executable; without it ld warns that a missing
 * .note.GNU-stack implies an executable stack. */
.section .note.GNU-stack, "", @progbits
