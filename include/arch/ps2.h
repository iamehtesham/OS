#ifndef ARCH_PS2_H
#define ARCH_PS2_H

/* The 8042 PS/2 controller's ports. Hardware facts with no owner: the ring-3
 * keyboard driver reads scancodes through them, and the kernel touches them
 * only to drain the controller at boot and to unwedge it if the driver is
 * gone. Like arch/io.h, this header is included at both privilege levels. */

/* 0x60 carries scancodes out and commands in; 0x64 is the status register on
 * read and the command register on write. */
#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64

/* Status bit 0: a byte is waiting in the controller's output buffer. The
 * controller holds IRQ1 asserted for as long as this is set. */
#define PS2_STATUS_OUTPUT_FULL 0x01

#endif /* ARCH_PS2_H */
