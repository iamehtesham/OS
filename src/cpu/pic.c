#include <stdint.h>

#include "arch/io.h"
#include "cpu/pic.h"

/* Each 8259 answers on two ports: a command port and a data port. The master
 * sits at 0x20/0x21, the slave at 0xA0/0xA1. */
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

/* OCW2 with the EOI bit set: clears the highest-priority in-service bit. */
#define PIC_EOI 0x20

/* Initialisation control words. Writing ICW1 to the command port starts the
 * four-word init sequence; the remaining three go to the data port. */
#define ICW1_ICW4 0x01 /* an ICW4 will follow */
#define ICW1_INIT 0x10 /* begin initialisation */
#define ICW4_8086 0x01 /* 8086/88 mode rather than the ancient MCS-80/85 mode */

void pic_remap(uint8_t master_offset, uint8_t slave_offset)
{
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /* ICW2: the vector each chip's first line maps to. */
    outb(PIC1_DATA, master_offset);
    io_wait();
    outb(PIC2_DATA, slave_offset);
    io_wait();

    /* ICW3 is asymmetric: the master takes a bitmask of which lines have a
     * slave attached, the slave takes the line number it answers on. */
    outb(PIC1_DATA, 1u << PIC_CASCADE_IRQ);
    io_wait();
    outb(PIC2_DATA, PIC_CASCADE_IRQ);
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();
}

void pic_init(void)
{
    pic_remap(PIC_MASTER_VECTOR_OFFSET, PIC_SLAVE_VECTOR_OFFSET);

    /* Mask every line. irq_install_handler unmasks the ones we actually
     * service, so no device can interrupt into a vector with no handler. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_send_eoi(uint8_t irq)
{
    /* A slave line leaves an in-service bit set on both chips: the slave's own
     * and the master's cascade line. Both must be acknowledged, slave first. */
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }

    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_set_mask(uint8_t irq)
{
    const uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    const uint8_t  bit  = (uint8_t)(irq & 7);

    outb(port, (uint8_t)(inb(port) | (uint8_t)(1u << bit)));
}

void pic_clear_mask(uint8_t irq)
{
    const uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    const uint8_t  bit  = (uint8_t)(irq & 7);

    outb(port, (uint8_t)(inb(port) & (uint8_t)~(1u << bit)));
}
