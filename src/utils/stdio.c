#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/vga.h"
#include "utils/stdio.h"
#include "utils/string.h"

/* Widest conversion we can emit is 10 digits (a 32-bit decimal value); 32 bytes
 * leaves room even if a base-2 conversion is added later. */
#define NUMBUF_SIZE 32

static const char digits_lower[] = "0123456789abcdef";
static const char digits_upper[] = "0123456789ABCDEF";

/* Converts value into out, which is NOT NUL-terminated, and returns the digit
 * count. Division yields digits least-significant first, so they are built in a
 * scratch buffer and reversed on the way out.
 *
 * Everything here is deliberately 32-bit: a 64-bit divide would emit a call to
 * libgcc's __udivdi3, which a freestanding link has no way to resolve. */
static size_t kutoa(uint32_t value, uint32_t base, bool uppercase, char *out)
{
    const char *digits = uppercase ? digits_upper : digits_lower;
    char scratch[NUMBUF_SIZE];
    size_t len = 0;

    if (value == 0) {
        scratch[len++] = '0';
    }

    while (value != 0) {
        scratch[len++] = digits[value % base];
        value /= base;
    }

    for (size_t i = 0; i < len; i++) {
        out[i] = scratch[len - 1 - i];
    }

    return len;
}

int kvprintf(const char *fmt, va_list args)
{
    char numbuf[NUMBUF_SIZE];
    int written = 0;

    while (*fmt != '\0') {
        if (*fmt != '%') {
            /* Emit the whole run of literal text in one call so the hardware
             * cursor is reprogrammed once rather than per character. */
            const char *start = fmt;

            while (*fmt != '\0' && *fmt != '%') {
                fmt++;
            }

            const size_t run = (size_t)(fmt - start);

            vga_write(start, run);
            written += (int)run;
            continue;
        }

        fmt++; /* step past the '%' */

        switch (*fmt) {
        case '\0':
            /* Truncated specifier at the end of the format string: show the
             * stray '%' rather than reading past the terminator. */
            vga_write("%", 1);
            written++;
            continue;

        case '%':
            vga_write("%", 1);
            written++;
            break;

        case 'c': {
            /* Default argument promotion widens char to int in varargs. */
            const char c = (char)va_arg(args, int);

            vga_write(&c, 1);
            written++;
            break;
        }

        case 's': {
            const char *str = va_arg(args, const char *);

            if (str == NULL) {
                str = "(null)";
            }

            const size_t len = kstrlen(str);

            vga_write(str, len);
            written += (int)len;
            break;
        }

        case 'd':
        case 'i': {
            const int value = va_arg(args, int);
            uint32_t magnitude;

            if (value < 0) {
                /* Negating INT32_MIN overflows, so take the magnitude in
                 * unsigned arithmetic, where the wraparound is defined. */
                magnitude = (uint32_t)0 - (uint32_t)value;
                vga_write("-", 1);
                written++;
            } else {
                magnitude = (uint32_t)value;
            }

            const size_t len = kutoa(magnitude, 10, false, numbuf);

            vga_write(numbuf, len);
            written += (int)len;
            break;
        }

        case 'u': {
            const size_t len = kutoa(va_arg(args, unsigned int), 10, false, numbuf);

            vga_write(numbuf, len);
            written += (int)len;
            break;
        }

        case 'x':
        case 'X': {
            const size_t len =
                kutoa(va_arg(args, unsigned int), 16, *fmt == 'X', numbuf);

            vga_write(numbuf, len);
            written += (int)len;
            break;
        }

        case 'p': {
            /* Pointers carry an explicit 0x so they cannot be misread as
             * decimal in a log line. */
            const uintptr_t value = (uintptr_t)va_arg(args, void *);
            const size_t len = kutoa((uint32_t)value, 16, false, numbuf);

            vga_write("0x", 2);
            vga_write(numbuf, len);
            written += 2 + (int)len;
            break;
        }

        default:
            /* Unknown conversion: echo it verbatim so a broken format string is
             * visible on screen instead of silently swallowing its argument. */
            vga_write("%", 1);
            vga_write(fmt, 1);
            written += 2;
            break;
        }

        fmt++;
    }

    return written;
}

int kprintf(const char *fmt, ...)
{
    va_list args;
    int written;

    va_start(args, fmt);
    written = kvprintf(fmt, args);
    va_end(args);

    return written;
}
