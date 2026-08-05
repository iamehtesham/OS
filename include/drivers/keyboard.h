#ifndef DRIVERS_KEYBOARD_H
#define DRIVERS_KEYBOARD_H

/* PS/2 controller registers. 0x60 carries scancodes out and commands in; 0x64
 * is the status register on read and the command register on write. */
#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64

/* Status bit 0: a byte is waiting in the controller's output buffer. */
#define PS2_STATUS_OUTPUT_FULL 0x01

/* Registers the IRQ1 handler and unmasks the line. Call after pic_init. */
void keyboard_init(void);

#endif /* DRIVERS_KEYBOARD_H */
