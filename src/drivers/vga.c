#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/io.h"
#include "drivers/vga.h"
#include "utils/string.h"

/* Colour text mode maps the character grid straight onto physical memory here;
 * stores land on screen with no driver handshake. volatile keeps the compiler
 * from eliding writes it can see are never read back. */
static volatile uint16_t *const vga_buffer = (volatile uint16_t *)0xB8000;

/* CRTC index/data register pair: a write to CRTC_INDEX selects which internal
 * CRTC register CRTC_DATA then reads or writes. This is the colour-adapter
 * pair; a monochrome adapter would answer at 0x3B4/0x3B5 instead. */
#define CRTC_INDEX 0x3D4
#define CRTC_DATA  0x3D5

/* CRTC registers backing the text cursor. */
#define CRTC_CURSOR_START    0x0A  /* bits 0-4 top scanline, bit 5 disables the cursor */
#define CRTC_CURSOR_END      0x0B  /* bits 0-4 bottom scanline */
#define CRTC_CURSOR_LOC_HIGH 0x0E
#define CRTC_CURSOR_LOC_LOW  0x0F

/* Character cells are 16 scanlines tall in mode 3, so the bottom two rows give
 * the conventional underline cursor. */
#define CURSOR_SCANLINE_START 14
#define CURSOR_SCANLINE_END   15

static size_t  cursor_col;
static size_t  cursor_row;
static uint8_t current_color;

/* Set when the last column of a row has just been filled. The cursor stays put
 * and the move to the next row is deferred until another printable character
 * actually needs it, matching how real terminals behave: without this, an
 * exactly-80-column line would consume two rows once its trailing '\n' arrived,
 * and a '\r' meant to overwrite that line would act on the wrong row. */
static bool wrap_pending;

/* Pushes the software cursor position out to the CRTC as a linear cell index. */
static void vga_sync_cursor(void)
{
    const uint16_t pos = (uint16_t)(cursor_row * VGA_WIDTH + cursor_col);

    outb(CRTC_INDEX, CRTC_CURSOR_LOC_LOW);
    outb(CRTC_DATA, (uint8_t)(pos & 0xFF));
    outb(CRTC_INDEX, CRTC_CURSOR_LOC_HIGH);
    outb(CRTC_DATA, (uint8_t)((pos >> 8) & 0xFF));
}

static void vga_blank_row(size_t row)
{
    for (size_t col = 0; col < VGA_WIDTH; col++) {
        vga_buffer[row * VGA_WIDTH + col] = vga_entry(' ', current_color);
    }
}

/* Drops the top line and shifts the rest up by one row. The copy is written as
 * an explicit loop over the volatile buffer rather than a memmove so that every
 * access stays a real 16-bit store into video memory. */
static void vga_scroll(void)
{
    for (size_t row = 1; row < VGA_HEIGHT; row++) {
        for (size_t col = 0; col < VGA_WIDTH; col++) {
            vga_buffer[(row - 1) * VGA_WIDTH + col] = vga_buffer[row * VGA_WIDTH + col];
        }
    }

    vga_blank_row(VGA_HEIGHT - 1);
}

/* Moves to column 0 of the next line, scrolling instead of advancing when
 * already on the last row. */
static void vga_newline(void)
{
    cursor_col = 0;
    wrap_pending = false;

    if (cursor_row + 1 == VGA_HEIGHT) {
        vga_scroll();
    } else {
        cursor_row++;
    }
}

/* Writes one character without touching the CRTC; callers batch the cursor
 * update so a long string costs four port writes instead of four per byte. */
static void vga_putchar_raw(char c)
{
    switch (c) {
    case '\n':
        vga_newline();
        return;
    case '\r':
        cursor_col = 0;
        wrap_pending = false;
        return;
    case '\b':
        /* Step the insertion point back one cell and blank it. A pending wrap
         * means the point is parked past the last column, so clearing the flag
         * is itself the step back and the column must not also decrement. */
        if (wrap_pending) {
            wrap_pending = false;
        } else if (cursor_col > 0) {
            cursor_col--;
        } else if (cursor_row > 0) {
            cursor_row--;
            cursor_col = VGA_WIDTH - 1;
        } else {
            return; /* top-left corner: nothing to back into */
        }

        vga_buffer[cursor_row * VGA_WIDTH + cursor_col] =
            vga_entry(' ', current_color);
        return;
    case '\t':
        /* Advance to the next 8-column tab stop, stopping at the right edge so
         * the tab never spills a space onto the following row. */
        do {
            vga_putchar_raw(' ');
        } while (!wrap_pending && cursor_col % 8 != 0);
        return;
    default:
        break;
    }

    if (wrap_pending) {
        vga_newline();
    }

    vga_buffer[cursor_row * VGA_WIDTH + cursor_col] =
        vga_entry((unsigned char)c, current_color);

    /* Park on the last column rather than advancing off the row; the next
     * printable character triggers the deferred wrap above. */
    if (cursor_col + 1 == VGA_WIDTH) {
        wrap_pending = true;
    } else {
        cursor_col++;
    }
}

void vga_init(void)
{
    current_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_clear();
    vga_enable_cursor(CURSOR_SCANLINE_START, CURSOR_SCANLINE_END);
}

void vga_clear(void)
{
    for (size_t row = 0; row < VGA_HEIGHT; row++) {
        vga_blank_row(row);
    }

    cursor_col = 0;
    cursor_row = 0;
    wrap_pending = false;
    vga_sync_cursor();
}

void vga_set_color(enum vga_color fg, enum vga_color bg)
{
    current_color = vga_entry_color(fg, bg);
}

void vga_putchar(char c)
{
    vga_putchar_raw(c);
    vga_sync_cursor();
}

void vga_write(const char *data, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        vga_putchar_raw(data[i]);
    }

    vga_sync_cursor();
}

void vga_writestring(const char *str)
{
    vga_write(str, kstrlen(str));
}

void vga_move_cursor(size_t col, size_t row)
{
    cursor_col = (col < VGA_WIDTH) ? col : VGA_WIDTH - 1;
    cursor_row = (row < VGA_HEIGHT) ? row : VGA_HEIGHT - 1;
    wrap_pending = false;
    vga_sync_cursor();
}

void vga_enable_cursor(uint8_t start_scanline, uint8_t end_scanline)
{
    /* Read-modify-write both registers: bits 6-7 of CURSOR_START and bits 5-7
     * of CURSOR_END hold unrelated CRTC state. Clearing bit 5 of CURSOR_START
     * is what actually switches the cursor back on. */
    outb(CRTC_INDEX, CRTC_CURSOR_START);
    outb(CRTC_DATA, (uint8_t)((inb(CRTC_DATA) & 0xC0) | (start_scanline & 0x1F)));

    outb(CRTC_INDEX, CRTC_CURSOR_END);
    outb(CRTC_DATA, (uint8_t)((inb(CRTC_DATA) & 0xE0) | (end_scanline & 0x1F)));
}

void vga_disable_cursor(void)
{
    outb(CRTC_INDEX, CRTC_CURSOR_START);
    outb(CRTC_DATA, 0x20); /* bit 5 set = cursor off */
}
