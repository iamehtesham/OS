#include "user/ulib.h"

/* EAX carries the call number in and the result out; EBX and ECX carry the
 * arguments. The memory clobber stops GCC caching anything across a call the
 * kernel may read from or write to. */

int32_t u_print(const char *text)
{
    int32_t result;

    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_PRINT), "b"(text) : "memory");

    return result;
}

int32_t u_send(uint32_t target_pid, const ipc_message_t *msg)
{
    int32_t result;

    __asm__ volatile ("int $0x80"
                      : "=a"(result)
                      : "a"(SYS_SEND), "b"(target_pid), "c"(msg)
                      : "memory");

    return result;
}

int32_t u_recv(ipc_message_t *msg)
{
    int32_t result;

    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_RECV), "b"(msg) : "memory");

    return result;
}

void u_yield(void)
{
    int32_t ignored;

    __asm__ volatile ("int $0x80" : "=a"(ignored) : "a"(SYS_YIELD) : "memory");
}

uint32_t u_map_physical(uint32_t physical_addr, uint32_t length)
{
    uint32_t result;

    __asm__ volatile ("int $0x80"
                      : "=a"(result)
                      : "a"(SYS_MAP_PHYSICAL), "b"(physical_addr), "c"(length)
                      : "memory");

    return result;
}

bool u_grant_info(struct sys_grant *out)
{
    int32_t result;

    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_GRANT_INFO), "b"(out) : "memory");

    return result == 0;
}

bool u_shm_map(struct sys_shm *out, uint32_t peer_pid)
{
    int32_t result;

    __asm__ volatile ("int $0x80"
                      : "=a"(result)
                      : "a"(SYS_SHM_MAP), "b"(out), "c"(peer_pid)
                      : "memory");

    return result == 0;
}

uint32_t u_shm_attach(uint32_t id)
{
    uint32_t result;

    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_SHM_ATTACH), "b"(id) : "memory");

    return result;
}

int32_t u_send_bounded(uint32_t target_pid, const ipc_message_t *msg, uint32_t attempts)
{
    for (;;) {
        const int32_t result = u_send(target_pid, msg);

        /* A full slot means the receiver has not collected its last message.
         * Give it the CPU rather than spinning on the slot; every other result
         * is final. */
        if (result != IPC_ERR_FULL) {
            return result;
        }

        if (attempts == 0) {
            return IPC_ERR_FULL;
        }

        attempts--;
        u_yield();
    }
}

/* ---- strings and memory -------------------------------------------------- */

size_t u_strlen(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0') {
        n++;
    }

    return n;
}

int u_strcmp(const char *a, const char *b)
{
    while (*a != '\0' && (unsigned char)*a == (unsigned char)*b) {
        a++;
        b++;
    }

    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

void u_strncpy(char *dest, const char *src, size_t size)
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

void *u_memcpy(void *dest, const void *src, size_t n)
{
    uint8_t *const       d = (uint8_t *)dest;
    const uint8_t *const s = (const uint8_t *)src;

    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }

    return dest;
}

void *u_memset(void *dest, int value, size_t n)
{
    uint8_t *const d = (uint8_t *)dest;
    const uint8_t  b = (uint8_t)value;

    for (size_t i = 0; i < n; i++) {
        d[i] = b;
    }

    return dest;
}

/* ---- building output lines ----------------------------------------------- */

char *u_append(char *out, const char *limit, const char *text)
{
    while (*text != '\0' && out < limit) {
        *out++ = *text++;
    }

    return out;
}

char *u_append_dec(char *out, const char *limit, uint32_t value)
{
    char     digits[10];
    uint32_t count = 0;

    /* Unsigned division by a constant becomes a multiply and a shift, so this
     * never reaches for a libgcc helper the link could not resolve. */
    do {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);

    while (count > 0u && out < limit) {
        *out++ = digits[--count];
    }

    return out;
}

char *u_append_hex(char *out, const char *limit, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";

    if (out < limit) {
        *out++ = '0';
    }

    if (out < limit) {
        *out++ = 'x';
    }

    for (int32_t shift = 28; shift >= 0 && out < limit; shift -= 4) {
        *out++ = digits[(value >> shift) & 0xFu];
    }

    return out;
}

uint32_t u_load32(const uint8_t *from)
{
    return (uint32_t)from[0] | ((uint32_t)from[1] << 8) | ((uint32_t)from[2] << 16) |
           ((uint32_t)from[3] << 24);
}

void u_store32(uint8_t *to, uint32_t value)
{
    to[0] = (uint8_t)(value & 0xFFu);
    to[1] = (uint8_t)((value >> 8) & 0xFFu);
    to[2] = (uint8_t)((value >> 16) & 0xFFu);
    to[3] = (uint8_t)((value >> 24) & 0xFFu);
}

void u_spin(uint32_t iterations)
{
    for (volatile uint32_t i = 0; i < iterations; i++) {
    }
}
