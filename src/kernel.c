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
#include "task/elf.h"
#include "task/scheduler.h"
#include "task/task.h"
#include "user/ipc_demo.h"
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

/* Where the paging self-test parks its scratch mapping.
 *
 * It must stay OUT of the process window. This mapping is never removed, so
 * the kernel directory keeps a page table for its 4 MiB span forever, and
 * paging_create_address_space shares that table into every process. A process
 * image linked inside the span would then be refused by map_into -- which is
 * the correct refusal, but the address would be unusable for a reason that has
 * nothing to do with the image. Sitting at 0xA0000000 it was worse than
 * unusable: before map_into checked table ownership, a segment aimed there was
 * written into the kernel's own page table. */
#define PAGING_TEST_ADDRESS 0xE0000000u

_Static_assert(PAGING_TEST_ADDRESS < USER_IMAGE_BASE || PAGING_TEST_ADDRESS >= USER_IMAGE_LIMIT,
               "the paging self-test mapping must stay outside the process address window");

static bool paging_test(void)
{
    void *const frame = pmm_alloc_block();

    if (frame == NULL) {
        return false;
    }

    const uint32_t phys = (uint32_t)(uintptr_t)frame;
    const uint32_t virt = PAGING_TEST_ADDRESS;

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

/* Reads an executable out of the initrd through the VFS and starts it as a
 * ring-3 process with an address space of its own.
 *
 * This is the first code here that runs a program the kernel was not linked
 * with: everything ring 3 has executed so far was compiled into the kernel
 * image and merely placed in user-accessible sections. */
static void process_start(const char *name)
{
    if (fs_root == NULL) {
        kprintf("Exec : no filesystem mounted, so /%s cannot be started\n", name);
        return;
    }

    fs_node_t *const file = vfs_finddir(fs_root, name);

    if (file == NULL) {
        kprintf("Exec : /%s is not in the initrd\n", name);
        return;
    }

    /* Staged through the heap rather than parsed where it lies: the loader
     * wants one flat readable image, and reading it through the VFS is what
     * makes this work against any filesystem rather than just the initrd. */
    uint8_t *const image = kmalloc(file->length);

    if (image == NULL) {
        kprintf("Exec : no heap for a %u byte image\n", file->length);
        return;
    }

    vfs_open(file);
    const uint32_t got = vfs_read(file, 0, file->length, image);
    vfs_close(file);

    elf_image_t loaded;

    if (got != file->length || !elf_load(image, got, &loaded)) {
        kfree(image);
        kprintf("Exec : /%s could not be loaded\n", name);
        return;
    }

    /* The segments now live in frames of their own, so the staging copy has
     * done its job. */
    kfree(image);

    task_t *const process = create_user_process(loaded.entry, loaded.directory);

    if (process == NULL) {
        kprintf("Exec : /%s loaded but could not be spawned\n", name);
        return;
    }

    kprintf("Exec : /%s -> pid %u, entry %p, private cr3 %p\n", name, process->pid,
            (void *)(uintptr_t)loaded.entry, (void *)(uintptr_t)loaded.directory);
    kprintf("       %u PT_LOAD, %u pages, %u B from file + %u B zero-filled bss\n",
            loaded.segments, loaded.pages, loaded.file_bytes, loaded.bss_bytes);
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

    /* The receiver is created first so it gets IPC_DEMO_RECEIVER_PID, which is
     * the pid the sender addresses. */
    task_t *const receiver = create_user_task(ipc_demo_receiver);
    task_t *const sender   = create_user_task(ipc_demo_sender);

    if (receiver == NULL || sender == NULL) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("\nCould not create the user tasks.\n");
        return;
    }

    kprintf("Tasks : PIT %u Hz; ring-3 receiver pid %u, sender pid %u; kernel idles\n",
            pit_frequency(), receiver->pid, sender->pid);

    /* Both of those were linked into the kernel image. This one is not. */
    process_start("dummy.elf");
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
    kprintf("\nInterrupts on. Two ring-3 tasks pass messages through int 0x80:\n\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* The kernel thread is the idle task. It never blocks and never dies, so
     * the scheduler always has somewhere to go when every other task is
     * waiting on a message -- which is what makes blocking in recv safe. The
     * scheduler only picks it when nothing else is runnable, so hlt here means
     * the whole machine genuinely has nothing to do until the next interrupt. */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
