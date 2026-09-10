#ifndef USER_ULIB_H
#define USER_ULIB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ipc/ipc.h"
#include "sys/syscall_abi.h"

/* The runtime every ring-3 program links against.
 *
 * There is no libc, so this is all of it: thin wrappers over int 0x80 and the
 * handful of string helpers a freestanding program cannot do without. It is
 * linked into each program's own ELF rather than shared, because there is no
 * dynamic linker and each process has its own address space. */

/* ---- system calls -------------------------------------------------------- */

int32_t  u_print(const char *text);
int32_t  u_send(uint32_t target_pid, const ipc_message_t *msg);
int32_t  u_recv(ipc_message_t *msg);
void     u_yield(void);

/* Maps a physical range this task has been granted, and returns the virtual
 * address it landed at, or 0. The kernel chooses the address. */
uint32_t u_map_physical(uint32_t physical_addr, uint32_t length);

/* Asks the kernel which physical range, if any, this task may map. Returns
 * false and zeroes the struct when the task holds no grant. */
bool     u_grant_info(struct sys_grant *out);

/* Creates a shared page and maps it here, naming the one other process allowed
 * to attach to it. Fills in the id to hand to that process and the address to
 * use in this one. A peer of 0 makes the segment private to the creator. */
bool     u_shm_map(struct sys_shm *out, uint32_t peer_pid);

/* Maps a shared page created elsewhere, and returns its address IN THIS
 * process, which will not be the address the creator sees. Zero on failure. */
uint32_t u_shm_attach(uint32_t id);

/* Sends, retrying while the target's single mailbox slot is still full,
 * yielding between attempts and giving up after `attempts` of them.
 *
 * The bound is the point. An unbounded retry stakes the caller's own liveness
 * on the receiver draining its mailbox, which for a server answering an
 * untrusted client means one client can stop the service for everyone. */
int32_t  u_send_bounded(uint32_t target_pid, const ipc_message_t *msg, uint32_t attempts);

/* ---- strings and memory -------------------------------------------------- */

size_t u_strlen(const char *s);
int    u_strcmp(const char *a, const char *b);
void   u_strncpy(char *dest, const char *src, size_t size);
void  *u_memcpy(void *dest, const void *src, size_t n);
void  *u_memset(void *dest, int value, size_t n);

/* ---- building output lines ----------------------------------------------- */

/* Each returns the new end of the string. None terminates: the caller writes
 * the NUL once, after the last append.
 *
 * `limit` is the last byte that may be written, i.e. buffer + sizeof(buffer) -
 * 1, leaving room for that NUL. Every one of these stops there and silently
 * truncates. The bound is not optional politeness: the unbounded version of
 * u_append is what let a directory listing of ordinary filenames run off the
 * end of a static buffer and overwrite the pointer that lived after it. */
char *u_append(char *out, const char *limit, const char *text);
char *u_append_dec(char *out, const char *limit, uint32_t value);
char *u_append_hex(char *out, const char *limit, uint32_t value);

/* The last writable byte of an array, for passing as `limit`. */
#define U_LIMIT(buffer) ((buffer) + sizeof(buffer) - 1u)

/* Reads a little-endian word out of a message payload, and writes one in.
 * Spelled out rather than cast, because a payload is a byte array and may sit
 * at any alignment. */
uint32_t u_load32(const uint8_t *from);
void     u_store32(uint8_t *to, uint32_t value);

/* Busy-waits. hlt is privileged, so a ring-3 program has no other way to pace
 * itself; the timer still preempts this. */
void u_spin(uint32_t iterations);

#endif /* USER_ULIB_H */
