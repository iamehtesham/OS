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

/* Why an open failed. Carried as a word so a client can report the difference
 * between "no such file" and "you asked me something I cannot parse". */
#define VFS_ERR_NOT_FOUND  1u
#define VFS_ERR_BAD_NAME   2u
#define VFS_ERR_NOT_MOUNTED 3u
#define VFS_ERR_BAD_REQUEST 4u

/* A filename has to fit the payload with room for its terminator. Shorter than
 * the VFS's own name field, so the server refuses a request it could not have
 * represented rather than silently truncating one. */
#define VFS_MSG_NAME_MAX (IPC_PAYLOAD_SIZE - 1u)

/* Bytes a single MSG_READ_REPLY can carry: the payload less the count byte. */
#define VFS_MSG_READ_MAX (IPC_PAYLOAD_SIZE - 1u)

#endif /* IPC_VFS_PROTO_H */
