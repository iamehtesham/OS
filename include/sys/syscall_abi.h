#ifndef SYS_SYSCALL_ABI_H
#define SYS_SYSCALL_ABI_H

/* The system call ABI, and nothing else.
 *
 * This header is included by the kernel AND by standalone ring-3 programs
 * compiled as separate ELF binaries, so it must stay free of kernel types and
 * declarations -- a user program that included the kernel's headers would be
 * one edit away from calling into code it cannot reach.
 *
 * Call number in EAX, arguments in EBX then ECX, result back in EAX. */

#define SYS_PRINT 1u /* ebx = const char *              -> chars written    */
#define SYS_SEND  2u /* ebx = target pid, ecx = msg *   -> IPC_OK or error  */
#define SYS_RECV  3u /* ebx = msg *; blocks until one arrives               */
#define SYS_YIELD 4u /* give up the rest of this timeslice                  */

/* ebx = physical address, ecx = length -> the virtual address it was mapped
 * at, or 0. Refused unless the caller holds a grant covering the whole range;
 * see task_grant_physical. The kernel chooses the virtual address. */
#define SYS_MAP_PHYSICAL 5u

/* ebx = struct sys_grant *; fills in the physical range this task is allowed
 * to map, or zeroes it. A server cannot guess where the boot loader put its
 * data, and must not be told by another user program. */
#define SYS_GRANT_INFO 6u

/* ebx = struct sys_shm *, ecx = the one other pid allowed to attach (0 for
 * none); creates a shared page, maps it into the caller, and fills in the id to
 * hand to that process and the address to use here. Returns 0 or -1. */
#define SYS_SHM_MAP 7u

/* ebx = an id from SYS_SHM_MAP -> the address the same page is now mapped at
 * in THIS process, or 0. The two processes see one page at two addresses;
 * nothing is copied. */
#define SYS_SHM_ATTACH 8u

/* What SYS_GRANT_INFO writes. Shared between the kernel and ring 3, so its
 * layout is part of the ABI. */
struct sys_grant {
    unsigned int base;   /* physical address, exactly as the loader reported */
    unsigned int length; /* bytes                                            */
};

/* What SYS_SHM_MAP writes. The id is the only part safe to send to another
 * process: an address means nothing outside the address space that produced
 * it, and a physical address would be a capability a receiver could invent. */
struct sys_shm {
    unsigned int id;
    unsigned int vaddr;
};

/* Longest string sys_print will copy out of user memory. A user pointer is not
 * trusted to be terminated, so the length has to be bounded by something. */
#define SYSCALL_MAX_STRING 256u

#endif /* SYS_SYSCALL_ABI_H */
