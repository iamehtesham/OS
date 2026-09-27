#ifndef SYS_SYSCALL_ABI_H
#define SYS_SYSCALL_ABI_H

/* The system call ABI, and nothing else.
 *
 * This header is included by the kernel AND by standalone ring-3 programs
 * compiled as separate ELF binaries, so it must stay free of kernel types and
 * declarations -- a user program that included the kernel's headers would be
 * one edit away from calling into code it cannot reach.
 *
 * Call number in EAX, arguments in EBX then ECX, result back in EAX.
 *
 * There is no print call. Text output is a message to the console server, a
 * ring-3 process; the kernel has no path from a system call to the screen. */

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
 * none), edx = pages (0 means 1, at most SHM_MAX_PAGES); creates a shared
 * segment of that many zeroed pages, maps it contiguously into the caller, and
 * fills in the id to hand to that process, the address to use here and the
 * page count. Returns 0 or -1. */
#define SYS_SHM_MAP 7u

/* ebx = struct sys_shm * with id filled in -> 0 or -1; on success vaddr is the
 * address the same segment is now mapped at in THIS process and pages its
 * size. The two processes see one segment at two addresses; nothing is
 * copied. Refused unless the caller is the segment's creator or its named
 * peer. */
#define SYS_SHM_ATTACH 8u

/* Longest shared segment, in pages. Sized for a program image: the ELF files
 * built here are three pages, and sixteen leaves room to grow. */
#define SHM_MAX_PAGES 16u

/* No arguments -> 0, or -1 if this task was not privileged for it. Sets
 * IOPL=3 in the caller's saved EFLAGS, so that from the iret onwards its
 * in/out instructions pass the CPL <= IOPL check instead of faulting. Only a
 * task the kernel marked as a device driver is allowed; see task_grant_io. */
#define SYS_GRANT_IO 9u

/* ebx = IRQ line -> 0, or -1. Re-opens a line the kernel masked when it
 * forwarded that interrupt to this task. Refused unless the caller is the
 * task the kernel routes that line to; see irq_route_to_task. */
#define SYS_UNMASK_IRQ 10u

/* ebx = pid -> 0. Names the one sender whose messages to the caller land in a
 * reserved second mailbox slot that recv drains ahead of the ordinary one, so
 * no other process can crowd that sender out by keeping the ordinary slot
 * full. Affects only the caller's own mailbox, so any task may call it; 0
 * clears the reservation. */
#define SYS_TRUST_SENDER 11u

/* No arguments -> the virtual address the VGA text buffer (physical 0xB8000)
 * is now mapped at, writable, or 0. Refused unless the kernel marked the
 * caller as the console server; see task_grant_vga. */
#define SYS_MAP_HW_BUFFER 12u

/* ebx = struct sys_klog * -> 0 or -1. Copies a chunk of the kernel log. The
 * struct is in/out: see below. */
#define SYS_KLOG_READ 13u

/* ebx = shared segment id holding an ELF image, ecx = the image's length in
 * bytes -> the new process's pid, or -1. The caller must be the segment's
 * creator or peer. The kernel copies the image out before it parses a byte,
 * refuses a length larger than the segment, and refuses anything elf_load
 * refuses: bad magic, wrong class, wrong machine, a header or segment past
 * the end, an entry outside the mapped image. Nothing runs on failure. The
 * child is an ordinary unprivileged process that records the caller as its
 * parent. */
#define SYS_SPAWN 14u

/* ebx = pid -> the pid that spawned it, or 0 for a process the kernel started
 * at boot, a pid that does not exist, or the kernel itself. Anyone may ask;
 * parentage is not a secret, and it is how the console decides which
 * terminal a spawned process prints on. */
#define SYS_PARENT_OF 15u

/* ebx = exit status, never returns. Ends the calling process: its user memory
 * is freed at once and it becomes a zombie holding the status until its parent
 * reaps it with SYS_WAITPID, or -- if it is an orphan -- until the kernel does.
 * A fault ends a process the same way, with a status of TASK_EXIT_FAULT_BASE
 * ORed with the trap vector. */
#define SYS_EXIT 16u

/* ebx = a child's pid, ecx = int * (or 0) -> that pid, or -1. Blocks until the
 * named child has exited, then frees its corpse and writes its exit status to
 * *ecx. Refused with -1 unless the pid is a child of the caller that still
 * exists. Returns as soon as the child exits, whether cleanly or by fault. */
#define SYS_WAITPID 17u

/* ebx = page count, ecx = uint32_t * -> the virtual address a run of that many
 * PHYSICALLY CONTIGUOUS, zeroed frames is mapped at (writable), with the
 * physical address of the first written to *ecx; 0 on failure. For a device
 * that DMAs across a contiguous buffer. The frames are pinned, so they are
 * never reclaimed -- a device keeps a physical pointer the kernel cannot
 * revoke. Refused unless the caller is a device driver (may_use_io). */
#define SYS_ALLOC_DMA 18u

/* ebx = IRQ line -> 0, or -1. Routes a line the caller discovered at run time
 * (a PCI device's IRQ) to itself, installing a kernel forwarder and opening
 * the line, exactly as the keyboard's line was wired at boot. Refused for the
 * timer, the keyboard, the cascade (IRQ 2), an already-owned line, or a
 * non-driver caller. */
#define SYS_CLAIM_IRQ 19u

/* ebx = ticks from now -> 0, or -1. Arms this task's one alarm: when that many
 * timer ticks have passed, the task's next recv returns a MSG_TIMER from the
 * kernel (IPC_KERNEL_PID), ahead of any interrupt or message. 0 cancels. Arming
 * or cancelling also discards an expiry that fired but was never collected, so
 * a cancelled alarm can never arrive late. Refused, changing nothing, for 2^31
 * ticks or more -- the deadline is compared by signed difference, and a longer
 * one would look already past. One alarm per task; arming replaces it. */
#define SYS_ALARM 20u

/* -> the number of timer ticks since boot (SYS_ALARM_HZ a second), wrapping at
 * 2^32 -- compare two readings by their signed difference. The clock alarms
 * count in: a program can ask whether an alarm it armed is due without waiting
 * in recv for it, and measure how long something took. */
#define SYS_TICKS 21u

/* The tick rate an alarm counts in. The kernel programs the timer from this
 * constant, so the number ring 3 uses to turn seconds into ticks and the rate
 * the hardware runs at cannot drift apart -- to within the PIT's whole-number
 * divisor: 1193182 / 11931 is 100.007 Hz, so a tick is 69 parts per million
 * short of 10 ms, and an alarm ends that much early (a day's lease, six seconds
 * early -- the safe side of a lease). */
#define SYS_ALARM_HZ 100u
#define SYS_ALARM_MAX_TICKS 0x7FFFFFFFu

/* Largest DMA buffer, in pages. An RTL8139 RX ring is 8 KiB plus slack; three
 * pages covers it, and this leaves generous room above that. */
#define DMA_MAX_PAGES 16u

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
    unsigned int pages; /* written by both calls; never read from the caller */
};

/* What SYS_KLOG_READ takes and gives back. In: offset is how far the caller
 * has read, buffer and length describe where the next bytes may go. Out:
 * offset is where the delivered bytes actually start (later than requested if
 * the log wrapped past the caller), length is how many were delivered; 0 at
 * the end. The caller's next offset is offset + length. */
struct sys_klog {
    unsigned int offset;
    unsigned int buffer;
    unsigned int length;
};

#endif /* SYS_SYSCALL_ABI_H */
