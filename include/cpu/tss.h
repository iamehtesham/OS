#ifndef CPU_TSS_H
#define CPU_TSS_H

#include <stdint.h>

/* The 32-bit Task State Segment (Intel SDM Vol. 3A, section 7.2.1).
 *
 * Software task switching uses almost none of this. What matters is ss0:esp0:
 * when an interrupt crosses from ring 3 to ring 0 the CPU has no register
 * saying where the kernel stack is, so it reads that pair out of the TSS,
 * switches to it, and only then starts pushing the interrupt frame. Leave them
 * wrong and the very first push faults with no valid stack to report it on --
 * a double fault, then a triple fault, then a reset. */
struct tss_entry {
    uint32_t prev_tss; /* only used by hardware task switching */
    uint32_t esp0;     /* stack pointer loaded on entry to ring 0 */
    uint32_t ss0;      /* stack segment loaded on entry to ring 0 */
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

/* Zeroes the TSS, points ss0:esp0 at the given ring-0 stack, and loads the
 * task register with ltr. Must run after the GDT contains the TSS descriptor. */
void tss_init(uint32_t kernel_stack_top);

/* Repoints esp0 at a different ring-0 stack. Called on every context switch:
 * esp0 is consumed at each ring 3 -> ring 0 entry, so it has to name the
 * incoming task's own kernel stack or two user tasks would share one. */
void tss_set_kernel_stack(uint32_t kernel_stack_top);

uint32_t tss_base(void);
uint32_t tss_limit(void);

#endif /* CPU_TSS_H */
