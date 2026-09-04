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

/* Frames kept free BELOW the identity window for the page tables map_page must
 * still allocate after CR0.PG is set. Growing the window to exactly the
 * allocator's high-water mark is not enough: the tables built while growing it
 * raise that mark themselves, so the fixed point can settle with no reachable
 * frame left and every later mapping failing the reachability guard. */
#define PAGING_TABLE_RESERVE (16u * PAGE_SIZE) /* 64 KiB, i.e. 16 page tables */

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

uint32_t paging_directory_physical(void);
uint32_t paging_identity_limit(void);
bool     paging_is_enabled(void);

/* Reads CR2, which holds the faulting linear address after a page fault. */
uint32_t paging_fault_address(void);

#endif /* MM_PAGING_H */
