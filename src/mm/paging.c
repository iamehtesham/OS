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

bool map_page(uint32_t physical_addr, uint32_t virtual_addr, uint32_t flags)
{
    const uint32_t dir_index   = (virtual_addr >> PAGE_DIRECTORY_SHIFT) & PAGE_INDEX_MASK;
    const uint32_t table_index = (virtual_addr >> PAGE_TABLE_SHIFT) & PAGE_INDEX_MASK;

    if (page_directory == NULL) {
        return false;
    }

    if ((page_directory[dir_index] & PAGE_PRESENT) == 0) {
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
        if (paging_enabled && table_phys >= identity_limit) {
            pmm_free_block(frame);
            return false;
        }

        zero_frame(frame);

        /* The CPU ANDs permissions along the walk, so the directory entry has
         * to be at least as permissive as any page beneath it: a read-only PDE
         * would force every page in its 4 MiB range read-only no matter what
         * the PTE says. USER is propagated because it is likewise ANDed. */
        page_directory[dir_index] = (table_phys & PAGE_FRAME_MASK) | PAGE_PRESENT |
                                    PAGE_WRITABLE | (flags & PAGE_USER);
    }

    page_entry_t *const table =
        (page_entry_t *)(uintptr_t)(page_directory[dir_index] & PAGE_FRAME_MASK);

    table[table_index] =
        (physical_addr & PAGE_FRAME_MASK) | (flags & ~PAGE_FRAME_MASK) | PAGE_PRESENT;

    tlb_invalidate(virtual_addr);

    return true;
}

void paging_init(void)
{
    void *const dir_frame = pmm_alloc_block();

    if (dir_frame == NULL) {
        kprintf("paging: no free frame for the page directory\n");
        return;
    }

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

        if (required > identity_limit) {
            identity_limit = align_up(required, PAGING_DIRECTORY_SPAN);
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

uint32_t paging_fault_address(void)
{
    uint32_t cr2;

    __asm__ volatile ("movl %%cr2, %0" : "=r"(cr2));

    return cr2;
}
