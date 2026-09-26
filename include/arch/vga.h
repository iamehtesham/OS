#ifndef ARCH_VGA_H
#define ARCH_VGA_H

#include <stdint.h>

/* The VGA colour text adapter, as hardware facts with no owner. The kernel's
 * panic path and the ring-3 console server both drive it, so like arch/io.h
 * and arch/ps2.h this header is included at both privilege levels. */

/* Where mode 3's character grid lives in physical memory: 80x25 cells of two
 * bytes, character then attribute, row after row. */
#define VGA_TEXT_BUFFER_PHYS 0xB8000u

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

/* CRTC index/data register pair: a write to the index port selects which
 * internal CRTC register the data port then reads or writes. This is the
 * colour-adapter pair; a monochrome adapter answers at 0x3B4/0x3B5. */
#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5

/* CRTC registers backing the text cursor. The location is a 16-bit CELL index
 * -- row * 80 + column, not a byte offset -- split across two 8-bit registers,
 * and is relative to the start address in registers 0x0C/0x0D, which nothing
 * here ever moves from 0. */
#define VGA_CRTC_CURSOR_START    0x0A /* bits 0-4 top scanline, bit 5 disables the cursor */
#define VGA_CRTC_CURSOR_END      0x0B /* bits 0-4 bottom scanline */
#define VGA_CRTC_CURSOR_LOC_HIGH 0x0E
#define VGA_CRTC_CURSOR_LOC_LOW  0x0F

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

#endif /* ARCH_VGA_H */
