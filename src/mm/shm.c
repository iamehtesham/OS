#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/shm.h"
#include "utils/string.h"

typedef struct {
    uint32_t id;                     /* SHM_ID_NONE marks a free slot */
    uint32_t frames[SHM_MAX_PAGES]; /* physical addresses, in page order */
    uint32_t page_count;

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

static bool entitled(const shm_segment_t *slot, uint32_t caller_pid)
{
    /* Existence is not entitlement. Without this an unrelated process reads and
     * writes every shared page by counting 1, 2, 3 -- demonstrated before the
     * check existed, with a third program lifting another pair's payload out of
     * their page and overwriting it. */
    return caller_pid == slot->owner_pid || caller_pid == slot->peer_pid;
}

bool shm_create(uint32_t owner_pid, uint32_t peer_pid, uint32_t pages, uint32_t *out_id,
                uint32_t *out_frames)
{
    if (out_id == NULL || out_frames == NULL || pages == 0 || pages > SHM_MAX_PAGES) {
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

    uint32_t allocated = 0;

    for (; allocated < pages; allocated++) {
        void *const frame = pmm_alloc_block();

        /* The kernel writes each frame through the identity map to clear it,
         * so it has to be reachable there -- and it must be cleared, because a
         * recycled frame carries the previous owner's bytes and this one is
         * about to be readable by two ring-3 processes. */
        if (frame == NULL || !paging_frame_is_reachable((uint32_t)(uintptr_t)frame)) {
            pmm_free_block(frame); /* a no-op for NULL */
            break;
        }

        kmemset(frame, 0, PAGE_SIZE);
        slot->frames[allocated] = (uint32_t)(uintptr_t)frame;
    }

    if (allocated < pages) {
        /* All or nothing: a half-built segment is frames nobody can name. */
        for (uint32_t i = 0; i < allocated; i++) {
            pmm_free_block((void *)(uintptr_t)slot->frames[i]);
            slot->frames[i] = 0;
        }

        return false;
    }

    slot->id         = next_id++;
    slot->page_count = pages;
    slot->owner_pid  = owner_pid;
    slot->peer_pid   = peer_pid;

    /* The single reference from pmm_alloc_block is now the REGISTRY's, not the
     * caller's. The caller takes its own when it maps the frames, so a segment
     * whose every user has died still has one reference per frame and cannot
     * be handed out by the allocator while its id remains valid. */
    *out_id = slot->id;
    kmemcpy(out_frames, slot->frames, sizeof(slot->frames));

    return true;
}

bool shm_attach(uint32_t id, uint32_t caller_pid, uint32_t *out_frames, uint32_t *out_pages)
{
    shm_segment_t *const slot = find_by_id(id);

    if (slot == NULL || out_frames == NULL || out_pages == NULL || !entitled(slot, caller_pid)) {
        return false;
    }

    /* One reference per frame per mapping. If any frame is at its ceiling the
     * whole attach is refused and the references already taken are given
     * back, rather than wrapping a count that would later free a frame two
     * processes are still reading. */
    for (uint32_t i = 0; i < slot->page_count; i++) {
        if (!pmm_ref_block((void *)(uintptr_t)slot->frames[i])) {
            for (uint32_t j = 0; j < i; j++) {
                pmm_free_block((void *)(uintptr_t)slot->frames[j]);
            }

            return false;
        }
    }

    kmemcpy(out_frames, slot->frames, sizeof(slot->frames));
    *out_pages = slot->page_count;

    return true;
}

bool shm_frames(uint32_t id, uint32_t caller_pid, uint32_t *out_frames, uint32_t *out_pages)
{
    const shm_segment_t *const slot = find_by_id(id);

    if (slot == NULL || out_frames == NULL || out_pages == NULL || !entitled(slot, caller_pid)) {
        return false;
    }

    kmemcpy(out_frames, slot->frames, sizeof(slot->frames));
    *out_pages = slot->page_count;

    return true;
}

uint32_t shm_collect(void)
{
    uint32_t retired = 0;

    for (uint32_t i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (segments[i].id == SHM_ID_NONE) {
            continue;
        }

        /* One reference left on every frame means the registry's own: every
         * process that had this segment mapped has had its address space torn
         * down. Nothing else needs to be tracked, because the count already is
         * the tracking. */
        bool idle = true;

        for (uint32_t p = 0; p < segments[i].page_count; p++) {
            if (pmm_ref_count((void *)(uintptr_t)segments[i].frames[p]) > 1u) {
                idle = false;
                break;
            }
        }

        if (!idle) {
            continue;
        }

        for (uint32_t p = 0; p < segments[i].page_count; p++) {
            pmm_free_block((void *)(uintptr_t)segments[i].frames[p]);
            segments[i].frames[p] = 0;
        }

        segments[i].id         = SHM_ID_NONE;
        segments[i].page_count = 0;
        segments[i].owner_pid  = 0;
        segments[i].peer_pid   = 0;
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
