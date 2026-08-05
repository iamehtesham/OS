#include <stdint.h>

#include "cpu/irq.h"
#include "cpu/isr.h"
#include "drivers/vga.h"
#include "mm/paging.h"
#include "utils/stdio.h"

/* Intel SDM Vol. 3A, Table 6-1. */
static const char *const exception_names[ISR_EXCEPTION_COUNT] = {
    "Divide-by-zero Error",           /* 0  #DE */
    "Debug",                          /* 1  #DB */
    "Non-maskable Interrupt",         /* 2  NMI */
    "Breakpoint",                     /* 3  #BP */
    "Overflow",                       /* 4  #OF */
    "Bound Range Exceeded",           /* 5  #BR */
    "Invalid Opcode",                 /* 6  #UD */
    "Device Not Available",           /* 7  #NM */
    "Double Fault",                   /* 8  #DF */
    "Coprocessor Segment Overrun",    /* 9       */
    "Invalid TSS",                    /* 10 #TS */
    "Segment Not Present",            /* 11 #NP */
    "Stack-Segment Fault",            /* 12 #SS */
    "General Protection Fault",       /* 13 #GP */
    "Page Fault",                     /* 14 #PF */
    "Reserved",                       /* 15      */
    "x87 Floating-Point Exception",   /* 16 #MF */
    "Alignment Check",                /* 17 #AC */
    "Machine Check",                  /* 18 #MC */
    "SIMD Floating-Point Exception",  /* 19 #XM */
    "Virtualization Exception",       /* 20 #VE */
    "Control Protection Exception",   /* 21 #CP */
    "Reserved",                       /* 22      */
    "Reserved",                       /* 23      */
    "Reserved",                       /* 24      */
    "Reserved",                       /* 25      */
    "Reserved",                       /* 26      */
    "Reserved",                       /* 27      */
    "Hypervisor Injection Exception", /* 28 #HV */
    "VMM Communication Exception",    /* 29 #VC */
    "Security Exception",             /* 30 #SX */
    "Reserved",                       /* 31      */
};

void interrupt_dispatch(struct registers *regs)
{
    /* Vectors 0-31 are the architecturally reserved CPU exceptions; everything
     * from 32 up is a remapped PIC line. */
    if (regs->int_no < IRQ_VECTOR_BASE) {
        isr_handler(regs);
    } else {
        irq_handler(regs);
    }
}

void isr_handler(struct registers *regs)
{
    const char *name = (regs->int_no < ISR_EXCEPTION_COUNT)
                           ? exception_names[regs->int_no]
                           : "Unknown Interrupt";

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
    kprintf("\n *** CPU EXCEPTION *** \n");

    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    kprintf("  vector     : %u (%s)\n", regs->int_no, name);
    kprintf("  error code : 0x%x\n", regs->err_code);
    kprintf("  eip        : %p\n", (void *)regs->eip);
    kprintf("  cs:eflags  : 0x%x : 0x%x\n", regs->cs, regs->eflags);

    /* A page fault does not carry the offending address in the frame; the CPU
     * leaves it in CR2, and the error code describes the access that faulted
     * rather than being a selector like the other error-code exceptions. */
    if (regs->int_no == ISR_PAGE_FAULT) {
        kprintf("  cr2        : %p <- faulting address\n",
                (void *)paging_fault_address());
        kprintf("  cause      : %s, %s, %s%s\n",
                (regs->err_code & PAGE_FAULT_PROTECTION) ? "protection violation"
                                                         : "page not present",
                (regs->err_code & PAGE_FAULT_WRITE) ? "write" : "read",
                (regs->err_code & PAGE_FAULT_USER) ? "user" : "supervisor",
                (regs->err_code & PAGE_FAULT_FETCH) ? ", instruction fetch" : "");
    }

    kprintf("  halted.\n");

    /* Nothing here can fix the fault, and returning would re-execute the
     * faulting instruction forever. Mask interrupts and stop the CPU; the
     * jump back into hlt catches an NMI waking us up. */
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
