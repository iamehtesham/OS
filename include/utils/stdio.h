#ifndef UTILS_STDIO_H
#define UTILS_STDIO_H

#include <stdarg.h>

/* Minimal printf for kernel debugging. Supported conversions:
 *
 *   %c  character            %u  unsigned decimal
 *   %s  NUL-terminated string (prints "(null)" for a null pointer)
 *   %d  signed decimal       %x  lowercase hexadecimal
 *   %i  signed decimal       %X  uppercase hexadecimal
 *   %p  pointer, as 0x-prefixed hex
 *   %%  a literal percent sign
 *
 * There is no field-width, precision, or length-modifier support: arguments are
 * whatever the default promotions produce, so every integer conversion is
 * 32-bit. Returns the number of characters written.
 *
 * The format attribute is what makes GCC type-check these call sites under
 * -Wall; the supported conversions deliberately match standard printf semantics
 * so its diagnostics stay accurate. */
int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int kvprintf(const char *fmt, va_list args);

#endif /* UTILS_STDIO_H */
