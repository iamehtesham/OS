#ifndef IPC_SHM_PROTO_H
#define IPC_SHM_PROTO_H

#include <stdint.h>

#include "ipc/ipc.h"

/* The contract between the two shared-memory demo processes.
 *
 * What crosses between them is an id, four bytes, and nothing else. The page
 * itself never travels: both sides map the same physical frame, at different
 * virtual addresses, and the messages exist only to name it and to say when
 * each side has finished writing. */

/* Fixed by creation order, like the VFS server's. The reader is started first
 * so it is already blocked in recv when the writer sends. The kernel checks
 * this rather than assuming it. */
#define SHM_READER_PID 3u

#define MSG_SHM_OFFER 20u /* writer -> reader: data = { u32 shm id }  */
#define MSG_SHM_REPLY 21u /* reader -> writer: the reply is in the page */

/* Where each side writes inside the shared page. Two disjoint offsets, so each
 * process can prove it sees the other's bytes without either overwriting what
 * it is trying to read. */
#define SHM_WRITER_OFFSET 0u
#define SHM_READER_OFFSET 64u

#define SHM_WRITER_TEXT "ZERO-COPY-SUCCESS"
#define SHM_READER_TEXT "READER-WROTE-BACK"

#endif /* IPC_SHM_PROTO_H */
