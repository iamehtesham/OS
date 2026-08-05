#ifndef CPU_PIC_H
#define CPU_PIC_H

#include <stdint.h>

/* Where the two 8259s are remapped to. The BIOS leaves the master on vectors
 * 8-15, which collide head-on with the CPU's own exceptions #DF through #GP --
 * a timer tick would be indistinguishable from a double fault. Moving them to
 * 32 puts every IRQ above the 32 architecturally reserved vectors. */
#define PIC_MASTER_VECTOR_OFFSET 32
#define PIC_SLAVE_VECTOR_OFFSET  40

/* The slave's output is wired into the master's IRQ2 line. */
#define PIC_CASCADE_IRQ 2

void pic_init(void);
void pic_remap(uint8_t master_offset, uint8_t slave_offset);
void pic_send_eoi(uint8_t irq);
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);

#endif /* CPU_PIC_H */
