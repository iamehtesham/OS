#ifndef CPU_GDT_H
#define CPU_GDT_H

/* Segment selectors are an index into the GDT shifted left by 3; the low three
 * bits hold the requested privilege level and the table indicator, all zero for
 * ring-0 GDT entries. Entry 1 is kernel code, entry 2 is kernel data. */
#define GDT_KERNEL_CODE_SELECTOR 0x08
#define GDT_KERNEL_DATA_SELECTOR 0x10

void gdt_init(void);

#endif /* CPU_GDT_H */
