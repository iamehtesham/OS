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

/* ---- output -------------------------------------------------------------- */

/* There is no print system call. Both of these are in stdio.c: they format,
 * then send the text to the console server -- which owns the screen -- as
 * MSG_PRINT_STR messages of up to 31 characters, each placed whole. Every process gets its own terminal there once focused; until
 * then its output shares the system console. Returns characters sent. */
int32_t  u_print(const char *text);
int32_t  u_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- system calls -------------------------------------------------------- */

int32_t  u_send(uint32_t target_pid, const ipc_message_t *msg);
int32_t  u_recv(ipc_message_t *msg);
void     u_yield(void);

/* Maps a physical range this task has been granted, and returns the virtual
 * address it landed at, or 0. The kernel chooses the address. */
uint32_t u_map_physical(uint32_t physical_addr, uint32_t length);

/* Asks the kernel which physical range, if any, this task may map. Returns
 * false and zeroes the struct when the task holds no grant. */
bool     u_grant_info(struct sys_grant *out);

/* Creates a shared segment of one page (or `pages`, up to SHM_MAX_PAGES) and
 * maps it here, naming the one other process allowed to attach to it. Fills in
 * the id to hand to that process, the address to use in this one and the page
 * count. A peer of 0 makes the segment private to the creator. */
bool     u_shm_map(struct sys_shm *out, uint32_t peer_pid);
bool     u_shm_map_pages(struct sys_shm *out, uint32_t peer_pid, uint32_t pages);

/* Maps a shared segment created elsewhere, and returns its address IN THIS
 * process, which will not be the address the creator sees. Zero on failure.
 * The _info form fills in the page count too, from the kernel -- the one
 * source a server may take a segment's size from. */
uint32_t u_shm_attach(uint32_t id);
bool     u_shm_attach_info(struct sys_shm *segment);

/* Asks the kernel to start the ELF image in a shared segment this process
 * owns or is peer to, `length` bytes of it, as a new process. Returns the
 * child's pid, or negative if the kernel refused: not entitled, too long, or
 * not a valid program. */
int32_t  u_spawn(uint32_t shm_id, uint32_t length);

/* The pid that spawned `pid`, or 0. */
uint32_t u_parent_of(uint32_t pid);

/* Ends this process with an exit status. Does not return. */
void     u_exit(int32_t status) __attribute__((noreturn));

/* Waits for a child to exit, writes its status through `status` (unless null),
 * and returns the child's pid, or -1 if it is not a living child of ours. */
int32_t  u_waitpid(uint32_t pid, int32_t *status);

/* Asks the kernel to raise this task's IOPL to 3. True on success, after which
 * in/out instructions execute instead of faulting. Refused for every task the
 * kernel did not start as a device driver. */
bool     u_grant_io(void);

/* Re-opens an IRQ line the kernel masked when it forwarded an interrupt to
 * this task. Call it once the device has been serviced -- the line stays shut
 * until then. Refused unless this task is the one the kernel routes the line
 * to. */
bool     u_unmask_irq(uint32_t irq);

/* Allocates `pages` physically contiguous, zeroed frames for DMA, maps them
 * into this process (writable), writes the physical address of the first to
 * *phys_out, and returns the virtual address, or 0. The physical address is
 * what a device's DMA engine needs. Driver-only. The buffer is pinned and
 * never reclaimed. */
uint32_t u_alloc_dma(uint32_t pages, uint32_t *phys_out);

/* Routes an IRQ line this process discovered (a PCI device's) to itself, so
 * the kernel forwards it here like any driver line. True on success. Driver-
 * only, and refused for the timer, the keyboard, or an owned line. */
bool     u_claim_irq(uint32_t irq);

/* Names the one sender whose messages to this process get a reserved mailbox
 * slot, drained ahead of the ordinary one. A server trusts the driver that
 * feeds it; an application trusts the server it takes input from. Without it
 * any process that knows this one's pid can keep its ordinary slot full and
 * starve the sender that matters. Zero clears it. */
bool     u_trust_sender(uint32_t pid);

/* Maps the VGA text buffer into this process, writable, and returns where; 0
 * unless the kernel started this process as the console server. */
uint32_t u_map_hw_buffer(void);

/* Reads a chunk of the kernel log; see struct sys_klog for the in/out fields.
 * False only if the request itself was unreadable. */
bool     u_klog_read(struct sys_klog *request);

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
