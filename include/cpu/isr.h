#ifndef CPU_ISR_H
#define CPU_ISR_H

#include <stdbool.h>
#include <stdint.h>

#define ISR_EXCEPTION_COUNT 32

/* Software interrupt vector reserved for system calls. Its IDT gate carries
 * DPL 3 so ring 3 is allowed to raise it; every other gate stays DPL 0, which
 * is what stops user code from forging an exception or an IRQ. */
#define SYSCALL_VECTOR 0x80

/* Vector 14. Unlike other exceptions it reports the offending address out of
 * band, in CR2 rather than in the interrupt frame. */
#define ISR_PAGE_FAULT 14

/* Page fault error code bits (Intel SDM Vol. 3A, section 4.7). */
#define PAGE_FAULT_PROTECTION (1u << 0) /* clear means the page was not present */
#define PAGE_FAULT_WRITE      (1u << 1) /* clear means the access was a read    */
#define PAGE_FAULT_USER       (1u << 2) /* clear means it happened in ring 0    */
#define PAGE_FAULT_RESERVED   (1u << 3) /* a reserved bit was set in an entry   */
#define PAGE_FAULT_FETCH      (1u << 4) /* the access was an instruction fetch  */

/* A view onto the interrupt frame built by the stubs in isr_stubs.S. Field
 * order mirrors the push order exactly, lowest address first, so a pointer to
 * the saved DS is a pointer to this struct.
 *
 * useresp and ss exist ONLY when the interrupt crossed a privilege boundary --
 * the CPU pushes the interrupted stack there so iret can switch back. For an
 * interrupt taken in ring 0 the frame simply ends at eflags and those two
 * fields alias whatever happens to sit above it. Test (cs & 3) != 0 before
 * reading them. */
struct registers {
    uint32_t ds;        /* pushed by the stub */
    uint32_t edi;       /* pusha, in the order it writes them */
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp_at_pusha; /* ESP before pusha ran; not useful, kept for layout */
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t int_no;    /* pushed by the stub */
    uint32_t err_code;  /* pushed by the CPU, or a dummy 0 from the stub */
    uint32_t eip;       /* pushed by the CPU */
    uint32_t cs;
    uint32_t eflags;
    uint32_t useresp;   /* present only when (cs & 3) != 0 */
    uint32_t ss;        /* likewise */
} __attribute__((packed));

/* True when the interrupt arrived from ring 3, which is also exactly when
 * useresp and ss are meaningful. */
static inline bool registers_from_user(const struct registers *regs)
{
    return (regs->cs & 3u) != 0u;
}

/* Called from interrupt_common_stub for every vector. Routes to the exception
 * handler or the IRQ dispatcher based on the vector number. */
void interrupt_dispatch(struct registers *regs);

void isr_handler(struct registers *regs);

/* CPU exception entry points, one per vector, defined in isr_stubs.S. */
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

#endif /* CPU_ISR_H */
