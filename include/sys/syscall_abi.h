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

/* Longest string sys_print will copy out of user memory. A user pointer is not
 * trusted to be terminated, so the length has to be bounded by something. */
#define SYSCALL_MAX_STRING 256u

#endif /* SYS_SYSCALL_ABI_H */
