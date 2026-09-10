#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/shm.h"
#include "utils/stdio.h"
#include "utils/string.h"

typedef struct {
    uint32_t id;    /* SHM_ID_NONE marks a free slot */
    uint32_t frame; /* physical address of the shared page */

    /* Who may attach: the creator, and the single process it named when it
     * created the segment. Everyone else is refused however they came by the
     * id -- being told it, guessing it, or watching it go past. */
    uint32_t owner_pid;
    uint32_t peer_pid;
} shm_segment_t;

static shm_segment_t segments[SHM_MAX_SEGMENTS];

/* Ids are handed out in sequence and never reused, for the same reason pids are
 * not: a recycled id would let a process holding a stale one attach to a
 * segment that now belongs to something else, which is a use-after-free wearing
 * a legitimate-looking number. */
static uint32_t next_id = 1;

static shm_segment_t *find_by_id(uint32_t id)
{
    if (id == SHM_ID_NONE) {
        return NULL;
    }

    for (uint32_t i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (segments[i].id == id) {
            return &segments[i];
        }
    }

    return NULL;
}

bool shm_create(uint32_t owner_pid, uint32_t peer_pid, uint32_t *out_id,
                uint32_t *out_frame)
{
    if (out_id == NULL || out_frame == NULL) {
        return false;
    }

    shm_segment_t *slot = NULL;

    for (uint32_t i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (segments[i].id == SHM_ID_NONE) {
            slot = &segments[i];
            break;
        }
    }

    if (slot == NULL) {
        return false;
    }

    void *const frame = pmm_alloc_block();

    if (frame == NULL) {
        return false;
    }

    /* The kernel writes this frame through the identity map to clear it, so it
     * has to be reachable there -- and it must be cleared, because a recycled
     * frame carries the previous owner's bytes and this one is about to be
     * readable by two ring-3 processes. */
    if (!paging_frame_is_reachable((uint32_t)(uintptr_t)frame)) {
        pmm_free_block(frame);
        return false;
    }

    kmemset(frame, 0, PAGE_SIZE);

    slot->id        = next_id++;
    slot->frame     = (uint32_t)(uintptr_t)frame;
    slot->owner_pid = owner_pid;
    slot->peer_pid  = peer_pid;

    /* The single reference from pmm_alloc_block is now the REGISTRY's, not the
     * caller's. The caller takes its own when it maps the frame, so a segment
     * whose every user has died still has one reference and cannot be handed
     * out by the allocator while its id remains valid. */
    *out_id    = slot->id;
    *out_frame = slot->frame;

    return true;
}

bool shm_attach(uint32_t id, uint32_t caller_pid, uint32_t *out_frame)
{
    shm_segment_t *const slot = find_by_id(id);

    if (slot == NULL || out_frame == NULL) {
        return false;
    }

    /* Existence is not entitlement. Without this an unrelated process reads and
     * writes every shared page by counting 1, 2, 3 -- demonstrated before the
     * check existed, with a third program lifting another pair's payload out of
     * their page and overwriting it. */
    if (caller_pid != slot->owner_pid && caller_pid != slot->peer_pid) {
        return false;
    }

    /* One reference per mapping. If this fails the frame is at its ceiling and
     * the attach is refused, rather than wrapping a count that would later free
     * a frame two processes are still reading. */
    if (!pmm_ref_block((void *)(uintptr_t)slot->frame)) {
        return false;
    }

    *out_frame = slot->frame;

    return true;
}

uint32_t shm_collect(void)
{
    uint32_t retired = 0;

    for (uint32_t i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (segments[i].id == SHM_ID_NONE) {
            continue;
        }

        /* One reference left means the registry's own: every process that had
         * this frame mapped has had its address space torn down. Nothing else
         * needs to be tracked, because the count already is the tracking. */
        if (pmm_ref_count((void *)(uintptr_t)segments[i].frame) > 1u) {
            continue;
        }

        pmm_free_block((void *)(uintptr_t)segments[i].frame);

        segments[i].id        = SHM_ID_NONE;
        segments[i].frame     = 0;
        segments[i].owner_pid = 0;
        segments[i].peer_pid  = 0;
        retired++;
    }

    return retired;
}

uint32_t shm_segment_count(void)
{
    uint32_t live = 0;

    for (uint32_t i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (segments[i].id != SHM_ID_NONE) {
            live++;
        }
    }

    return live;
}
