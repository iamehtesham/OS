#ifndef SYS_SYSCALL_H
#define SYS_SYSCALL_H

#include <stdint.h>

#include "cpu/isr.h"

/* The call numbers and their argument registers. Kept in their own header
 * because standalone ring-3 binaries include them too, and those must not pull
 * in kernel declarations. */
#include "sys/syscall_abi.h"

/* Entry point reached from the int 0x80 stub. Reads the call number from
 * regs->eax and its argument from regs->ebx, and writes the result back into
 * regs->eax -- popa restores it, so the value lands in the caller's EAX. */
void syscall_handler(struct registers *regs);

uint32_t syscall_count(void);

/* Defined in syscall_stub.S. */
extern void isr128(void);

#endif /* SYS_SYSCALL_H */
