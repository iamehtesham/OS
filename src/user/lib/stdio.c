/* Output, for a process that is not allowed to touch the screen.
 *
 * There is no print system call any more. A character leaves a process as a
 * message to the console server, which owns the one mapping of the VGA text
 * buffer and decides which terminal the character belongs on. Formatting
 * happens here, in the printing process; the console never sees a format
 * string, only characters, so nothing a process prints can be mistaken for an
 * instruction to the console. */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include "ipc/ipc.h"
#include "ipc/vga_proto.h"
#include "user/ulib.h"

/* Up to VGA_PRINT_CHUNK characters per message. The console places a message
 * whole, so a line that fits one message can never be split by another
 * process's output -- and a terminal may have two writers now, a shell and a
 * program it started. Bounded, like every send: a busy console is waited for,
 * a dead one answers NO_TASK at once. */
static bool emit(const char *text, uint32_t count)
{
    ipc_message_t msg;

    msg.sender_pid   = 0;
    msg.receiver_pid = 0;
    msg.type         = MSG_PRINT_STR;
    u_memset(msg.data, 0, IPC_PAYLOAD_SIZE);
    msg.data[0] = (uint8_t)count;
    u_memcpy(msg.data + 1, text, count);

    return u_send_bounded(VGA_SERVER_PID, &msg, VGA_PRINT_ATTEMPTS) == IPC_OK;
}

int32_t u_print(const char *text)
{
    int32_t sent = 0;

    while (*text != '\0') {
        uint32_t count = 0;

        while (count < VGA_PRINT_CHUNK && text[count] != '\0') {
            count++;
        }

        if (!emit(text, count)) {
            break; /* the console is gone; the rest would go the same way */
        }

        sent += (int32_t)count;
        text += count;
    }

    return sent;
}

/* Variable-width lowercase hex with no prefix and no leading zeros -- the
 * kernel's %x, so that a number reads the same from either side of the
 * privilege boundary. */
static char *append_hex_digits(char *out, const char *limit, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    char              scratch[8];
    uint32_t          count = 0;

    do {
        scratch[count++] = digits[value & 0xFu];
        value >>= 4;
    } while (value != 0u);

    while (count > 0u && out < limit) {
        *out++ = scratch[--count];
    }

    return out;
}

/* Formats into out, which is not terminated; returns the new end. Stops at
 * limit. The conversions are the kernel formatter's: %c %s %d %i %u %x %p %%,
 * no widths, everything 32-bit. %x is bare variable-width hex and %p is the
 * same with a 0x prefix, exactly as in the kernel. */
static char *format(char *out, const char *limit, const char *fmt, va_list args)
{
    while (*fmt != '\0' && out < limit) {
        if (*fmt != '%') {
            *out++ = *fmt++;
            continue;
        }

        fmt++;

        switch (*fmt) {
        case '\0':
            /* A '%' at the very end: show it rather than read past the end. */
            if (out < limit) {
                *out++ = '%';
            }

            return out;

        case '%':
            *out++ = '%';
            break;

        case 'c':
            *out++ = (char)va_arg(args, int);
            break;

        case 's': {
            const char *s = va_arg(args, const char *);

            out = u_append(out, limit, s != 0 ? s : "(null)");
            break;
        }

        case 'd':
        case 'i': {
            const int value = va_arg(args, int);

            if (value < 0) {
                *out++ = '-';
                /* The magnitude in unsigned arithmetic, where INT32_MIN's
                 * negation is defined. */
                out = u_append_dec(out, limit, (uint32_t)0 - (uint32_t)value);
            } else {
                out = u_append_dec(out, limit, (uint32_t)value);
            }

            break;
        }

        case 'u':
            out = u_append_dec(out, limit, va_arg(args, unsigned int));
            break;

        case 'x':
            out = append_hex_digits(out, limit, va_arg(args, unsigned int));
            break;

        case 'p':
            /* Pointers carry an explicit 0x, like the kernel's, so they cannot
             * be misread as decimal; %x does not. Neither is zero-padded --
             * u_append_hex, which is, is for the fixed-width addresses the
             * demo programs print, not for this. */
            out = u_append(out, limit, "0x");
            out = append_hex_digits(out, limit, va_arg(args, unsigned int));
            break;

        default:
            /* Unknown conversion: echoed, so a broken format string is
             * visible rather than silently eating its argument. */
            *out++ = '%';

            if (out < limit) {
                *out++ = *fmt;
            }

            break;
        }

        fmt++;
    }

    return out;
}

int32_t u_printf(const char *fmt, ...)
{
    /* Static, like every line buffer in userland: it lands in .bss, which the
     * loader zero-fills, so there is no initialiser for GCC to turn into a
     * memset this freestanding link could not resolve. A process prints from
     * one place at a time, so one buffer is enough. */
    static char line[256];

    va_list args;

    va_start(args, fmt);
    char *const end = format(line, U_LIMIT(line), fmt, args);
    va_end(args);

    *end = '\0';

    return u_print(line);
}
