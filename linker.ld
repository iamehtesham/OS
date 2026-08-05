/* Link map for the 32-bit kernel image.
 *
 * The kernel is placed at 1 MiB because everything below that is carved up by
 * legacy PC hardware -- real-mode IVT, BIOS data area, video memory, option
 * ROMs -- and is not usable as contiguous RAM. 1 MiB is the first address a
 * Multiboot loader is guaranteed to be able to load us at. */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    /* Bracket the whole image so the physical memory manager can reserve the
     * kernel's own frames from an exact extent rather than a hardcoded size
     * that would go stale as the kernel grows. */
    _kernel_start = .;

    /* The Multiboot header must appear within the first 8 KiB of the image and
     * be 4-byte aligned, so .multiboot is emitted ahead of all other code. */
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
        *(.text.*)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
        *(.data.*)
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    /* After .bss: the reserved range must cover zero-initialised data too. */
    _kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
    }
}
