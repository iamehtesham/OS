#include <stddef.h>
#include <stdint.h>

#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/irq.h"
#include "cpu/isr.h"
#include "sys/syscall.h"

/* Intel SDM Vol. 3A, section 6.11. As with the GDT the handler address is split
 * either side of the selector and attribute bytes. */
struct idt_entry {
    uint16_t base_low;    /* handler address bits 0-15 */
    uint16_t selector;    /* code segment selector the handler runs under */
    uint8_t  always_zero;
    uint8_t  flags;
    uint16_t base_high;   /* handler address bits 16-31 */
} __attribute__((packed));

/* Operand of lidt, same shape as the GDT's. */
struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* 0x8E = present, ring 0, 32-bit interrupt gate. An interrupt gate clears IF on
 * entry, unlike a trap gate, so a handler cannot be interrupted by the very
 * condition it is servicing. */
#define IDT_GATE_INTERRUPT_32 0x8E

/* Same gate with DPL 3. The DPL of an interrupt gate is the highest CPL
 * allowed to reach it with an int instruction, so without this a ring-3
 * int 0x80 raises #GP instead of entering the kernel. It is deliberately the
 * only such gate: every other vector stays DPL 0 so user code cannot fake a
 * page fault or an IRQ. */
#define IDT_GATE_INTERRUPT_32_USER 0xEE

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idt_pointer;

/* Defined in idt_load.S. */
extern void idt_load(const struct idt_ptr *ptr);

/* Addresses of the per-vector stubs, which only assembly can provide because
 * they must not have a C prologue. */
static void (*const exception_stubs[ISR_EXCEPTION_COUNT])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
};

/* Hardware interrupt stubs, wired at IRQ_VECTOR_BASE once the PIC is remapped
 * there. Installing the gates is harmless before pic_init runs, because every
 * PIC line stays masked until irq_install_handler opens it. */
static void (*const irq_stubs[IRQ_COUNT])(void) = {
    irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7,
    irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15,
};

static void idt_set_gate(size_t num, uint32_t base, uint16_t selector, uint8_t flags)
{
    idt[num].base_low    = (uint16_t)(base & 0xFFFF);
    idt[num].base_high   = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].selector    = selector;
    idt[num].always_zero = 0;
    idt[num].flags       = flags;
}

void idt_init(void)
{
    /* Clear every gate explicitly rather than trusting the loader to have
     * zeroed .bss. A zeroed entry has its present bit clear, so an unwired
     * vector raises #GP instead of jumping to address 0. */
    for (size_t i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    for (size_t i = 0; i < ISR_EXCEPTION_COUNT; i++) {
        idt_set_gate(i, (uint32_t)(uintptr_t)exception_stubs[i],
                     GDT_KERNEL_CODE_SELECTOR, IDT_GATE_INTERRUPT_32);
    }

    for (size_t i = 0; i < IRQ_COUNT; i++) {
        idt_set_gate(IRQ_VECTOR_BASE + i, (uint32_t)(uintptr_t)irq_stubs[i],
                     GDT_KERNEL_CODE_SELECTOR, IDT_GATE_INTERRUPT_32);
    }

    idt_set_gate(SYSCALL_VECTOR, (uint32_t)(uintptr_t)isr128, GDT_KERNEL_CODE_SELECTOR,
                 IDT_GATE_INTERRUPT_32_USER);

    idt_pointer.limit = (uint16_t)(sizeof(idt) - 1);
    idt_pointer.base  = (uint32_t)(uintptr_t)&idt;

    idt_load(&idt_pointer);
}
