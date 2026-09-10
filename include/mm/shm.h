#ifndef MM_SHM_H
#define MM_SHM_H

#include <stdbool.h>
#include <stdint.h>

/* The shared memory registry: a small table mapping an integer id to a physical
 * frame, so two processes in different address spaces can name the same memory
 * without either one being able to name a physical address.
 *
 * An id is the only thing that crosses between processes: a physical address
 * would be meaningless in the receiver's address space and would be one it
 * could invent. But an id alone is only a NAME. Possession of the integer is
 * not possession of the page, so each segment also records who may attach to
 * it, and the id is checked against that as well as against this table. */

/* Fixed, because there is no allocator to grow it and a bounded table is one
 * fewer thing a hostile process can exhaust without limit. */
#define SHM_MAX_SEGMENTS 16u

/* Never a valid id, so zero can mean "none" in a message payload. */
#define SHM_ID_NONE 0u

/* Creates a segment: one zeroed frame, registered under a fresh id.
 *
 * The registry keeps a reference of its own to that frame, which is what stops
 * an id from ever naming memory the allocator has handed to somebody else. On
 * return the frame's count is exactly that one reference; the caller adds its
 * own by mapping it. Returns false if the table is full or memory ran out. */
bool shm_create(uint32_t owner_pid, uint32_t peer_pid, uint32_t *out_id,
                uint32_t *out_frame);

/* Resolves an id to its frame and takes one more reference for a new mapping.
 *
 * Refuses unless the caller is the segment's creator or the one peer that
 * creator named. Checking only that an id EXISTS is not authorization: ids are
 * small sequential integers, so a process that was never given one can simply
 * count upwards and map, read and write every shared page in the system. That
 * is what this check exists to stop, and it is the difference between an id
 * being a name and being a capability. */
bool shm_attach(uint32_t id, uint32_t caller_pid, uint32_t *out_frame);

/* Retires every segment no process is mapping any more.
 *
 * A segment is idle exactly when the only reference left is the registry's own,
 * so this needs no bookkeeping of its own and cannot disagree with the truth:
 * the reference count IS the record of who is using the frame. Called after
 * reaping dead processes, since that is what drops their references. Returns
 * how many segments were retired. */
uint32_t shm_collect(void);

uint32_t shm_segment_count(void);

#endif /* MM_SHM_H */
