#ifndef CPU_GDT_H
#define CPU_GDT_H

/* Segment selectors are an index into the GDT shifted left by 3; the low three
 * bits hold the requested privilege level and the table indicator, all zero for
 * ring-0 GDT entries. Entry 1 is kernel code, entry 2 is kernel data. */
#define GDT_KERNEL_CODE_SELECTOR 0x08
#define GDT_KERNEL_DATA_SELECTOR 0x10
#define GDT_USER_CODE_SELECTOR   0x18
#define GDT_USER_DATA_SELECTOR   0x20
#define GDT_TSS_SELECTOR         0x28

/* A selector's low two bits are its requested privilege level. Ring 3 must
 * load its data and stack segments with RPL 3, and the CS pushed for an iret
 * into ring 3 must carry it too -- that RPL is what tells iret this is an
 * inter-privilege return and makes it pop SS:ESP as well. */
#define GDT_USER_CODE_SELECTOR_RPL3 (GDT_USER_CODE_SELECTOR | 3u)
#define GDT_USER_DATA_SELECTOR_RPL3 (GDT_USER_DATA_SELECTOR | 3u)

void gdt_init(void);

#endif /* CPU_GDT_H */
