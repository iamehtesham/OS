#ifndef MM_PMM_H
#define MM_PMM_H

#include <stdbool.h>
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

/* Every frame carries a reference count as well as a bitmap bit, because a
 * frame can now be mapped into more than one address space at a time.
 *
 *   0            free
 *   1 .. 254     allocated, held by that many references
 *   PMM_PINNED   reserved firmware, kernel or loader memory
 *
 * Pinned is a sentinel rather than a large count: reserved memory was never
 * allocated, so a reference to it must never be able to count down to zero and
 * hand the BIOS data area or a boot module to the next allocation. Decrementing
 * a pinned frame is a no-op. */
#define PMM_PINNED 255u

/* Returns the physical address of a 4 KiB frame with a reference count of one,
 * or NULL when none is free. NULL is unambiguous because frame 0 lives in the
 * reserved first megabyte and is therefore never allocatable. */
void *pmm_alloc_block(void);

/* Drops one reference. The frame returns to the pool only when the last one
 * goes. Ignores NULL, addresses outside tracked memory, already-free frames
 * and pinned frames, so a double free cannot corrupt the counters. */
void pmm_free_block(void *addr);

/* Takes one more reference to an already-allocated frame, for a second address
 * space mapping it. Returns false if the frame is free (referencing nothing) or
 * already at the maximum -- a count that wrapped to zero would free a frame
 * that is still mapped, which is worse than refusing to share it. */
bool pmm_ref_block(void *addr);

/* How many references a frame carries, or 0 if it is free. PMM_PINNED for
 * reserved memory. */
uint32_t pmm_ref_count(void *addr);

/* Exclusive end address of the highest frame currently marked used, or 0 if
 * none is. Paging needs this to size an identity map that covers everything the
 * kernel must still be able to reach once translation is on. */
uint32_t pmm_highest_used_address(void);

uint32_t pmm_total_blocks(void);
uint32_t pmm_used_blocks(void);
uint32_t pmm_free_blocks(void);

/* Bytes occupied by the allocator's own metadata -- the bitmap AND the
 * reference count array, which are laid out back to back at bitmap_addr. Valid
 * after pmm_init; the caller needs it to reserve those frames. */
uint32_t pmm_metadata_size(void);

#endif /* MM_PMM_H */
