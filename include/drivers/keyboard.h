#ifndef DRIVERS_KEYBOARD_H
#define DRIVERS_KEYBOARD_H

#include <stdint.h>

/* The kernel half of the keyboard. There is no scancode table here any more:
 * the kernel drains the controller once at boot, and from then on IRQ1 is
 * handed to the ring-3 driver the kernel routed it to. What remains is the
 * fallback for when no such driver is alive. */

/* Registers the IRQ1 handler and unmasks the line. Call after pic_init. */
void keyboard_init(void);

/* Scancodes the kernel had to read and throw away because no ring-3 driver
 * was there to take them. Zero on a healthy system. */
uint32_t keyboard_dropped(void);

#endif /* DRIVERS_KEYBOARD_H */
