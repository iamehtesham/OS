#include <stdbool.h>
#include <stdint.h>

#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/irq.h"
#include "cpu/pic.h"
#include "cpu/tss.h"
#include "drivers/keyboard.h"
#include "drivers/vga.h"
#include "drivers/pit.h"
#include "fs/initrd.h"
#include "fs/vfs.h"
#include "mm/kheap.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "sys/syscall.h"
#include "task/scheduler.h"
#include "task/task.h"
#include "multiboot.h"
#include "utils/stdio.h"
#include "utils/string.h"

/* Defined in src/task/user.S. */
extern void switch_to_user_mode(uint32_t entry, uint32_t user_stack_top);

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

    /* First pass: find the top of physical memory, which is what the bitmap
     * has to be sized against. */
    uint32_t mem_top = 0;

    for (const struct multiboot_mmap_entry *e = (const struct multiboot_mmap_entry *)mmap_start;
         (uintptr_t)e < mmap_end; e = mmap_next(e)) {
        uint32_t base;
        uint32_t length;

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

    kprintf("PMM  : %u MiB usable, %u blocks of %u KiB; %u free, %u used\n",
            mem_top / (1024u * 1024u), pmm_total_blocks(), PMM_BLOCK_SIZE / 1024u,
            pmm_free_blocks(), pmm_used_blocks());

    return bitmap_addr + pmm_bitmap_size();
}

static bool paging_test(void)
{
    void *const frame = pmm_alloc_block();

    if (frame == NULL) {
        return false;
    }

    const uint32_t phys = (uint32_t)(uintptr_t)frame;
    const uint32_t virt = 0xA0000000u;

    if (!map_page(phys, virt, PAGE_WRITABLE)) {
        return false;
    }

    volatile uint32_t *const via_virtual = (volatile uint32_t *)(uintptr_t)virt;

    *via_virtual = 0xDEADBEEFu;

    /* Reading the same bytes back through the identity map proves the
     * translation actually landed on the intended frame. Writing and reading
     * one virtual address alone would pass even if it mapped somewhere else. */
    const volatile uint32_t *const via_physical =
        (const volatile uint32_t *)(uintptr_t)phys;

    return *via_virtual == 0xDEADBEEFu && *via_physical == 0xDEADBEEFu;
}

/* Exercises alignment, block reuse and full coalescing, and collapses the
 * result to one line -- the detailed walk-through served its purpose when the
 * allocator was new, and the screen is only 25 rows. */
static void heap_test(void)
{
    void *const a = kmalloc(32);
    void *const b = kmalloc(100);
    void *const c = kmalloc(7);

    /* ORing the addresses puts every low bit any of them set into one value,
     * so a single mask tests all three at once. */
    const bool aligned = a != NULL && b != NULL && c != NULL &&
                         ((((uint32_t)(uintptr_t)a | (uint32_t)(uintptr_t)b |
                            (uint32_t)(uintptr_t)c) &
                           (KHEAP_ALIGNMENT - 1u)) == 0);

    kfree(b);

    void *const reused   = kmalloc(64);
    const bool  recycled = (reused == b);

    kfree(reused);
    kfree(a);
    kfree(c);

    const bool coalesced =
        kheap_block_count() == 1 &&
        kheap_largest_free_block() == KHEAP_SIZE - (uint32_t)sizeof(struct kheap_block);

    kprintf("Heap : %u KiB at 0x%x; align=%s reuse=%s coalesce=%s\n",
            kheap_total_bytes() / 1024u, KHEAP_VIRTUAL_BASE,
            aligned ? "ok" : "FAIL", recycled ? "ok" : "FAIL",
            coalesced ? "ok" : "FAIL");
}

/* Returns the first Multiboot module's span. The initrd is handed to us this
 * way rather than read from a disk, which is the whole point of an initrd. */
static bool multiboot_first_module(const struct multiboot_info *mbi, uint32_t *start,
                                   uint32_t *size)
{
    if ((mbi->flags & MULTIBOOT_INFO_MODS) == 0 || mbi->mods_count == 0) {
        return false;
    }

    const struct multiboot_mod_list *const mods =
        (const struct multiboot_mod_list *)(uintptr_t)mbi->mods_addr;

    if (mods[0].mod_end <= mods[0].mod_start) {
        return false;
    }

    *start = mods[0].mod_start;
    *size  = mods[0].mod_end - mods[0].mod_start;

    return true;
}

static void vfs_test(const struct multiboot_info *mbi)
{
    uint32_t start;
    uint32_t size;

    if (!multiboot_first_module(mbi, &start, &size)) {
        vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
        kprintf("\nVFS  : no Multiboot module loaded (use 'make qemu')\n");
        return;
    }

    fs_root = initrd_init(start, size);

    if (fs_root == NULL) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("\nVFS  : initrd failed to mount\n");
        return;
    }

    kprintf("\nVFS  : initrd at 0x%x (%u B), %u files, '%s' mounted\n", start, size,
            initrd_file_count(), fs_root->name);

    /* Everything below goes through the vfs_* wrappers rather than the driver,
     * so this listing would work unchanged against any filesystem that fills
     * in readdir and finddir. */
    for (uint32_t index = 0;; index++) {
        struct dirent *const entry = vfs_readdir(fs_root, index);

        if (entry == NULL) {
            break;
        }

        fs_node_t *const node = vfs_finddir(fs_root, entry->name);

        kprintf("  %s (%u B)\n", entry->name, node != NULL ? node->length : 0u);
    }

    fs_node_t *const file = vfs_finddir(fs_root, "hello.txt");

    if (file == NULL) {
        kprintf("  hello.txt not found\n");
        return;
    }

    vfs_open(file);

    uint8_t        buffer[128];
    const uint32_t got = vfs_read(file, 0, (uint32_t)sizeof(buffer) - 1u, buffer);

    buffer[got] = '\0';

    vfs_close(file);

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("\ncat /%s (%u of %u B):\n", file->name, got, file->length);
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    kprintf("%s", (const char *)buffer);

    /* The image is read-only, so the write slot reports zero bytes written
     * rather than pretending to succeed. */
    const uint8_t byte = 'x';

    kprintf("vfs_write on a read-only file returned %u bytes\n",
            vfs_write(file, 0, 1, &byte));
}

/* Emitted by boot.S: the top of the original kernel stack, which is where the
 * CPU lands when ring 3 traps into the kernel. */
extern uint8_t stack_top[];

/* Burns roughly a timeslice so the alternating pattern is legible instead of
 * scrolling past faster than it can be read. volatile stops GCC deleting a
 * loop with no effect. */
static void task_delay(void)
{
    for (volatile uint32_t i = 0; i < 3000000u; i++) {
    }
}

/* Both tasks share the VGA driver with no locking, so a preemption partway
 * through kprintf can interleave two writes. With one character per call the
 * window is a few instructions wide and the worst case is a transposed
 * character -- worth knowing, and the reason a real kernel needs a lock here. */
static void task_a(void)
{
    for (;;) {
        kprintf("A");
        task_delay();
    }
}

static void task_b(void)
{
    for (;;) {
        kprintf("B");
        task_delay();
    }
}

/* ---- ring 3 ---------------------------------------------------------- */

/* Lives in the kernel image's .rodata, so the page holding it has to be made
 * user-readable before ring 3 can pass it to the kernel. */
static const char user_message[] = "  [ring 3] hello via int 0x80\n";

/* Runs at CPL 3. It may not execute cli, sti, hlt, in, out, or touch any
 * control register -- every one of those faults at this privilege level. The
 * only way it can reach the kernel at all is the int 0x80 gate. */
static void user_test(void)
{
    for (;;) {
        uint32_t written;

        /* EAX carries the call number and comes back holding the result; EBX
         * carries the argument. The memory clobber stops GCC caching anything
         * across a call that prints. */
        __asm__ volatile ("int $0x80"
                          : "=a"(written)
                          : "a"(SYS_PRINT), "b"(user_message)
                          : "memory");

        (void)written;

        /* A busy loop is the only way ring 3 can pace itself: hlt is
         * privileged. The timer still preempts this, which is what proves the
         * scheduler survives the privilege drop. */
        for (volatile uint32_t i = 0; i < 12000000u; i++) {
        }
    }
}

/* Identity-maps a range as user-accessible. map_page also widens the directory
 * entry, which matters because these pages sit under the kernel's existing
 * supervisor-only table and permissions are ANDed along the walk. */
static bool map_user_range(uint32_t start, uint32_t length, uint32_t flags)
{
    const uint32_t first = start & PAGE_FRAME_MASK;
    const uint32_t last  = (start + length - 1u) & PAGE_FRAME_MASK;

    for (uint32_t page = first; page <= last; page += PAGE_SIZE) {
        /* map_page really can fail -- it needs a frame for a new page table,
         * and refuses one it could not reach after CR0.PG. Dropping to ring 3
         * with an unmapped stack would fault on the first push. */
        if (!map_page(page, page, PAGE_USER | flags)) {
            return false;
        }
    }

    return true;
}

static void enter_user_mode(void)
{
    void *const stack_frame = pmm_alloc_block();

    if (stack_frame == NULL) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("\nNo frame for a user stack.\n");
        return;
    }

    const uint32_t user_stack = (uint32_t)(uintptr_t)stack_frame;

    /* Ring 3 needs exactly three things reachable: the code it runs, the
     * string it hands to the kernel, and a stack. Everything else stays
     * supervisor-only, so a stray user pointer is refused by the MMU rather
     * than quietly honoured.
     *
     * The code and string pages are mapped without PAGE_WRITABLE, so ring 3
     * can read and execute but not modify them. They do share their pages with
     * neighbouring kernel code and rodata, which ring 3 can therefore read --
     * a real information leak, and the reason a grown-up kernel links user
     * code into its own section. */
    const bool mapped =
        map_user_range((uint32_t)(uintptr_t)user_test, PAGE_SIZE, 0) &&
        map_user_range((uint32_t)(uintptr_t)user_message, sizeof(user_message), 0) &&
        map_user_range(user_stack, PAGE_SIZE, PAGE_WRITABLE);

    if (!mapped) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("\nCould not map the ring-3 pages; staying in ring 0.\n");
        pmm_free_block(stack_frame);
        return;
    }

    kprintf("User  : entry %p, stack %p, gate int 0x%x (DPL 3)\n",
            (void *)(uintptr_t)user_test, (void *)(uintptr_t)(user_stack + PAGE_SIZE),
            SYSCALL_VECTOR);

    /* Does not return: the only way down to ring 3 is an iret, and there is no
     * instruction that comes back up except a trap. */
    switch_to_user_mode((uint32_t)(uintptr_t)user_test, user_stack + PAGE_SIZE);
}

static void tasking_start(void)
{
    if (!tasking_init()) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("\nTasking failed to initialise.\n");
        return;
    }

    /* Order matters: the chip and the hook must both be live before the first
     * tick can arrive, and no tick can arrive until sti below. */
    pit_init(100);
    scheduler_init();

    if (create_task(task_a) == NULL || create_task(task_b) == NULL) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("\nCould not create tasks.\n");
        return;
    }

    kprintf("Tasks : PIT at %u Hz, %u tasks (kernel + A + B), round robin\n",
            pit_frequency(), task_count());
}

void kernel_main(uint32_t magic, uint32_t mb_info_addr)
{
    vga_init();

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("Hello from the custom OS!\n\n");

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    gdt_init();

    /* After the GDT, which must already hold the descriptor ltr will load, and
     * before anything can trap from ring 3. */
    tss_init((uint32_t)(uintptr_t)stack_top);

    idt_init();
    kprintf("CPU  : GDT 6 entries (+ring3 code/data, TSS); IDT %d entries\n", IDT_ENTRIES);

    /* Must precede sti: until the PIC is remapped its lines still land on
     * vectors 8-15, where a keystroke arrives as a double fault. */
    pic_init();
    keyboard_init();
    kprintf("IRQ  : PIC remapped to %d-%d; keyboard on IRQ%d (vector %d)\n",
            PIC_MASTER_VECTOR_OFFSET, PIC_SLAVE_VECTOR_OFFSET + 7,
            IRQ_KEYBOARD, IRQ_VECTOR_BASE + IRQ_KEYBOARD);

    if (magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        const struct multiboot_info *const mbi =
            (const struct multiboot_info *)(uintptr_t)mb_info_addr;
        const uint32_t protected_end = memory_init(mbi);

        if (protected_end != 0) {
            paging_init();

            if (paging_is_enabled()) {
                kprintf("Paging: identity 0x0-0x%x, CR0.PG+WP set, test map %s\n",
                        paging_identity_limit(), paging_test() ? "ok" : "FAIL");

                if (kheap_init()) {
                    heap_test();
                    vfs_test(mbi);
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

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    tasking_start();

    __asm__ volatile ("sti");

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\nInterrupts on. A and B run in ring 0, the message below in ring 3:\n\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* Drops this thread to ring 3 and never comes back. The tasks created above
     * keep running in ring 0, preempting it, which is the point: the privilege
     * drop does not stop the scheduler. */
    enter_user_mode();

    /* Only reached if entering ring 3 failed. */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
