#ifndef ARCH_IO_H
#define ARCH_IO_H

#include <stdint.h>

/* Port-mapped I/O. x86 exposes device registers through a separate 64 KiB
 * address space that only the in/out instructions can reach, so these accesses
 * cannot be expressed as ordinary pointer dereferences. */

/* "a" pins the byte to AL; "Nd" lets GCC use the short immediate encoding for
 * port numbers below 0x100 and fall back to DX for the rest. */
static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));

    return value;
}

/* Burns one bus cycle by writing to the POST diagnostic port, which no real
 * device answers. The 8259 needs a short settling delay between back-to-back
 * initialisation-word writes on older chipsets. */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif /* ARCH_IO_H */
