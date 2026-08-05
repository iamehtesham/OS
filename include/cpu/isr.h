#ifndef CPU_ISR_H
#define CPU_ISR_H

#include <stdint.h>

#define ISR_EXCEPTION_COUNT 32

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
 * There is deliberately no useresp/ss pair at the end: the CPU only pushes
 * those when the interrupt crosses a privilege boundary, and this kernel never
 * leaves ring 0. Declaring them would invite reads of whatever happens to sit
 * above the frame. */
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
} __attribute__((packed));

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
