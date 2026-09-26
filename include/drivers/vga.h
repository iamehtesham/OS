#ifndef DRIVERS_VGA_H
#define DRIVERS_VGA_H

#include <stddef.h>
#include <stdint.h>

#include "arch/vga.h"

/* The kernel's own text driver. It clears the screen once at boot and is
 * otherwise used by exactly one caller: panic. Ordinary output goes to the
 * kernel log and is shown by the ring-3 console server, which maps the same
 * buffer this driver writes to and owns it from then on. */

void vga_init(void);
void vga_clear(void);

void vga_set_color(enum vga_color fg, enum vga_color bg);

void vga_putchar(char c);
void vga_write(const char *data, size_t size);
void vga_writestring(const char *str);

void vga_move_cursor(size_t col, size_t row);
void vga_enable_cursor(uint8_t start_scanline, uint8_t end_scanline);
void vga_disable_cursor(void);

#endif /* DRIVERS_VGA_H */
