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

int kvformat(kformat_sink sink, void *context, const char *fmt, va_list args)
{
    char numbuf[NUMBUF_SIZE];
    int written = 0;

    while (*fmt != '\0') {
        if (*fmt != '%') {
            /* Hand over the whole run of literal text in one call: a sink that
             * reprograms a hardware cursor does so once per run, not per byte. */
            const char *start = fmt;

            while (*fmt != '\0' && *fmt != '%') {
                fmt++;
            }

            const size_t run = (size_t)(fmt - start);

            sink(context, start, run);
            written += (int)run;
            continue;
        }

        fmt++; /* step past the '%' */

        switch (*fmt) {
        case '\0':
            /* Truncated specifier at the end of the format string: show the
             * stray '%' rather than reading past the terminator. */
            sink(context, "%", 1);
            written++;
            continue;

        case '%':
            sink(context, "%", 1);
            written++;
            break;

        case 'c': {
            /* Default argument promotion widens char to int in varargs. */
            const char c = (char)va_arg(args, int);

            sink(context, &c, 1);
            written++;
            break;
        }

        case 's': {
            const char *str = va_arg(args, const char *);

            if (str == NULL) {
                str = "(null)";
            }

            const size_t len = kstrlen(str);

            sink(context, str, len);
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
                sink(context, "-", 1);
                written++;
            } else {
                magnitude = (uint32_t)value;
            }

            const size_t len = kutoa(magnitude, 10, false, numbuf);

            sink(context, numbuf, len);
            written += (int)len;
            break;
        }

        case 'u': {
            const size_t len = kutoa(va_arg(args, unsigned int), 10, false, numbuf);

            sink(context, numbuf, len);
            written += (int)len;
            break;
        }

        case 'x':
        case 'X': {
            const size_t len =
                kutoa(va_arg(args, unsigned int), 16, *fmt == 'X', numbuf);

            sink(context, numbuf, len);
            written += (int)len;
            break;
        }

        case 'p': {
            /* Pointers carry an explicit 0x so they cannot be misread as
             * decimal in a log line. */
            const uintptr_t value = (uintptr_t)va_arg(args, void *);
            const size_t len = kutoa((uint32_t)value, 16, false, numbuf);

            sink(context, "0x", 2);
            sink(context, numbuf, len);
            written += 2 + (int)len;
            break;
        }

        default:
            /* Unknown conversion: echo it verbatim so a broken format string is
             * visible instead of silently swallowing its argument. */
            sink(context, "%", 1);
            sink(context, fmt, 1);
            written += 2;
            break;
        }

        fmt++;
    }

    return written;
}

static void screen_sink(void *context, const char *data, size_t length)
{
    (void)context;
    vga_write(data, length);
}

int panic_print(const char *fmt, ...)
{
    va_list args;
    int written;

    va_start(args, fmt);
    written = kvformat(screen_sink, NULL, fmt, args);
    va_end(args);

    return written;
}

void panic(const char *fmt, ...)
{
    va_list args;

    __asm__ volatile ("cli");

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
    vga_write("\n *** KERNEL PANIC *** \n", 24);
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);

    va_start(args, fmt);
    (void)kvformat(screen_sink, NULL, fmt, args);
    va_end(args);

    vga_write("\n  halted.\n", 11);

    /* The jump back into hlt catches an NMI waking the CPU. */
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
