#include <stddef.h>
#include <stdint.h>

#include "utils/string.h"

size_t kstrlen(const char *str)
{
    size_t len = 0;

    while (str[len] != '\0') {
        len++;
    }

    return len;
}

void kstrncpy(char *dest, const char *src, size_t size)
{
    if (size == 0) {
        return;
    }

    size_t i = 0;

    while (i + 1 < size && src[i] != '\0') {
        dest[i] = src[i];
        i++;
    }

    dest[i] = '\0';
}

void *kmemcpy(void *dest, const void *src, size_t n)
{
    uint8_t *const       d = (uint8_t *)dest;
    const uint8_t *const s = (const uint8_t *)src;

    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }

    return dest;
}

void *kmemset(void *dest, int value, size_t n)
{
    uint8_t *const d = (uint8_t *)dest;
    const uint8_t  b = (uint8_t)value;

    for (size_t i = 0; i < n; i++) {
        d[i] = b;
    }

    return dest;
}

int kstrcmp(const char *a, const char *b)
{
    /* Compare as unsigned so bytes above 0x7F order after ASCII rather than
     * before it, which is what strcmp is specified to do. */
    while (*a != '\0' && (unsigned char)*a == (unsigned char)*b) {
        a++;
        b++;
    }

    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
