/* The keyboard driver, in ring 3.
 *
 * Everything the kernel's old keyboard.c did, it does here instead: read the
 * scancode, decode it, print it. The kernel's part shrank to two things it
 * alone can do -- take the interrupt and touch the interrupt controller -- and
 * both of those it does on this process's behalf, over the same IPC every
 * other program uses.
 *
 * The cycle for one keystroke:
 *
 *   key pressed -> IRQ1 -> kernel masks IRQ1, marks it pending here, switches
 *   here -> recv returns MSG_HARDWARE_INTERRUPT -> inb(0x60) -> decode, print
 *   -> u_unmask_irq(1) -> the line is open for the next key.
 *
 * Two privileges make it possible, and this process holds them because the
 * kernel decided at boot that it should, not because it asked. It may raise its
 * IOPL, which is what makes the inb below execute rather than fault; and it is
 * the task IRQ1 is routed to, which is what makes the unmask succeed. */

#include <stdbool.h>
#include <stdint.h>

#include "arch/io.h"
#include "arch/ps2.h"
#include "ipc/input_proto.h"
#include "ipc/ipc.h"
#include "ipc/kbd_proto.h"
#include "user/ulib.h"

/* Scancodes arrive one byte at a time; bit 7 distinguishes the two halves of a
 * keystroke, so the tables only need to cover the low 128 values. */
#define SCANCODE_TABLE_SIZE 128u

/* Bit 7 set marks a break code -- the release half of a keystroke. */
#define SCANCODE_BREAK_MASK 0x80u

#define SCANCODE_LSHIFT 0x2Au
#define SCANCODE_RSHIFT 0x36u

/* Prefix byte: the next byte is an extended key (arrows, keypad Enter, right
 * control, Print Screen ...). */
#define SCANCODE_EXTENDED 0xE0u

/* US QWERTY, scancode set 1. Entries left at 0 are keys this driver does not
 * translate: control, alt, caps lock, function keys and the keypad. Escape is
 * 0x1B, which the input server consumes to show the system console.
 * Everything from 0x40 up zero-fills.
 *
 * Set 1 is what the PS/2 controller produces by default, because it translates
 * the keyboard's native set 2 back into the original XT codes. */
static const char unshifted[SCANCODE_TABLE_SIZE] = {
    /* 0x00 */ 0,    0x1B, '1',  '2',  '3',  '4',  '5',  '6',
    /* 0x08 */ '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',
    /* 0x10 */ 'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',
    /* 0x18 */ 'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',
    /* 0x20 */ 'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',
    /* 0x28 */ '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',
    /* 0x30 */ 'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',
    /* 0x38 */ 0,    ' ',  0,    0,    0,    0,    0,    0,
};

static const char shifted[SCANCODE_TABLE_SIZE] = {
    /* 0x00 */ 0,    0x1B, '!',  '@',  '#',  '$',  '%',  '^',
    /* 0x08 */ '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t',
    /* 0x10 */ 'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',
    /* 0x18 */ 'O',  'P',  '{',  '}',  '\n', 0,    'A',  'S',
    /* 0x20 */ 'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',
    /* 0x28 */ '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',
    /* 0x30 */ 'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',
    /* 0x38 */ 0,    ' ',  0,    0,    0,    0,    0,    0,
};

/* Driver state, and the reason a driver is a process: it lives here, in
 * memory nothing else can reach, across interrupts. */
static bool     shift_down;
static bool     extended_pending; /* the previous byte was the 0xE0 prefix */
static uint32_t keys_undelivered; /* the input server refused or was gone */

static void translate(uint8_t scancode)
{
    /* An extended key is two bytes, and the second reuses ordinary values:
     * keypad Enter is 0xE0 0x1C, and 0x1C on its own is Enter. Looking the
     * second byte up in the plain table would print a newline for a key this
     * driver does not claim to understand -- and Print Screen, whose make code
     * is 0xE0 0x2A 0xE0 0x37, would press shift and print an asterisk. So the
     * prefix is remembered and the byte after it is dropped whole: none of the
     * extended keys are decoded here, deliberately. */
    if (scancode == SCANCODE_EXTENDED) {
        extended_pending = true;
        return;
    }

    if (extended_pending) {
        extended_pending = false;
        return;
    }

    const bool    released = (scancode & SCANCODE_BREAK_MASK) != 0;
    const uint8_t key      = scancode & (uint8_t)~SCANCODE_BREAK_MASK;

    /* Shift is the one key whose release matters. */
    if (key == SCANCODE_LSHIFT || key == SCANCODE_RSHIFT) {
        shift_down = !released;
        return;
    }

    /* Only make codes print. */
    if (released) {
        return;
    }

    const char c = shift_down ? shifted[key] : unshifted[key];

    if (c == 0) {
        return;
    }

    /* The driver does not print. It knows what the key IS; it has no idea who
     * it is FOR, and that decision belongs to the input server. So the
     * character is packaged and sent, and this process's part is over. */
    ipc_message_t key_msg;

    key_msg.sender_pid   = 0; /* the kernel stamps the real one */
    key_msg.receiver_pid = 0;
    key_msg.type         = MSG_KEYPRESS;
    u_memset(key_msg.data, 0, IPC_PAYLOAD_SIZE);
    key_msg.data[0] = (uint8_t)c;

    /* Bounded, like every send from a server to something it does not control.
     * An input server that has stopped reading must cost it keystrokes, not
     * cost the driver its loop -- a driver parked in a retry never unmasks the
     * line, and the keyboard is dead for everyone. */
    if (u_send_bounded(INPUT_SERVER_PID, &key_msg, INPUT_SEND_ATTEMPTS) != IPC_OK) {
        keys_undelivered++;

        if (keys_undelivered == 1) {
            u_print("  [kbd] the input server is not taking keys; dropping them\n");
        }
    }
}

/* Services one interrupt. Reads while the controller says a byte is waiting
 * rather than exactly once: on the way in, a notification for a line whose
 * byte was already consumed must not read stale data out of 0x60, and on the
 * way out a second byte that arrived while the line was masked is handled now
 * rather than costing an extra round trip. */
static void service_keyboard(void)
{
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
        translate(inb(PS2_DATA_PORT));
    }
}

void _start(void)
{
    /* Without this the inb in service_keyboard is a general protection fault
     * and the kernel kills this process. Its success is the kernel agreeing
     * that this process is a driver. */
    if (!u_grant_io()) {
        u_print("  [kbd] the kernel refused to raise IOPL: not a driver, exiting\n");
        goto park;
    }

    u_print("  [kbd] IOPL raised; driving the PS/2 controller from ring 3. Type something.\n");

    ipc_message_t msg;

    for (;;) {
        if (u_recv(&msg) != IPC_OK) {
            continue;
        }

        /* Only the kernel can be stamped with IPC_KERNEL_PID, so this line is
         * what makes a forged "interrupt" from another process fall through:
         * it would otherwise read the data port with nothing waiting and
         * unmask a line on someone else's say-so. Anything else that lands
         * here is simply not for this driver. */
        if (msg.type != MSG_HARDWARE_INTERRUPT || msg.sender_pid != IPC_KERNEL_PID) {
            continue;
        }

        const uint32_t irq = u_load32(msg.data);

        if (irq != KBD_IRQ) {
            continue; /* not a line this driver was given */
        }

        service_keyboard();

        /* Last, after the device has been read. Until this call the line is
         * closed, so no second IRQ1 can arrive while this one is still being
         * handled -- that ordering is the whole synchronisation between the
         * device and this loop. */
        if (!u_unmask_irq(KBD_IRQ)) {
            u_print("  [kbd] the kernel refused to unmask IRQ1; the keyboard is stuck\n");
        }
    }

park:
    for (;;) {
        u_recv(&msg);
    }
}
