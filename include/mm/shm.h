#ifndef MM_SHM_H
#define MM_SHM_H

#include <stdbool.h>
#include <stdint.h>

#include "sys/syscall_abi.h"

/* The shared memory registry: a small table mapping an integer id to a set of
 * physical frames, so two processes in different address spaces can name the
 * same memory without either one being able to name a physical address.
 *
 * An id is the only thing that crosses between processes: a physical address
 * would be meaningless in the receiver's address space and would be one it
 * could invent. But an id alone is only a NAME. Possession of the integer is
 * not possession of the pages, so each segment also records who may attach to
 * it, and the id is checked against that as well as against this table.
 *
 * A segment is one or more pages, mapped contiguously in each address space
 * that attaches it but not necessarily contiguous in physical memory: the
 * frames come from the allocator one at a time, and nothing here depends on
 * where they land. */

/* Fixed, because there is no allocator to grow it and a bounded table is one
 * fewer thing a hostile process can exhaust without limit. */
#define SHM_MAX_SEGMENTS 16u

/* Never a valid id, so zero can mean "none" in a message payload. */
#define SHM_ID_NONE 0u

/* Creates a segment of `pages` zeroed frames, registered under a fresh id.
 *
 * The registry keeps a reference of its own to every frame, which is what
 * stops an id from ever naming memory the allocator has handed to somebody
 * else. On return each frame's count is exactly that one reference; the caller
 * adds its own by mapping them. Writes the frames' physical addresses, in page
 * order, to out_frames, which must hold SHM_MAX_PAGES entries. Returns false
 * if the table is full, the count is 0 or above SHM_MAX_PAGES, or memory ran
 * out -- with nothing allocated in that last case. */
bool shm_create(uint32_t owner_pid, uint32_t peer_pid, uint32_t pages, uint32_t *out_id,
                uint32_t *out_frames);

/* Resolves an id to its frames and takes one more reference on each, for a new
 * mapping. Writes the frames and the page count.
 *
 * Refuses unless the caller is the segment's creator or the one peer that
 * creator named. Checking only that an id EXISTS is not authorization: ids are
 * small sequential integers, so a process that was never given one can simply
 * count upwards and map, read and write every shared page in the system. That
 * is what this check exists to stop, and it is the difference between an id
 * being a name and being a capability. Takes no references at all on failure. */
bool shm_attach(uint32_t id, uint32_t caller_pid, uint32_t *out_frames, uint32_t *out_pages);

/* Resolves an id to its frames WITHOUT taking references, for a kernel that
 * wants to read the pages once and is done -- sys_spawn copying an image out.
 * Same entitlement rule as shm_attach: the caller must be owner or peer, so a
 * process cannot have the kernel read a segment it could not map itself. */
bool shm_frames(uint32_t id, uint32_t caller_pid, uint32_t *out_frames, uint32_t *out_pages);

/* Retires every segment no process is mapping any more.
 *
 * A segment is idle exactly when the only reference left on each of its frames
 * is the registry's own, so this needs no bookkeeping of its own and cannot
 * disagree with the truth: the reference count IS the record of who is using
 * the frame. Called after reaping dead processes, since that is what drops
 * their references. Returns how many segments were retired. */
uint32_t shm_collect(void);

uint32_t shm_segment_count(void);

#endif /* MM_SHM_H */
