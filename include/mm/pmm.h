#ifndef MM_PMM_H
#define MM_PMM_H

#include <stdint.h>

/* Allocation granularity. The shift is what the implementation actually uses:
 * dividing by a power of two keeps every address/frame conversion a shift, so
 * nothing here can emit a libgcc division helper. */
#define PMM_BLOCK_SIZE  4096u
#define PMM_BLOCK_SHIFT 12u

/* Sizes the bitmap for mem_size bytes of physical address space, places it at
 * bitmap_addr, and marks every frame USED. Callers then hand back the regions
 * the firmware reports as available, and re-reserve whatever is already spoken
 * for. Starting fully-used means a gap in the memory map costs unusable RAM
 * rather than handing out memory that belongs to something else. */
void pmm_init(uint32_t mem_size, uint32_t bitmap_addr);

/* Marks a region allocatable. Rounds the base UP and the end DOWN, so only
 * frames lying entirely inside the region are freed. */
void pmm_free_region(uint32_t base, uint32_t length);

/* Marks a region used. Rounds the base DOWN and the end UP, so any frame the
 * region touches even partially is protected. Must run AFTER pmm_free_region
 * for overlapping ranges -- the kernel sits inside a region the firmware
 * reports as available, so freeing would otherwise undo the reservation. */
void pmm_reserve_region(uint32_t base, uint32_t length);

/* Returns the physical address of a 4 KiB frame, or NULL when none is free.
 * NULL is unambiguous because frame 0 lives in the reserved first megabyte and
 * is therefore never allocatable. */
void *pmm_alloc_block(void);

/* Returns a frame to the pool. Ignores addresses outside tracked memory and
 * frames that are already free, so a double free cannot corrupt the counters. */
void pmm_free_block(void *addr);

/* Exclusive end address of the highest frame currently marked used, or 0 if
 * none is. Paging needs this to size an identity map that covers everything the
 * kernel must still be able to reach once translation is on. */
uint32_t pmm_highest_used_address(void);

uint32_t pmm_total_blocks(void);
uint32_t pmm_used_blocks(void);
uint32_t pmm_free_blocks(void);

/* Bytes occupied by the bitmap itself, valid after pmm_init. The caller needs
 * this to reserve the bitmap's own frames. */
uint32_t pmm_bitmap_size(void);

#endif /* MM_PMM_H */
