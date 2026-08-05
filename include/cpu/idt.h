#ifndef CPU_IDT_H
#define CPU_IDT_H

/* The IDT is fixed at 256 entries: the vector number is a byte, so nothing
 * above 255 can ever be raised. */
#define IDT_ENTRIES 256

void idt_init(void);

#endif /* CPU_IDT_H */
