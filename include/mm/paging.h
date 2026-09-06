#ifndef MM_PAGING_H
#define MM_PAGING_H

#include <stdbool.h>
#include <stdint.h>

#define PAGE_SIZE 4096u

#define PAGE_DIRECTORY_ENTRIES 1024u
#define PAGE_TABLE_ENTRIES     1024u

/* A 32-bit linear address is three fields: bits 31-22 index the directory,
 * bits 21-12 index the page table, and bits 11-0 are the offset the hardware
 * adds after the walk. One directory entry therefore spans 4 MiB. */
#define PAGE_DIRECTORY_SHIFT 22u
#define PAGE_TABLE_SHIFT     12u
#define PAGE_INDEX_MASK      0x3FFu

/* Entry flag bits (Intel SDM Vol. 3A, section 4.3). The top 20 bits hold the
 * frame address, which is why every table must be 4 KiB aligned. */
#define PAGE_PRESENT       (1u << 0)
#define PAGE_WRITABLE      (1u << 1)
#define PAGE_USER          (1u << 2)
#define PAGE_WRITE_THROUGH (1u << 3)
#define PAGE_CACHE_DISABLE (1u << 4)
#define PAGE_ACCESSED      (1u << 5)
#define PAGE_DIRTY         (1u << 6)
#define PAGE_FRAME_MASK    0xFFFFF000u

/* Minimum size of the identity-mapped window created at init: below this
 * address virtual equals physical. paging_init extends it upward in 4 MiB
 * steps when anything the kernel must keep reachable -- the PMM bitmap, the
 * page directory, the page tables -- sits higher, which a large Multiboot
 * module causes. Call paging_identity_limit() for the value actually used. */
#define PAGING_IDENTITY_LIMIT 0x400000u /* 4 MiB */

/* One directory entry's worth of address space; the identity window is always
 * a whole number of these, since each one costs exactly one page table. */
#define PAGING_DIRECTORY_SPAN 0x400000u

/* Frames kept free BELOW the identity window for everything the kernel must
 * still be able to address directly once CR0.PG is set. Growing the window to
 * exactly the allocator's high-water mark is not enough: the tables built
 * while growing it raise that mark themselves, so the fixed point can settle
 * with no reachable frame left and every later mapping failing the
 * reachability guard.
 *
 * The reserve has to cover far more than those tables, because everything the
 * kernel fills by physical address comes out of it: the 1 MiB kernel heap and
 * its tables, each process's page directory and page tables, every ELF segment
 * frame the loader zero-fills, and every ring-3 stack frame. Sized at 64 KiB
 * this was invisible only because a small initrd happened to leave megabytes
 * of slack below the 4 MiB rounding; a 2 MiB initrd consumed that slack and
 * the heap alone exhausted the window, disabling the loader and then ring 3
 * on a machine with 128 MiB of free RAM. */
#define PAGING_TABLE_RESERVE (512u * PAGE_SIZE) /* 2 MiB */

/* Layout of a process address space, as built by the ELF loader.
 *
 * Below USER_IMAGE_BASE lies the identity-mapped kernel, which a process must
 * never be able to name; at 0xC0000000 and above lies the kernel heap. That
 * leaves the window between them, and the loader refuses any segment outside
 * it. The refusal is load-bearing rather than tidy: kernel page tables are
 * SHARED into every address space, so mapping a user page at a kernel address
 * would not shadow the kernel's mapping, it would overwrite it, in every
 * address space at once. */
#define USER_IMAGE_BASE   0x40000000u
#define USER_STACK_TOP    0xC0000000u /* exclusive; the kernel heap starts here */
#define USER_STACK_PAGES  4u          /* 16 KiB of ring-3 stack                 */
#define USER_STACK_BOTTOM (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)
#define USER_IMAGE_LIMIT  USER_STACK_BOTTOM

/* Builds the page directory, identity-maps the low 4 MiB and turns on the MMU. */
void paging_init(void);

/* Maps one 4 KiB page, allocating a page table from the PMM if the directory
 * has none for that range. Returns false if a frame could not be obtained, or
 * if a new table would land outside the reachable identity-mapped window.
 * flags are the PTE bits below PAGE_FRAME_MASK; PAGE_PRESENT is always set. */
bool map_page(uint32_t physical_addr, uint32_t virtual_addr, uint32_t flags);

/* True when ring 3 could read the given address: both the directory entry and
 * the page table entry must be present and user-accessible, which is the same
 * test the MMU applies. Used to vet pointers handed in by system calls rather
 * than dereferencing them on trust. */
bool paging_user_can_read(uint32_t virtual_addr);

/* As above, but the page must also be writable. A receiver could otherwise
 * point the kernel at its own read-only code page as a destination. */
bool paging_user_can_write(uint32_t virtual_addr);

/* As paging_user_can_read, but against an address space that is not the active
 * one -- for vetting an address in a process being built. Both levels are
 * tested, which a raw paging_entry_in cannot do: a page can be user-accessible
 * in a table shared from the kernel while the directory entry that reaches it
 * has the user bit cleared, and the MMU would then refuse what the bare page
 * table entry appears to permit. */
bool paging_user_can_read_in(uint32_t directory_phys, uint32_t virtual_addr);

/* Builds a fresh address space for a process and returns its page directory's
 * physical address, or 0.
 *
 * The kernel's own directory entries are copied in, so the kernel stays
 * addressable no matter which space is active -- that is what lets an
 * interrupt, a system call and the scheduler run with a process's CR3 loaded.
 * They are copied with the user bit CLEARED: a supervisor access ignores that
 * bit, so the kernel loses nothing, while ring 3 loses the ability to reach
 * any user page belonging to the tasks that live in identity-mapped memory.
 *
 * Copying entries means the page TABLES are shared, not duplicated, so a later
 * kernel mapping into an existing table appears in every address space. A
 * brand-new kernel directory entry would not; none is created after boot. */
uint32_t paging_create_address_space(void);

/* Frees a process address space: every frame and page table reachable from it
 * that the kernel does not also own, then the directory itself. Never touches
 * a table shared with the kernel directory, and refuses the kernel's own. */
void paging_destroy_address_space(uint32_t directory_phys);

/* map_page, but into an arbitrary address space rather than the kernel's. */
bool paging_map_in(uint32_t directory_phys, uint32_t physical_addr,
                   uint32_t virtual_addr, uint32_t flags);

/* The page table entry for an address in that address space, or 0 if the
 * directory has no table for it. Lets a caller find a page it already mapped
 * instead of allocating a second frame for it. */
uint32_t paging_entry_in(uint32_t directory_phys, uint32_t virtual_addr);

/* True when the kernel can reach a physical frame through the identity map,
 * which is what it needs in order to write one directly. Anything the kernel
 * must fill before or without mapping it -- a page table, a frame it is about
 * to zero-fill or copy a segment into -- has to satisfy this. */
bool paging_frame_is_reachable(uint32_t physical_addr);

uint32_t paging_directory_physical(void);
uint32_t paging_identity_limit(void);
bool     paging_is_enabled(void);

/* Reads CR2, which holds the faulting linear address after a page fault. */
uint32_t paging_fault_address(void);

#endif /* MM_PAGING_H */
