#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mm/kheap.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "utils/stdio.h"

#define HEADER_SIZE ((uint32_t)sizeof(struct kheap_block))

static struct kheap_block *heap_head;

/* Rounds up to the next multiple of KHEAP_ALIGNMENT. Adding ALIGNMENT-1 carries
 * into the next multiple and the mask clears the low bits; this only works
 * because the alignment is a power of two. */
static uint32_t align_up(uint32_t value)
{
    return (value + KHEAP_ALIGNMENT - 1u) & ~(KHEAP_ALIGNMENT - 1u);
}

static void *payload_of(struct kheap_block *block)
{
    return (uint8_t *)block + HEADER_SIZE;
}

bool kheap_init(void)
{
    /* Back every page of the region with a frame. The frames are whatever the
     * PMM hands out and are not contiguous; paging is what makes the range look
     * like one flat run to everything above. */
    for (uint32_t offset = 0; offset < KHEAP_SIZE; offset += PAGE_SIZE) {
        void *const frame = pmm_alloc_block();

        if (frame == NULL) {
            kprintf("kheap: out of physical frames at offset 0x%x\n", offset);
            return false;
        }

        if (!map_page((uint32_t)(uintptr_t)frame, KHEAP_VIRTUAL_BASE + offset,
                      PAGE_WRITABLE)) {
            kprintf("kheap: map_page failed at 0x%x\n", KHEAP_VIRTUAL_BASE + offset);
            pmm_free_block(frame);
            return false;
        }
    }

    /* One free block spanning everything except its own header. */
    heap_head          = (struct kheap_block *)(uintptr_t)KHEAP_VIRTUAL_BASE;
    heap_head->magic   = KHEAP_MAGIC;
    heap_head->size    = KHEAP_SIZE - HEADER_SIZE;
    heap_head->next    = NULL;
    heap_head->is_free = 1;

    return true;
}

void *kmalloc(uint32_t size)
{
    if (heap_head == NULL || size == 0) {
        return NULL;
    }

    /* Guard the rounding itself: near the top of the range, size + 7 wraps. */
    if (size > UINT32_MAX - (KHEAP_ALIGNMENT - 1u)) {
        return NULL;
    }

    const uint32_t needed = align_up(size);

    for (struct kheap_block *block = heap_head; block != NULL; block = block->next) {
        if (block->magic != KHEAP_MAGIC) {
            kprintf("kmalloc: corrupt header at %p\n", (void *)block);
            return NULL;
        }

        if (!block->is_free || block->size < needed) {
            continue;
        }

        /* Split only when the remainder can carry a header AND a payload worth
         * having. Below that threshold the whole block is handed over, which is
         * also what makes an exact fit work without a special case: there is
         * simply nothing left to split. */
        if (block->size >= needed + HEADER_SIZE + KHEAP_ALIGNMENT) {
            struct kheap_block *const rest =
                (struct kheap_block *)((uint8_t *)block + HEADER_SIZE + needed);

            rest->magic   = KHEAP_MAGIC;
            rest->size    = block->size - needed - HEADER_SIZE;
            rest->next    = block->next;
            rest->is_free = 1;

            block->size = needed;
            block->next = rest;
        }

        block->is_free = 0;

        return payload_of(block);
    }

    return NULL;
}

void kfree(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    const uint32_t address = (uint32_t)(uintptr_t)ptr;

    /* Range-check before dereferencing. The header lives just below the
     * pointer, so reading it blind would fault on a wild address instead of
     * reporting one. */
    if (address < KHEAP_VIRTUAL_BASE + HEADER_SIZE ||
        address >= KHEAP_VIRTUAL_BASE + KHEAP_SIZE) {
        kprintf("kfree: %p is not inside the heap\n", ptr);
        return;
    }

    struct kheap_block *const block =
        (struct kheap_block *)((uint8_t *)ptr - HEADER_SIZE);

    if (block->magic != KHEAP_MAGIC) {
        kprintf("kfree: %p has no valid header\n", ptr);
        return;
    }

    if (block->is_free) {
        kprintf("kfree: %p freed twice\n", ptr);
        return;
    }

    /* Walking for the predecessor also settles whether this header is in the
     * list at all, and that answer must not be discarded. The walk can end two
     * ways -- by finding the block, or by running out -- and on the second exit
     * `prev` is merely the last node, unrelated to ptr. Merging against it
     * would splice two blocks that are not neighbours and hand the resulting
     * oversized block back out.
     *
     * The magic check above cannot stand in for this. It reads whatever sits
     * below the pointer, so a payload that happens to contain the magic word
     * reads as a live header; only membership proves otherwise. This has to
     * settle before the write below, which would otherwise land in caller
     * memory. */
    struct kheap_block *prev  = NULL;
    bool                found = false;

    for (struct kheap_block *scan = heap_head; scan != NULL; scan = scan->next) {
        if (scan == block) {
            found = true;
            break;
        }

        prev = scan;
    }

    if (!found) {
        kprintf("kfree: %p is not a live block\n", ptr);
        return;
    }

    block->is_free = 1;

    /* Merge forward first, then backward, so three adjacent free blocks
     * collapse into one in a single call. Blocks are address-ordered, so a
     * neighbour in the list is a neighbour in memory and the merged size is
     * simply both payloads plus the header that no longer separates them. */
    if (block->next != NULL && block->next->is_free) {
        block->size += HEADER_SIZE + block->next->size;
        block->next = block->next->next;
    }

    if (prev != NULL && prev->is_free) {
        prev->size += HEADER_SIZE + block->size;
        prev->next = block->next;
    }
}

uint32_t kheap_total_bytes(void)
{
    return KHEAP_SIZE;
}

uint32_t kheap_free_bytes(void)
{
    uint32_t total = 0;

    for (struct kheap_block *block = heap_head; block != NULL; block = block->next) {
        if (block->is_free) {
            total += block->size;
        }
    }

    return total;
}

uint32_t kheap_used_bytes(void)
{
    uint32_t total = 0;

    for (struct kheap_block *block = heap_head; block != NULL; block = block->next) {
        if (!block->is_free) {
            total += block->size;
        }
    }

    return total;
}

uint32_t kheap_largest_free_block(void)
{
    uint32_t largest = 0;

    for (struct kheap_block *block = heap_head; block != NULL; block = block->next) {
        if (block->is_free && block->size > largest) {
            largest = block->size;
        }
    }

    return largest;
}

uint32_t kheap_block_count(void)
{
    uint32_t count = 0;

    for (struct kheap_block *block = heap_head; block != NULL; block = block->next) {
        count++;
    }

    return count;
}
