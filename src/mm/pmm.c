#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mm/pmm.h"

#define BITS_PER_WORD 32u

static uint32_t *bitmap;
static uint32_t  bitmap_words;
static uint32_t  total_blocks;
static uint32_t  used_blocks;

static void bit_set(uint32_t frame)
{
    bitmap[frame / BITS_PER_WORD] |= (uint32_t)1u << (frame % BITS_PER_WORD);
}

static void bit_clear(uint32_t frame)
{
    bitmap[frame / BITS_PER_WORD] &= ~((uint32_t)1u << (frame % BITS_PER_WORD));
}

static bool bit_test(uint32_t frame)
{
    return (bitmap[frame / BITS_PER_WORD] & ((uint32_t)1u << (frame % BITS_PER_WORD))) != 0;
}

/* Returns the lowest free frame, or -1 when none is left. The frame index fits
 * comfortably in int32_t: 4 GiB of address space is only 0xFFFFF frames. */
static int32_t first_free_frame(void)
{
    for (uint32_t word = 0; word < bitmap_words; word++) {
        /* An all-ones word has no free frame; skipping it whole is what keeps
         * the scan cheap once memory fills up. */
        if (bitmap[word] == 0xFFFFFFFFu) {
            continue;
        }

        for (uint32_t bit = 0; bit < BITS_PER_WORD; bit++) {
            const uint32_t frame = word * BITS_PER_WORD + bit;

            if (frame >= total_blocks) {
                return -1;
            }

            if ((bitmap[word] & ((uint32_t)1u << bit)) == 0) {
                return (int32_t)frame;
            }
        }
    }

    return -1;
}

void pmm_init(uint32_t mem_size, uint32_t bitmap_addr)
{
    bitmap       = (uint32_t *)(uintptr_t)bitmap_addr;
    total_blocks = mem_size >> PMM_BLOCK_SHIFT;
    bitmap_words = (total_blocks + BITS_PER_WORD - 1) / BITS_PER_WORD;

    /* Every frame starts used, including the padding bits past total_blocks in
     * the final word -- leaving those clear would let the scan return frames
     * beyond the end of real memory. */
    for (uint32_t word = 0; word < bitmap_words; word++) {
        bitmap[word] = 0xFFFFFFFFu;
    }

    used_blocks = total_blocks;
}

void pmm_free_region(uint32_t base, uint32_t length)
{
    /* Reject a range that would wrap the address space rather than compute
     * nonsense frame numbers from it. Costs at most the topmost frame. */
    if (length == 0 || base + length < base) {
        return;
    }

    /* Convert to frame indices before rounding so the arithmetic cannot
     * overflow: both operands have already been shifted down by 4096.
     * Base rounds UP and the end rounds DOWN, so a frame straddling either
     * boundary stays used. */
    const uint32_t partial     = (base & (PMM_BLOCK_SIZE - 1u)) ? 1u : 0u;
    const uint32_t first_frame = (base >> PMM_BLOCK_SHIFT) + partial;

    /* Exclusive, so the end truly rounds DOWN: a frame holding the region's
     * final partial bytes is left used. Deriving this from the region's last
     * BYTE instead -- the way pmm_reserve_region correctly does -- would round
     * the end UP and hand out a frame that is only partly available. */
    const uint32_t end_frame = (base + length) >> PMM_BLOCK_SHIFT;

    for (uint32_t frame = first_frame; frame < end_frame && frame < total_blocks; frame++) {
        if (bit_test(frame)) {
            bit_clear(frame);
            used_blocks--;
        }
    }
}

void pmm_reserve_region(uint32_t base, uint32_t length)
{
    if (length == 0 || base + length < base) {
        return;
    }

    /* Opposite rounding to pmm_free_region: the base rounds DOWN and the
     * inclusive last frame rounds UP, so every frame the region touches even
     * partially is marked used. */
    const uint32_t first_frame = base >> PMM_BLOCK_SHIFT;
    const uint32_t last_frame  = (base + length - 1u) >> PMM_BLOCK_SHIFT;

    for (uint32_t frame = first_frame; frame <= last_frame && frame < total_blocks; frame++) {
        if (!bit_test(frame)) {
            bit_set(frame);
            used_blocks++;
        }
    }
}

void *pmm_alloc_block(void)
{
    const int32_t frame = first_free_frame();

    if (frame < 0) {
        return NULL;
    }

    bit_set((uint32_t)frame);
    used_blocks++;

    return (void *)(uintptr_t)((uint32_t)frame << PMM_BLOCK_SHIFT);
}

void pmm_free_block(void *addr)
{
    /* NULL is this allocator's out-of-memory sentinel, so freeing a failed
     * allocation has to be a no-op -- the `p = alloc(); if (!p) goto out; ...
     * out: free(p);` idiom depends on it. Without this guard NULL maps to
     * frame 0, which the reserved low megabyte deliberately keeps out of
     * circulation; clearing its bit would put the real-mode IVT and BIOS data
     * area into the free pool and make the next allocation return NULL as a
     * success the caller cannot distinguish from failure. */
    if (addr == NULL) {
        return;
    }

    const uint32_t frame = (uint32_t)(uintptr_t)addr >> PMM_BLOCK_SHIFT;

    if (frame >= total_blocks) {
        return;
    }

    /* Freeing an already-free frame would drive used_blocks below zero and hand
     * the same frame out twice, so swallow it. */
    if (!bit_test(frame)) {
        return;
    }

    bit_clear(frame);
    used_blocks--;
}

uint32_t pmm_highest_used_address(void)
{
    for (uint32_t word = bitmap_words; word-- > 0;) {
        if (bitmap[word] == 0) {
            continue;
        }

        for (uint32_t bit = BITS_PER_WORD; bit-- > 0;) {
            const uint32_t frame = word * BITS_PER_WORD + bit;

            /* Padding bits past the end of real memory are set to 1 at init;
             * they describe no frame and must not be reported. */
            if (frame >= total_blocks) {
                continue;
            }

            if (bitmap[word] & ((uint32_t)1u << bit)) {
                /* Guard the top frame, where frame + 1 would shift out of a
                 * 32-bit address entirely. */
                if (frame + 1u >= (1u << (32u - PMM_BLOCK_SHIFT))) {
                    return 0xFFFFF000u;
                }

                return (frame + 1u) << PMM_BLOCK_SHIFT;
            }
        }
    }

    return 0;
}

uint32_t pmm_total_blocks(void)
{
    return total_blocks;
}

uint32_t pmm_used_blocks(void)
{
    return used_blocks;
}

uint32_t pmm_free_blocks(void)
{
    return total_blocks - used_blocks;
}

uint32_t pmm_bitmap_size(void)
{
    return bitmap_words * sizeof(uint32_t);
}
