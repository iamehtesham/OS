#include <stdint.h>

#include "cpu/irq.h"
#include "cpu/isr.h"
#include "drivers/vga.h"
#include "mm/paging.h"
#include "sys/syscall.h"
#include "task/scheduler.h"
#include "task/task.h"
#include "utils/klog.h"
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
    /* Vectors 0-31 are the architecturally reserved CPU exceptions, 32-47 are
     * the remapped PIC lines, and 0x80 is the system call gate. The syscall
     * check comes first because 0x80 is above the IRQ base and would otherwise
     * be dispatched as a hardware line -- and acknowledged to a PIC that never
     * raised it. */
    if (regs->int_no == SYSCALL_VECTOR) {
        syscall_handler(regs);
    } else if (regs->int_no < IRQ_VECTOR_BASE) {
        isr_handler(regs);
    } else if (regs->int_no < IRQ_VECTOR_BASE + IRQ_COUNT) {
        irq_handler(regs);
    } else {
        klog("[unexpected interrupt vector %u]\n", regs->int_no);
    }
}

/* One report, two destinations. Which one is decided by the caller: a fault in
 * ring 3 is a diagnostic and goes to the kernel log, which the console server
 * shows on the system terminal like anything else; a fault in ring 0 is the
 * end, and goes straight to the screen through the one path the kernel keeps
 * for that, because nothing above the kernel can be trusted to relay it. */
typedef int (*report_fn)(const char *fmt, ...);

static void report(report_fn out, const struct registers *regs, const char *name)
{
    out("  vector     : %u (%s)\n", regs->int_no, name);
    out("  error code : 0x%x\n", regs->err_code);
    out("  eip        : %p\n", (void *)regs->eip);
    out("  cs:eflags  : 0x%x : 0x%x\n", regs->cs, regs->eflags);

    /* A page fault does not carry the offending address in the frame; the CPU
     * leaves it in CR2, and the error code describes the access that faulted
     * rather than being a selector like the other error-code exceptions. */
    if (regs->int_no == ISR_PAGE_FAULT) {
        out("  cr2        : %p <- faulting address\n", (void *)paging_fault_address());
        out("  cause      : %s, %s, %s%s\n",
            (regs->err_code & PAGE_FAULT_PROTECTION) ? "protection violation"
                                                     : "page not present",
            (regs->err_code & PAGE_FAULT_WRITE) ? "write" : "read",
            (regs->err_code & PAGE_FAULT_USER) ? "user" : "supervisor",
            (regs->err_code & PAGE_FAULT_FETCH) ? ", instruction fetch" : "");
    }
}

void isr_handler(struct registers *regs)
{
    const char *name = (regs->int_no < ISR_EXCEPTION_COUNT)
                           ? exception_names[regs->int_no]
                           : "Unknown Interrupt";

    /* A fault in ring 3 is the user program's problem, not the kernel's. End
     * the task and switch away -- halting here would take the PIT, the run
     * queue and every unrelated task down with it, which is a user program
     * being able to stop the whole machine.
     *
     * It ends exactly as sys_exit ends it: task_zombify frees its user memory,
     * hands back any interrupt lines it owned, and wakes a parent waiting on it
     * -- which is the point of doing it here rather than merely parking the
     * task. A shell that called sys_waitpid on this child would otherwise block
     * forever on a corpse that never reported. The status carries the trap
     * vector so the parent can tell a crash from a clean exit. The scheduler
     * skips a zombie and ipc_send refuses it; the loop below is a safety net
     * for the case schedule() somehow returns to it. */
    if (registers_from_user(regs)) {
        klog("*** CPU EXCEPTION in ring 3 ***\n");
        report(klog, regs, name);

        task_t *const self = task_current();

        if (self != NULL) {
            klog("  user task pid %u killed by fault; the kernel keeps running.\n", self->pid);
            task_zombify(self, (int32_t)(TASK_EXIT_FAULT_BASE + regs->int_no));
            schedule();
        }

        for (;;) {
            __asm__ volatile ("sti; hlt");
        }
    }

    /* A ring-0 fault is different: there is no smaller unit to sacrifice and
     * nothing left that can be trusted to keep running -- not even the console
     * server, so this is the one report that goes to the screen directly. Mask
     * interrupts and stop; the jump back into hlt catches an NMI waking us. */
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
    panic_print("\n *** CPU EXCEPTION *** \n");
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    report(panic_print, regs, name);
    panic_print("  halted.\n");

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
