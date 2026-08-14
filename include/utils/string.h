#ifndef UTILS_STRING_H
#define UTILS_STRING_H

#include <stddef.h>

/* Freestanding build: no libc, so the kernel carries its own string helpers. */
size_t kstrlen(const char *str);

/* Copies at most size-1 bytes and ALWAYS terminates, unlike strncpy, which
 * leaves the destination unterminated on truncation. Callers here handle names
 * that came from a file image, so silent truncation must still be safe. */
void kstrncpy(char *dest, const char *src, size_t size);

/* Returns <0, 0 or >0 like strcmp. */
int kstrcmp(const char *a, const char *b);

#endif /* UTILS_STRING_H */
