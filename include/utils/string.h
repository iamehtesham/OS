#ifndef UTILS_STRING_H
#define UTILS_STRING_H

#include <stddef.h>

/* Freestanding build: no libc, so the kernel carries its own string helpers. */
size_t kstrlen(const char *str);

#endif /* UTILS_STRING_H */
