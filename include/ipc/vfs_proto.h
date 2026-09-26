#ifndef IPC_VFS_PROTO_H
#define IPC_VFS_PROTO_H

#include <stdint.h>

#include "ipc/ipc.h"

/* The wire protocol between a client and the VFS server.
 *
 * Both sides are ring-3 ELF binaries in address spaces of their own, so this
 * header is the entire contract between them: nothing else crosses. Every
 * request and reply fits the fixed 32-byte IPC payload, which is what lets the
 * kernel copy a message without taking a length from either party. */

/* The server's pid is fixed by creation order: the kernel starts it first, so
 * it always follows the kernel idle task's 0. A client has to be able to name
 * the server before it has spoken to anything, which is why this is a
 * compile-time constant rather than something discovered at run time. */
#define VFS_SERVER_PID 1u

#define MSG_OPEN         10u /* data = filename, NUL terminated              */
#define MSG_OPEN_SUCCESS 11u /* data = { u32 length, u32 inode }             */
#define MSG_OPEN_FAIL    12u /* data = { u32 reason }                        */
#define MSG_READ         13u /* data = { u32 inode, u32 offset }             */
#define MSG_READ_REPLY   14u /* data = { u8 count, bytes... }                */
#define MSG_READ_FAIL    15u /* data = { u32 reason }                        */

/* Loading a whole file into a shared segment, for SYS_SPAWN. The client
 * creates the segment, names this server as the peer, and sends the id: the
 * server attaches, copies the file to the start of the segment, and replies
 * with how many bytes it wrote. The kernel enforces entitlement on the attach,
 * so a client cannot have the server write into a segment it does not own, and
 * the server takes the segment's size from the kernel, never from the client,
 * so a client cannot have it write past the end. */
#define MSG_LOAD       16u /* data = { u32 inode, u32 shm id }              */
#define MSG_LOAD_REPLY 17u /* data = { u32 bytes copied }                   */
#define MSG_LOAD_FAIL  18u /* data = { u32 reason }                         */

/* Directory listing, one entry per round trip. */
#define MSG_LIST       19u /* data = { u32 index }                          */
#define MSG_LIST_REPLY 20u /* data = name, NUL terminated (truncated to fit) */
#define MSG_LIST_END   21u /* no such index: the listing is over            */

/* Why an open failed. Carried as a word so a client can report the difference
 * between "no such file" and "you asked me something I cannot parse". */
#define VFS_ERR_NOT_FOUND  1u
#define VFS_ERR_BAD_NAME   2u
#define VFS_ERR_NOT_MOUNTED 3u
#define VFS_ERR_BAD_REQUEST 4u
#define VFS_ERR_NO_ACCESS   5u /* the segment could not be attached           */
#define VFS_ERR_TOO_BIG     6u /* the file does not fit the segment           */

/* A filename has to fit the payload with room for its terminator. Shorter than
 * the VFS's own name field, so the server refuses a request it could not have
 * represented rather than silently truncating one. */
#define VFS_MSG_NAME_MAX (IPC_PAYLOAD_SIZE - 1u)

/* Bytes a single MSG_READ_REPLY can carry: the payload less the count byte. */
#define VFS_MSG_READ_MAX (IPC_PAYLOAD_SIZE - 1u)

#endif /* IPC_VFS_PROTO_H */
