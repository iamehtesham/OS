#ifndef UTILS_STDIO_H
#define UTILS_STDIO_H

#include <stdarg.h>
#include <stddef.h>

/* The kernel's formatter, and the one place it may still put text on the
 * screen.
 *
 * Supported conversions:
 *
 *   %c  character            %u  unsigned decimal
 *   %s  NUL-terminated string (prints "(null)" for a null pointer)
 *   %d  signed decimal       %x  lowercase hexadecimal
 *   %i  signed decimal       %X  uppercase hexadecimal
 *   %p  pointer, as 0x-prefixed hex
 *   %%  a literal percent sign
 *
 * No field width, precision or length modifiers: every integer conversion is
 * 32-bit, so no libgcc helper is ever referenced.
 *
 * The format attribute is what makes GCC type-check call sites under -Wall. */

/* Where formatted text goes. The formatter itself writes nowhere: it hands
 * runs of bytes to a sink, and the sink decides. Two exist -- the kernel log
 * and the screen -- and only one of them is for ordinary use. */
typedef void (*kformat_sink)(void *context, const char *data, size_t length);

/* Formats into `sink`. Returns the number of characters produced. */
int kvformat(kformat_sink sink, void *context, const char *fmt, va_list args);

/* Writes straight to the VGA text buffer. Reserved for ring-0 fatal errors:
 * the console belongs to a ring-3 server now, and the kernel writing over it
 * is only acceptable when the kernel is about to stop. Everything else goes to
 * klog (utils/klog.h), which that server displays. */
int panic_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* panic_print in the panic colours, then halt with interrupts off, forever.
 * For a boot that cannot continue: with no memory map, no paging, no heap or
 * no first server, there is nothing above the kernel to hand the message to. */
void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#endif /* UTILS_STDIO_H */
