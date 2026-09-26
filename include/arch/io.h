#ifndef ARCH_IO_H
#define ARCH_IO_H

#include <stdint.h>

/* Port-mapped I/O. x86 exposes device registers through a separate 64 KiB
 * address space that only the in/out instructions can reach, so these accesses
 * cannot be expressed as ordinary pointer dereferences.
 *
 * Nothing here is kernel-specific: the encodings are the same at every
 * privilege level, and the CPU decides at run time whether CPL <= IOPL. So a
 * ring-3 driver includes this header too, and its first in/out after
 * SYS_GRANT_IO succeeds where the same instruction a moment earlier would have
 * been a general protection fault. */

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

/* 16- and 32-bit ports. The PCI configuration mechanism (0xCF8/0xCFC) is a
 * 32-bit register pair, and a NIC's registers are read and written a word or a
 * dword at a time; a byte driver could not reach them. Same privilege rule as
 * the byte pair above: these run in ring 3 once IOPL is 3. */
static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;

    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));

    return value;
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;

    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));

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
