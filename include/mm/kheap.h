#ifndef MM_KHEAP_H
#define MM_KHEAP_H

#include <stdbool.h>
#include <stdint.h>

/* Payload alignment. The constraint allows 4 or 8; 8 is what uint64_t and
 * double want on i386, so it is the safe general guarantee to hand callers. */
#define KHEAP_ALIGNMENT 8u

/* Where the heap lives in virtual memory, and how big it is. The frames behind
 * it come from the PMM one at a time and need not be physically contiguous --
 * paging is what makes the region look like one flat run. */
#define KHEAP_VIRTUAL_BASE 0xC0000000u
#define KHEAP_SIZE         (1024u * 1024u) /* 1 MiB */

/* Marks a live header. kfree has to trust a caller-supplied pointer, and
 * without this a stray pointer would silently corrupt the list rather than
 * produce a diagnosable error. */
#define KHEAP_MAGIC 0x4B48454Du /* "KHEM" */

struct kheap_block {
    uint32_t            magic;
    uint32_t            size; /* payload bytes, NOT counting this header */
    struct kheap_block *next; /* address-ordered, so neighbours are adjacent */
    uint8_t             is_free;
    /* three bytes of tail padding bring sizeof to 16 */
};

/* The whole alignment scheme rests on the header being a whole number of
 * alignment units: payload = block + sizeof(header), and the next header sits
 * at payload + a rounded-up size. Add a field carelessly and this fails the
 * build instead of silently drifting every payload out of alignment. */
_Static_assert(sizeof(struct kheap_block) % KHEAP_ALIGNMENT == 0,
               "kheap header size must be a multiple of KHEAP_ALIGNMENT");

/* Maps KHEAP_SIZE bytes at KHEAP_VIRTUAL_BASE and lays down one free block
 * spanning all of it. Returns false if frames or page tables ran out. */
bool kheap_init(void);

/* First fit. Returns an 8-byte aligned pointer, or NULL if no block is large
 * enough (or size is 0, which is treated as a caller bug). */
void *kmalloc(uint32_t size);

/* Releases a pointer from kmalloc and merges with adjacent free neighbours.
 * NULL is a no-op; anything that is not a live heap pointer is reported and
 * ignored rather than corrupting the list. */
void kfree(void *ptr);

uint32_t kheap_total_bytes(void);
uint32_t kheap_free_bytes(void);
uint32_t kheap_used_bytes(void);
uint32_t kheap_largest_free_block(void);
uint32_t kheap_block_count(void);

#endif /* MM_KHEAP_H */
