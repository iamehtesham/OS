#include <stdint.h>

#include "arch/io.h"
#include "cpu/irq.h"
#include "cpu/isr.h"
#include "drivers/keyboard.h"
#include "utils/stdio.h"

/* Scancodes arrive one byte at a time; bit 7 distinguishes the two halves of a
 * keystroke, so the table only needs to cover the low 128 values. */
#define SCANCODE_TABLE_SIZE 128

/* Bit 7 set marks a break code -- the release half of a keystroke. */
#define SCANCODE_BREAK_MASK 0x80

/* US QWERTY, scancode set 1, unshifted. Entries left at 0 are keys this driver
 * does not translate: modifiers, function keys, the keypad, and Escape.
 * Everything from 0x40 up zero-fills.
 *
 * Set 1 is what the PS/2 controller produces by default, because it translates
 * the keyboard's native set 2 back into the original XT codes. */
static const char scancode_to_ascii[SCANCODE_TABLE_SIZE] = {
    /* 0x00 */ 0,    0,    '1',  '2',  '3',  '4',  '5',  '6',
    /* 0x08 */ '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',
    /* 0x10 */ 'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',
    /* 0x18 */ 'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',
    /* 0x20 */ 'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',
    /* 0x28 */ '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',
    /* 0x30 */ 'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',
    /* 0x38 */ 0,    ' ',  0,    0,    0,    0,    0,    0,
};

static void keyboard_callback(struct registers *regs)
{
    (void)regs;

    /* The byte must be read even if it is discarded below: the controller holds
     * the line asserted until its output buffer is drained, so skipping the
     * read would wedge the keyboard after one keystroke. */
    const uint8_t scancode = inb(PS2_DATA_PORT);

    /* Only make codes are handled for now. This also swallows the 0xE0 prefix
     * that introduces extended keys such as the arrows, so their second byte
     * arrives as a separate unprefixed scancode -- harmless here because every
     * extended code lands on a 0 entry in the table. */
    if (scancode & SCANCODE_BREAK_MASK) {
        return;
    }

    const char c = scancode_to_ascii[scancode];

    if (c != 0) {
        kprintf("%c", c);
    }
}

void keyboard_init(void)
{
    /* Drain anything the firmware left latched before the line is unmasked.
     * The 8042 holds IRQ1 asserted for as long as a byte sits in its output
     * buffer, and pic_remap's ICW1 has just reset the 8259's edge-sense
     * circuit -- a line that is already high never produces the fresh
     * low-to-high transition an interrupt needs. Since keyboard_callback is the
     * only code that reads the data port, an undrained byte would deadlock the
     * keyboard permanently: no interrupt, so no read, so no edge, forever.
     *
     * The count is bounded so a wedged controller cannot hang the boot. */
    for (int i = 0; i < 16 && (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL); i++) {
        (void)inb(PS2_DATA_PORT);
    }

    irq_install_handler(IRQ_KEYBOARD, keyboard_callback);
}
