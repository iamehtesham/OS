#ifndef DRIVERS_VGA_H
#define DRIVERS_VGA_H

#include <stddef.h>
#include <stdint.h>

/* Dimensions of the standard VGA colour text mode (mode 3), which is the state
 * the BIOS/bootloader leaves the adapter in when we are handed control. */
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

/* The 4-bit palette hardwired into VGA text mode. Backgrounds only use the low
 * 3 bits: bit 3 of the high nibble is the blink attribute, not intensity. */
enum vga_color {
    VGA_COLOR_BLACK         = 0,
    VGA_COLOR_BLUE          = 1,
    VGA_COLOR_GREEN         = 2,
    VGA_COLOR_CYAN          = 3,
    VGA_COLOR_RED           = 4,
    VGA_COLOR_MAGENTA       = 5,
    VGA_COLOR_BROWN         = 6,
    VGA_COLOR_LIGHT_GREY    = 7,
    VGA_COLOR_DARK_GREY     = 8,
    VGA_COLOR_LIGHT_BLUE    = 9,
    VGA_COLOR_LIGHT_GREEN   = 10,
    VGA_COLOR_LIGHT_CYAN    = 11,
    VGA_COLOR_LIGHT_RED     = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_LIGHT_BROWN   = 14,
    VGA_COLOR_WHITE         = 15,
};

/* Attribute byte layout: background in the high nibble, foreground in the low. */
static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg)
{
    return (uint8_t)((uint8_t)fg | (uint8_t)((uint8_t)bg << 4));
}

/* A text-mode cell is 16 bits little-endian: codepoint then attribute. */
static inline uint16_t vga_entry(unsigned char c, uint8_t color)
{
    return (uint16_t)((uint16_t)c | ((uint16_t)color << 8));
}

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
