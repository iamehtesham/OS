#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mm/paging.h"
#include "mm/pmm.h"
#include "utils/stdio.h"

/* Both levels are plain arrays of 1024 32-bit entries, which is exactly one
 * 4 KiB frame. That is why pmm_alloc_block can back them directly: its
 * granularity IS the alignment the hardware demands, so no aligned attribute
 * or static reservation is involved. */
typedef uint32_t page_entry_t;

static page_entry_t *page_directory;
static uint32_t      page_directory_phys;
static bool          paging_enabled;

/* Extent of the identity map actually built, which is at least
 * PAGING_IDENTITY_LIMIT but grows to cover anything the PMM had already
 * reserved higher up. */
static uint32_t identity_limit = PAGING_IDENTITY_LIMIT;

static uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

/* Defined in paging_enable.S. */
extern void paging_enable(uint32_t page_directory_physical);

/* Drops any cached translation for one address. The CPU does not observe writes
 * to page tables on its own, so a table edit without this can be ignored in
 * favour of a stale TLB entry. */
static inline void tlb_invalidate(uint32_t virtual_addr)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

/* pmm_alloc_block returns whatever bytes the frame already held. A leftover
 * Present bit in an uninitialised entry would map a garbage physical address
 * that the MMU would use without complaint, so every entry starts cleared. */
static void zero_frame(void *frame)
{
    uint32_t *const words = (uint32_t *)frame;

    for (uint32_t i = 0; i < PAGE_SIZE / sizeof(uint32_t); i++) {
        words[i] = 0;
    }
}

bool paging_frame_is_reachable(uint32_t physical_addr)
{
    /* Before CR0.PG every physical address is trivially addressable. */
    return !paging_enabled || physical_addr < identity_limit;
}

/* True when the page table this directory would use for `dir_index` belongs to
 * the KERNEL directory rather than to this address space.
 *
 * paging_create_address_space copies the kernel's directory entries, and that
 * shares the page TABLES themselves rather than duplicating them. Writing a
 * process's mapping into one of those would not give the process a private
 * page: it would edit the kernel's own table, and with it every address space
 * that shares that table. */
static bool table_is_kernel_owned(const page_entry_t *dir, uint32_t dir_index)
{
    /* The kernel writing its own tables is the ordinary case. */
    if (page_directory == NULL || dir == page_directory) {
        return false;
    }

    if ((dir[dir_index] & PAGE_PRESENT) == 0 ||
        (page_directory[dir_index] & PAGE_PRESENT) == 0) {
        return false;
    }

    return (dir[dir_index] & PAGE_FRAME_MASK) ==
           (page_directory[dir_index] & PAGE_FRAME_MASK);
}

/* The shared body of every mapping operation. `dir` must itself be reachable
 * through the identity map, since this writes to it and to the page table
 * beneath it by physical address. */
static bool map_into(page_entry_t *dir, uint32_t physical_addr, uint32_t virtual_addr,
                     uint32_t flags)
{
    const uint32_t dir_index   = (virtual_addr >> PAGE_DIRECTORY_SHIFT) & PAGE_INDEX_MASK;
    const uint32_t table_index = (virtual_addr >> PAGE_TABLE_SHIFT) & PAGE_INDEX_MASK;

    if (dir == NULL) {
        return false;
    }

    /* An address space may only write tables it owns. This is the real
     * invariant behind the ELF loader's address window, and it is the one that
     * holds: what makes a mapping safe is not where it lands but whether the
     * table underneath belongs to this address space. Checking an address
     * range instead silently depends on the kernel never mapping anything
     * inside that range -- and the kernel does, so a segment aimed there was
     * written straight into the kernel's table, handing the process kernel
     * frames and handing two processes the same frames as each other. */
    if (table_is_kernel_owned(dir, dir_index)) {
        return false;
    }

    if ((dir[dir_index] & PAGE_PRESENT) == 0) {
        void *const frame = pmm_alloc_block();

        if (frame == NULL) {
            return false;
        }

        const uint32_t table_phys = (uint32_t)(uintptr_t)frame;

        /* The kernel has to WRITE this table, so it must be able to address it.
         * Before paging is on that is trivially true. Afterwards it holds only
         * while the PMM keeps handing out frames inside the identity-mapped
         * window -- past that the write below would itself page fault, with no
         * obvious cause. Fail here instead. */
        if (!paging_frame_is_reachable(table_phys)) {
            pmm_free_block(frame);
            return false;
        }

        zero_frame(frame);

        /* The CPU ANDs permissions along the walk, so the directory entry has
         * to be at least as permissive as any page beneath it: a read-only PDE
         * would force every page in its 4 MiB range read-only no matter what
         * the PTE says. USER is propagated because it is likewise ANDed. */
        dir[dir_index] = (table_phys & PAGE_FRAME_MASK) | PAGE_PRESENT | PAGE_WRITABLE |
                         (flags & PAGE_USER);
    } else if ((flags & PAGE_USER) != 0) {
        /* The table already exists, and it was almost certainly built for
         * supervisor pages. Permissions are ANDed along the walk, so a user
         * page underneath a supervisor-only directory entry is still
         * unreachable from ring 3. Widen the entry.
         *
         * This does not open the other pages in the range: each PTE keeps its
         * own user bit, and both levels must agree. */
        dir[dir_index] |= PAGE_USER;
    }

    page_entry_t *const table =
        (page_entry_t *)(uintptr_t)(dir[dir_index] & PAGE_FRAME_MASK);

    const uint32_t previous = table[table_index];

    table[table_index] =
        (physical_addr & PAGE_FRAME_MASK) | (flags & ~PAGE_FRAME_MASK) | PAGE_PRESENT;

    /* Replacing a live mapping with a DIFFERENT frame: the old frame just lost
     * this reference, and nothing else would ever notice. It would stay
     * allocated with a count that no page table backs, unreachable and
     * unfreeable for the life of the machine. Re-mapping the SAME frame is not
     * a replacement -- the ELF loader does it to widen permissions on a page two
     * segments share -- so the frames are compared rather than just the
     * present bit. */
    if ((previous & PAGE_PRESENT) != 0 &&
        (previous & PAGE_FRAME_MASK) != (physical_addr & PAGE_FRAME_MASK)) {
        pmm_free_block((void *)(uintptr_t)(previous & PAGE_FRAME_MASK));
    }

    tlb_invalidate(virtual_addr);

    return true;
}

bool map_page(uint32_t physical_addr, uint32_t virtual_addr, uint32_t flags)
{
    return map_into(page_directory, physical_addr, virtual_addr, flags);
}

bool paging_map_in(uint32_t directory_phys, uint32_t physical_addr, uint32_t virtual_addr,
                   uint32_t flags)
{
    if (directory_phys == 0 || !paging_frame_is_reachable(directory_phys)) {
        return false;
    }

    return map_into((page_entry_t *)(uintptr_t)directory_phys, physical_addr, virtual_addr,
                    flags);
}

uint32_t paging_entry_in(uint32_t directory_phys, uint32_t virtual_addr)
{
    if (directory_phys == 0) {
        return 0;
    }

    const page_entry_t *const dir       = (const page_entry_t *)(uintptr_t)directory_phys;
    const uint32_t            dir_index = (virtual_addr >> PAGE_DIRECTORY_SHIFT) & PAGE_INDEX_MASK;

    if ((dir[dir_index] & PAGE_PRESENT) == 0) {
        return 0;
    }

    const page_entry_t *const table =
        (const page_entry_t *)(uintptr_t)(dir[dir_index] & PAGE_FRAME_MASK);

    return table[(virtual_addr >> PAGE_TABLE_SHIFT) & PAGE_INDEX_MASK];
}

uint32_t paging_create_address_space(void)
{
    if (page_directory == NULL) {
        return 0;
    }

    void *const frame = pmm_alloc_block();

    if (frame == NULL) {
        return 0;
    }

    const uint32_t directory_phys = (uint32_t)(uintptr_t)frame;

    /* The kernel writes this directory by physical address, both here and on
     * every later mapping into it. */
    if (!paging_frame_is_reachable(directory_phys)) {
        pmm_free_block(frame);
        return 0;
    }

    page_entry_t *const dir = (page_entry_t *)frame;

    for (uint32_t i = 0; i < PAGE_DIRECTORY_ENTRIES; i++) {
        const uint32_t pde = page_directory[i];

        /* Share the kernel's mappings, minus the user bit. Permissions are
         * ANDed along the walk, so clearing it here makes every page under
         * this entry unreachable from ring 3 in THIS address space, whatever
         * the individual page table entries say -- and the entries are shared,
         * so they cannot be edited per process. The kernel is unaffected: a
         * CPL 0 access does not consult the user bit at all.
         *
         * That is what keeps a loaded process from reading the ring-3 code and
         * stacks of the tasks that live in identity-mapped low memory. */
        dir[i] = (pde & PAGE_PRESENT) ? (pde & ~PAGE_USER) : 0;
    }

    return directory_phys;
}

void paging_destroy_address_space(uint32_t directory_phys)
{
    if (directory_phys == 0 || page_directory == NULL) {
        return;
    }

    /* The kernel's own address space is not a process's to free, and freeing
     * it would unmap the code doing the freeing. */
    if (directory_phys == page_directory_phys) {
        return;
    }

    page_entry_t *const dir = (page_entry_t *)(uintptr_t)directory_phys;

    for (uint32_t i = 0; i < PAGE_DIRECTORY_ENTRIES; i++) {
        if ((dir[i] & PAGE_PRESENT) == 0) {
            continue;
        }

        /* Same table as the kernel's: shared in, not allocated here. Freeing
         * it would pull the kernel's own mappings out from under every other
         * address space. */
        if ((dir[i] & PAGE_FRAME_MASK) == (page_directory[i] & PAGE_FRAME_MASK)) {
            continue;
        }

        page_entry_t *const table = (page_entry_t *)(uintptr_t)(dir[i] & PAGE_FRAME_MASK);

        for (uint32_t j = 0; j < PAGE_TABLE_ENTRIES; j++) {
            if ((table[j] & PAGE_PRESENT) != 0) {
                pmm_free_block((void *)(uintptr_t)(table[j] & PAGE_FRAME_MASK));
            }
        }

        pmm_free_block(table);
    }

    pmm_free_block(dir);
}

void paging_init(uint32_t extra_reserve)
{
    void *const dir_frame = pmm_alloc_block();

    if (dir_frame == NULL) {
        kprintf("paging: no free frame for the page directory\n");
        return;
    }

    /* Zeroed for exactly the reason every page table is: pmm_alloc_block hands
     * back whatever bytes the frame held, and a stray Present bit in an entry
     * for a range nothing maps would name a garbage table. Worse here than in a
     * table, because paging_create_address_space copies every present directory
     * entry into every process, so one leftover bit would propagate that
     * garbage into every address space on the machine. */
    zero_frame(dir_frame);

    page_directory_phys = (uint32_t)(uintptr_t)dir_frame;
    page_directory      = (page_entry_t *)(uintptr_t)dir_frame;

    /* Identity-map before touching CR0. The window has to cover the kernel text
     * and stack, the VGA buffer at 0xB8000, the PMM bitmap, this directory and
     * every page table -- the instruction right after CR0.PG is set is already
     * fetched through the MMU, so anything missing here is unrecoverable.
     *
     * 4 MiB is the floor, not the answer. Whatever the PMM has already marked
     * used must be inside the window too, and a large Multiboot module pushes
     * the first free frame well past 4 MiB, taking the bitmap and this
     * directory with it. Mapping itself allocates page tables, which can raise
     * the requirement again, so this iterates to a fixed point -- it converges
     * quickly because each extra 4 MiB of window costs only one 4 KiB table. */
    uint32_t mapped = 0;

    for (;;) {
        const uint32_t required = pmm_highest_used_address();

        /* Reserve headroom rather than converging exactly on the high-water
         * mark: map_page still has to allocate page tables after CR0.PG is
         * set, and those come from the first free frame. Land the window flush
         * against the mark and that frame is outside it, so every later
         * mapping fails the reachability guard -- which is how a mid-sized
         * Multiboot module can leave the kernel with no heap at all.
         *
         * The caller's extra_reserve is added because the tables are only the
         * beginning: the heap and every process's frames come out of the same
         * window, and how much that is depends on what is being loaded. */
        const uint32_t headroom = PAGING_TABLE_RESERVE + extra_reserve;

        if (required <= UINT32_MAX - headroom) {
            const uint32_t wanted = align_up(required + headroom, PAGING_DIRECTORY_SPAN);

            /* align_up wraps to 0 within one span of the top of memory; only
             * take the new value when it is genuinely an increase. */
            if (wanted > identity_limit) {
                identity_limit = wanted;
            }
        }

        if (mapped >= identity_limit) {
            break;
        }

        while (mapped < identity_limit) {
            if (!map_page(mapped, mapped, PAGE_WRITABLE)) {
                kprintf("paging: identity map failed at 0x%x\n", mapped);
                page_directory = NULL;
                return;
            }

            mapped += PAGE_SIZE;
        }
    }

    /* The whole point of the loop is that allocation can still proceed once
     * translation is on. Say so out loud rather than discovering it later as an
     * unexplained map_page failure. */
    if (pmm_highest_used_address() >= identity_limit) {
        kprintf("paging: no reachable frames left below 0x%x\n", identity_limit);
        page_directory = NULL;
        return;
    }

    /* Reachable free space is what every later allocation draws on, so report
     * it rather than leaving a shortfall to surface as an unexplained failure
     * to start a process several screens later. */
    const uint32_t reachable_free = identity_limit - pmm_highest_used_address();

    if (reachable_free < extra_reserve) {
        kprintf("paging: only %u KiB reachable below 0x%x, %u KiB wanted\n",
                reachable_free / 1024u, identity_limit, extra_reserve / 1024u);
    }

    paging_enable(page_directory_phys);
    paging_enabled = true;
}

uint32_t paging_directory_physical(void)
{
    return page_directory_phys;
}

uint32_t paging_identity_limit(void)
{
    return identity_limit;
}

bool paging_is_enabled(void)
{
    return paging_enabled;
}

/* The address space the CPU is translating through right now.
 *
 * A process with its own page directory must have its pointers checked against
 * THAT directory: the kernel's would both accept addresses the caller cannot
 * reach and reject ones it can, and a system call runs with the caller's CR3
 * still loaded. Every directory is identity-mapped, so CR3 doubles as a
 * pointer to it. */
static const page_entry_t *active_directory(void)
{
    if (!paging_enabled) {
        return page_directory;
    }

    uint32_t cr3;

    __asm__ volatile ("movl %%cr3, %0" : "=r"(cr3));

    return (const page_entry_t *)(uintptr_t)(cr3 & PAGE_FRAME_MASK);
}

/* Both levels must carry every required bit, because that is exactly the test
 * the hardware applies: permissions are ANDed along the walk, and checking only
 * the PTE would report pages reachable that the MMU would in fact refuse. */
static bool page_has(const page_entry_t *directory, uint32_t virtual_addr, uint32_t required)
{
    if (directory == NULL) {
        return false;
    }

    const uint32_t dir_index   = (virtual_addr >> PAGE_DIRECTORY_SHIFT) & PAGE_INDEX_MASK;
    const uint32_t table_index = (virtual_addr >> PAGE_TABLE_SHIFT) & PAGE_INDEX_MASK;
    const uint32_t pde         = directory[dir_index];

    if ((pde & required) != required) {
        return false;
    }

    const page_entry_t *const table =
        (const page_entry_t *)(uintptr_t)(pde & PAGE_FRAME_MASK);

    return (table[table_index] & required) == required;
}

bool paging_user_can_read(uint32_t virtual_addr)
{
    return page_has(active_directory(), virtual_addr, PAGE_PRESENT | PAGE_USER);
}

bool paging_user_can_write(uint32_t virtual_addr)
{
    return page_has(active_directory(), virtual_addr, PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE);
}

bool paging_user_can_read_in(uint32_t directory_phys, uint32_t virtual_addr)
{
    if (directory_phys == 0) {
        return false;
    }

    return page_has((const page_entry_t *)(uintptr_t)directory_phys, virtual_addr,
                    PAGE_PRESENT | PAGE_USER);
}

uint32_t paging_fault_address(void)
{
    uint32_t cr2;

    __asm__ volatile ("movl %%cr2, %0" : "=r"(cr2));

    return cr2;
}
