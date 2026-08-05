#include <stdint.h>

#include "cpu/gdt.h"

#define GDT_ENTRIES 3

/* Intel SDM Vol. 3A, section 3.4.5. The base and limit are scattered across
 * non-contiguous fields because the layout still carries the 286's segment
 * descriptor format underneath the 386 extensions. */
struct gdt_entry {
    uint16_t limit_low;   /* limit bits 0-15 */
    uint16_t base_low;    /* base bits 0-15 */
    uint8_t  base_middle; /* base bits 16-23 */
    uint8_t  access;
    uint8_t  granularity; /* flags in bits 4-7, limit bits 16-19 in bits 0-3 */
    uint8_t  base_high;   /* base bits 24-31 */
} __attribute__((packed));

/* Operand of lgdt: a 16-bit limit followed by the 32-bit linear base. */
struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* Access byte, high to low: present | DPL | descriptor type | segment type.
 *   0x9A = present, ring 0, code/data, execute-and-read code segment
 *   0x92 = present, ring 0, code/data, read-and-write data segment */
#define GDT_ACCESS_KERNEL_CODE 0x9A
#define GDT_ACCESS_KERNEL_DATA 0x92

/* Granularity byte: G=1 scales the limit by 4 KiB, D/B=1 selects 32-bit
 * operands and addresses, and the low nibble carries limit bits 16-19. 0xCF is
 * therefore "4 KiB granular, 32-bit, limit 0xFFFFF", spanning the full 4 GiB. */
#define GDT_GRANULARITY_4K_32BIT 0xCF

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gdt_pointer;

/* Defined in gdt_flush.S. */
extern void gdt_flush(const struct gdt_ptr *ptr);

static void gdt_set_gate(int index, uint32_t base, uint32_t limit,
                         uint8_t access, uint8_t granularity)
{
    gdt[index].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[index].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].base_high   = (uint8_t)((base >> 24) & 0xFF);

    gdt[index].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[index].granularity = (uint8_t)(((limit >> 16) & 0x0F) | (granularity & 0xF0));

    gdt[index].access = access;
}

void gdt_init(void)
{
    /* Flat model: both real segments span the whole address space, so
     * segmentation is effectively neutralised and paging will do the real
     * memory management later. The null descriptor at index 0 is mandatory --
     * loading a selector of 0 must fault. */
    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0xFFFFF, GDT_ACCESS_KERNEL_CODE, GDT_GRANULARITY_4K_32BIT);
    gdt_set_gate(2, 0, 0xFFFFF, GDT_ACCESS_KERNEL_DATA, GDT_GRANULARITY_4K_32BIT);

    /* The limit field holds the size in bytes minus one. */
    gdt_pointer.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_pointer.base  = (uint32_t)(uintptr_t)&gdt;

    gdt_flush(&gdt_pointer);
}
