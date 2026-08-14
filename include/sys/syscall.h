#ifndef SYS_SYSCALL_H
#define SYS_SYSCALL_H

#include <stdint.h>

#include "cpu/isr.h"

/* Call numbers, passed in EAX. */
#define SYS_PRINT 1u

/* Longest string sys_print will copy out of user memory. A user pointer is not
 * trusted to be terminated, so the length has to be bounded by something. */
#define SYSCALL_MAX_STRING 256u

/* Entry point reached from the int 0x80 stub. Reads the call number from
 * regs->eax and its argument from regs->ebx, and writes the result back into
 * regs->eax -- popa restores it, so the value lands in the caller's EAX. */
void syscall_handler(struct registers *regs);

uint32_t syscall_count(void);

/* Defined in syscall_stub.S. */
extern void isr128(void);

#endif /* SYS_SYSCALL_H */
