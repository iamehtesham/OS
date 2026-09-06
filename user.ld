/* Link map for standalone ring-3 programs loaded from the initrd.
 *
 * These are ordinary ET_EXEC ELF files, not part of the kernel image. The
 * kernel's ELF loader reads the program headers and maps each PT_LOAD segment
 * into a page directory of the program's own, so the only thing this script
 * has to agree with is the window the loader accepts: USER_IMAGE_BASE in
 * include/mm/paging.h, which is 0x40000000. Below it lies the identity-mapped
 * kernel; above it the ring-3 stack and then the kernel heap.
 *
 * Every section is page aligned so that no two segments share a page. The
 * loader handles the shared case, but keeping segments on their own pages is
 * what lets .text stay read-only while .data is writable. */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text : ALIGN(4K)
    {
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(4K)
    {
        *(.data)
        *(.data.*)
    }

    /* Occupies no file bytes: the loader allocates frames for the difference
     * between p_memsz and p_filesz and zero-fills them. */
    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
