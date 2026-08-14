#include <stdint.h>

#include "cpu/gdt.h"
#include "cpu/tss.h"

static struct tss_entry tss;

void tss_init(uint32_t kernel_stack_top)
{
    uint8_t *const raw = (uint8_t *)&tss;

    for (uint32_t i = 0; i < sizeof(tss); i++) {
        raw[i] = 0;
    }

    tss.ss0  = GDT_KERNEL_DATA_SELECTOR;
    tss.esp0 = kernel_stack_top;

    /* An iomap_base at or past the segment limit tells the CPU there is no I/O
     * permission bitmap. Leaving it zero would make the CPU read the start of
     * the TSS itself as a bitmap on any ring-3 port access. */
    tss.iomap_base = (uint16_t)sizeof(tss);

    /* Loads the task register. The selector's RPL must be 0; the descriptor
     * itself is what the CPU consults on a privilege change. */
    __asm__ volatile ("ltr %0" : : "r"((uint16_t)GDT_TSS_SELECTOR));
}

void tss_set_kernel_stack(uint32_t kernel_stack_top)
{
    tss.esp0 = kernel_stack_top;
}

uint32_t tss_base(void)
{
    return (uint32_t)(uintptr_t)&tss;
}

uint32_t tss_limit(void)
{
    /* A TSS descriptor's limit is the last valid byte, and must be at least
     * 103 for a 32-bit TSS. */
    return (uint32_t)sizeof(tss) - 1u;
}
