#ifndef ARCH_PCI_H
#define ARCH_PCI_H

#include <stdint.h>

#include "arch/io.h"

/* PCI configuration space, access mechanism #1: a 32-bit address latch at
 * 0xCF8 and a 32-bit data window at 0xCFC. Writing a device+register address
 * to the latch selects what the data window then reads or writes. Both are
 * dword ports, which is why this needs outl/inl -- a byte driver could not
 * reach config space. Included at ring 3: the driver runs these under IOPL 3. */

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/* The register offsets this system reads. */
#define PCI_VENDOR_ID      0x00 /* u16 */
#define PCI_DEVICE_ID      0x02 /* u16 */
#define PCI_COMMAND        0x04 /* u16 */
#define PCI_BAR0           0x10 /* u32; bit 0 set means an I/O BAR */
#define PCI_INTERRUPT_LINE 0x3C /* u8: the IRQ the device is wired to */

/* Command-register bits. A NIC needs both: IO to answer its I/O BAR, and
 * BUS_MASTER to drive the bus itself, without which its DMA engine is inert
 * and nothing lands in the RX buffer. */
#define PCI_COMMAND_IO         (1u << 0)
#define PCI_COMMAND_BUS_MASTER (1u << 2)

/* A vendor id of 0xFFFF comes back from an absent device: the bus floats the
 * data lines high when nothing answers the address. */
#define PCI_NO_DEVICE 0xFFFFu

static inline uint32_t pci_address(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
           ((uint32_t)func << 8) | (off & 0xFCu);
}

static inline uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, dev, func, off));
    return inl(PCI_CONFIG_DATA);
}

static inline void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t value)
{
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, dev, func, off));
    outl(PCI_CONFIG_DATA, value);
}

/* The 16- and 8-bit reads extract the right lane of the dword the latch
 * addresses: config space is dword-granular, so a u16 at offset 2 is the high
 * half of the dword at offset 0. */
static inline uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    return (uint16_t)(pci_read32(bus, dev, func, off) >> ((off & 2u) * 8u));
}

static inline uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    return (uint8_t)(pci_read32(bus, dev, func, off) >> ((off & 3u) * 8u));
}

#endif /* ARCH_PCI_H */
