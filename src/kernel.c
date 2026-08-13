#include <stdbool.h>
#include <stdint.h>

#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/irq.h"
#include "cpu/pic.h"
#include "drivers/keyboard.h"
#include "drivers/vga.h"
#include "mm/kheap.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "multiboot.h"
#include "utils/stdio.h"
#include "utils/string.h"

/* Emitted by linker.ld to bracket the loaded image, .bss included. Declared as
 * arrays so the symbol's address is the value, without a spurious load. */
extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];

static uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

/* Entries are variable length: size counts the bytes after itself, so the step
 * is size + 4 rather than sizeof the struct. */
static const struct multiboot_mmap_entry *mmap_next(const struct multiboot_mmap_entry *entry)
{
    return (const struct multiboot_mmap_entry *)((uintptr_t)entry + entry->size +
                                                 sizeof(entry->size));
}

/* Narrows a 64-bit map entry to the 32-bit range this kernel can address.
 * Returns false for regions lying entirely above the reach of a 32-bit
 * physical address. */
static bool mmap_range32(const struct multiboot_mmap_entry *entry,
                         uint32_t *base, uint32_t *length)
{
    const uint64_t limit = 0xFFFFF000ull; /* last whole frame below 4 GiB */

    if (entry->addr >= limit) {
        return false;
    }

    uint64_t end = entry->addr + entry->len;

    if (end > limit) {
        end = limit;
    }

    *base   = (uint32_t)entry->addr;
    *length = (uint32_t)(end - entry->addr);

    return *length != 0;
}

/* Applied to every range the boot loader owns. pmm_reserve_region has exactly
 * this shape, so the same walk both places the bitmap and protects the ranges. */
typedef void (*range_fn)(uint32_t base, uint32_t length);

static uint32_t loader_top;

static void track_loader_top(uint32_t base, uint32_t length)
{
    const uint32_t end = base + length;

    if (length != 0 && end > base && end > loader_top) {
        loader_top = end;
    }
}

/* Visits everything the loader placed in memory and still owns: the info
 * structure, the memory map, the strings, the module descriptor array, and each
 * module payload. GRUB and QEMU both park these immediately above the kernel
 * image -- the module descriptors land exactly on the first page after
 * _kernel_end -- so the bitmap has to be positioned above all of it, and the
 * ranges reserved so the allocator never hands a module out. */
static void multiboot_for_each_owned_range(const struct multiboot_info *mbi, range_fn fn)
{
    fn((uint32_t)(uintptr_t)mbi, (uint32_t)sizeof(*mbi));

    if (mbi->flags & MULTIBOOT_INFO_MMAP) {
        fn(mbi->mmap_addr, mbi->mmap_length);
    }

    if (mbi->flags & MULTIBOOT_INFO_CMDLINE) {
        fn(mbi->cmdline, (uint32_t)kstrlen((const char *)(uintptr_t)mbi->cmdline) + 1u);
    }

    if (mbi->flags & MULTIBOOT_INFO_BOOT_LOADER_NAME) {
        fn(mbi->boot_loader_name,
           (uint32_t)kstrlen((const char *)(uintptr_t)mbi->boot_loader_name) + 1u);
    }

    if (mbi->flags & MULTIBOOT_INFO_MODS) {
        const struct multiboot_mod_list *mods =
            (const struct multiboot_mod_list *)(uintptr_t)mbi->mods_addr;

        fn(mbi->mods_addr, mbi->mods_count * (uint32_t)sizeof(*mods));

        for (uint32_t i = 0; i < mbi->mods_count; i++) {
            fn(mods[i].mod_start, mods[i].mod_end - mods[i].mod_start);
        }
    }
}

/* Brings up the physical allocator and returns the first address above
 * everything the kernel has reserved for itself, or 0 on failure. */
static uint32_t memory_init(const struct multiboot_info *mbi)
{
    if (!(mbi->flags & MULTIBOOT_INFO_MMAP)) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("\nNo Multiboot memory map -- PMM not initialised.\n");
        return 0;
    }

    const uintptr_t mmap_start = (uintptr_t)mbi->mmap_addr;
    const uintptr_t mmap_end   = mmap_start + mbi->mmap_length;

    const uint32_t kernel_start = (uint32_t)(uintptr_t)_kernel_start;
    const uint32_t kernel_end   = (uint32_t)(uintptr_t)_kernel_end;

    /* Place the bitmap above the kernel AND above anything the loader still
     * owns. Sizing from _kernel_end alone puts it straight on top of the module
     * descriptor array, which the bitmap fill would destroy before it could be
     * read. Ranges below the kernel (the info struct and map live near 0x9500)
     * never move the floor. */
    loader_top = kernel_end;
    multiboot_for_each_owned_range(mbi, track_loader_top);

    const uint32_t bitmap_addr = align_up(loader_top, PMM_BLOCK_SIZE);

    /* First pass: report the usable regions and find the top of physical
     * memory, which is what the bitmap has to be sized against. */
    uint32_t mem_top = 0;
    uint32_t entries = 0;

    kprintf("\n");

    for (const struct multiboot_mmap_entry *e = (const struct multiboot_mmap_entry *)mmap_start;
         (uintptr_t)e < mmap_end; e = mmap_next(e)) {
        uint32_t base;
        uint32_t length;

        entries++;

        if (!mmap_range32(e, &base, &length)) {
            continue;
        }

        /* Only usable RAM sets the ceiling. Frames above the highest available
         * address can never be handed out, so covering them would just inflate
         * the bitmap -- QEMU reports the BIOS flash at 0xFFFC0000, which alone
         * would size it for the full 4 GiB. Reserved holes BELOW the ceiling
         * still stay protected: they simply never get freed. */
        if (e->type == MULTIBOOT_MEMORY_AVAILABLE && base + length > mem_top) {
            mem_top = base + length;
        }
    }

    pmm_init(mem_top, bitmap_addr);

    /* Second pass: everything the firmware calls usable becomes allocatable. */
    for (const struct multiboot_mmap_entry *e = (const struct multiboot_mmap_entry *)mmap_start;
         (uintptr_t)e < mmap_end; e = mmap_next(e)) {
        uint32_t base;
        uint32_t length;

        if (e->type == MULTIBOOT_MEMORY_AVAILABLE && mmap_range32(e, &base, &length)) {
            pmm_free_region(base, length);
        }
    }

    /* Then take back what is already spoken for. This MUST follow the free
     * pass: the kernel sits inside a region the firmware reports as available,
     * so its bits were just cleared and have to be set again.
     *
     * The low megabyte covers the interrupt vector table, the BIOS data area,
     * the EBDA and the Multiboot structure itself -- and keeps frame 0 out of
     * circulation so a NULL return from pmm_alloc_block is unambiguous. */
    pmm_reserve_region(0, 0x100000u);
    pmm_reserve_region(kernel_start, kernel_end - kernel_start);
    pmm_reserve_region(bitmap_addr, pmm_bitmap_size());

    /* Loader-owned ranges above 1 MiB -- module payloads especially -- sit
     * inside AVAILABLE regions and were just freed, so they need taking back
     * too or the allocator will hand an initrd out as scratch memory. */
    multiboot_for_each_owned_range(mbi, pmm_reserve_region);

    kprintf("PMM  : %u map entries; %u MiB usable, %u blocks of %u KiB\n",
            entries, mem_top / (1024u * 1024u), pmm_total_blocks(), PMM_BLOCK_SIZE / 1024u);
    kprintf("       %u free, %u used\n", pmm_free_blocks(), pmm_used_blocks());
    kprintf("       reserved: low 1MiB, kernel 0x%x-0x%x, bitmap 0x%x (%u B)\n",
            kernel_start, kernel_end, bitmap_addr, pmm_bitmap_size());

    return bitmap_addr + pmm_bitmap_size();
}

static void paging_test(void)
{
    void *const frame = pmm_alloc_block();

    if (frame == NULL) {
        kprintf("\npaging test: no free frame\n");
        return;
    }

    const uint32_t phys = (uint32_t)(uintptr_t)frame;
    const uint32_t virt = 0xA0000000u;

    if (!map_page(phys, virt, PAGE_WRITABLE)) {
        kprintf("\npaging test: map_page failed\n");
        return;
    }

    volatile uint32_t *const via_virtual = (volatile uint32_t *)(uintptr_t)virt;

    *via_virtual = 0xDEADBEEFu;

    /* Reading the same bytes back through the identity map proves the
     * translation actually landed on the intended frame. Writing and reading
     * one virtual address alone would pass even if it mapped somewhere else. */
    const volatile uint32_t *const via_physical =
        (const volatile uint32_t *)(uintptr_t)phys;

    kprintf("        mapped 0x%x -> 0x%x, readback via both %s\n", virt, phys,
            (*via_virtual == 0xDEADBEEFu && *via_physical == 0xDEADBEEFu) ? "OK"
                                                                         : "FAILED");
}

static void heap_test(void)
{
    kprintf("\nHeap : %u KiB at 0x%x; header %u B, payloads %u-byte aligned\n",
            kheap_total_bytes() / 1024u, KHEAP_VIRTUAL_BASE,
            (uint32_t)sizeof(struct kheap_block), KHEAP_ALIGNMENT);

    void *const a = kmalloc(32);
    void *const b = kmalloc(100);
    void *const c = kmalloc(7);

    if (a == NULL || b == NULL || c == NULL) {
        kprintf("  allocation failed\n");
        return;
    }

    /* ORing the three addresses puts every low bit that any of them set into
     * one value, so a single mask tests all three at once. */
    const bool aligned = (((uint32_t)(uintptr_t)a | (uint32_t)(uintptr_t)b |
                           (uint32_t)(uintptr_t)c) &
                          (KHEAP_ALIGNMENT - 1u)) == 0;

    kprintf("  kmalloc 32/100/7 -> %p %p %p %s\n", a, b, c,
            aligned ? "(8-byte aligned)" : "(MISALIGNED)");
    kprintf("  blocks=%u used=%u B free=%u B\n", kheap_block_count(),
            kheap_used_bytes(), kheap_free_bytes());

    kfree(b);

    void *const reused = kmalloc(64);

    kprintf("  kfree(b) then kmalloc(64) -> %p %s\n", reused,
            reused == b ? "(reused b's block)" : "(different block)");

    const uint32_t before = kheap_block_count();

    kfree(reused);
    kfree(a);
    kfree(c);

    kprintf("  freed all: blocks %u -> %u, largest free %u B %s\n", before,
            kheap_block_count(), kheap_largest_free_block(),
            kheap_block_count() == 1 ? "(fully coalesced)" : "(FRAGMENTED)");
}

void kernel_main(uint32_t magic, uint32_t mb_info_addr)
{
    vga_init();

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("Hello from the custom OS!\n\n");

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    gdt_init();
    idt_init();
    kprintf("CPU  : GDT 3 entries; IDT %d entries (0-31 exceptions, %d-%d IRQs)\n",
            IDT_ENTRIES, PIC_MASTER_VECTOR_OFFSET, PIC_SLAVE_VECTOR_OFFSET + 7);

    /* Must precede sti: until the PIC is remapped its lines still land on
     * vectors 8-15, where a keystroke arrives as a double fault. */
    pic_init();
    keyboard_init();
    kprintf("IRQ  : PIC remapped to %d-%d; keyboard on IRQ%d (vector %d)\n",
            PIC_MASTER_VECTOR_OFFSET, PIC_SLAVE_VECTOR_OFFSET + 7,
            IRQ_KEYBOARD, IRQ_VECTOR_BASE + IRQ_KEYBOARD);

    if (magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        const uint32_t protected_end =
            memory_init((const struct multiboot_info *)(uintptr_t)mb_info_addr);

        if (protected_end != 0) {
            paging_init();

            if (paging_is_enabled()) {
                kprintf("\nPaging: dir 0x%x, identity 0x0-0x%x, CR0.PG+WP set\n",
                        paging_directory_physical(), paging_identity_limit());
                paging_test();

                if (kheap_init()) {
                    heap_test();
                } else {
                    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                    kprintf("\nHeap failed to initialise.\n");
                }
            } else {
                vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                kprintf("\nPaging failed to initialise.\n");
            }
        }
    } else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("\nNot entered by a Multiboot loader -- no memory map available.\n");
    }

    __asm__ volatile ("sti");

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\nInterrupts enabled. Type something:\n\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* hlt rather than a busy spin: the CPU parks until the next interrupt, the
     * handler runs, and iret resumes here to park again. */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
