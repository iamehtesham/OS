#include "user/ulib.h"

/* EAX carries the call number in and the result out; EBX and ECX carry the
 * arguments. The memory clobber stops GCC caching anything across a call the
 * kernel may read from or write to. */

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

bool u_shm_map_pages(struct sys_shm *out, uint32_t peer_pid, uint32_t pages)
{
    int32_t result;

    /* edx carries the page count. Always set, even by the one-page wrapper
     * below: the kernel reads the register, and a register nobody wrote holds
     * whatever the last thing to use it left there. */
    __asm__ volatile ("int $0x80"
                      : "=a"(result)
                      : "a"(SYS_SHM_MAP), "b"(out), "c"(peer_pid), "d"(pages)
                      : "memory");

    return result == 0;
}

bool u_shm_map(struct sys_shm *out, uint32_t peer_pid)
{
    return u_shm_map_pages(out, peer_pid, 1u);
}

bool u_shm_attach_info(struct sys_shm *segment)
{
    int32_t result;

    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_SHM_ATTACH), "b"(segment) : "memory");

    return result == 0;
}

uint32_t u_shm_attach(uint32_t id)
{
    struct sys_shm segment;

    segment.id    = id;
    segment.vaddr = 0;
    segment.pages = 0;

    return u_shm_attach_info(&segment) ? segment.vaddr : 0;
}

int32_t u_spawn(uint32_t shm_id, uint32_t length)
{
    int32_t result;

    __asm__ volatile ("int $0x80"
                      : "=a"(result)
                      : "a"(SYS_SPAWN), "b"(shm_id), "c"(length)
                      : "memory");

    return result;
}

uint32_t u_parent_of(uint32_t pid)
{
    uint32_t result;

    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_PARENT_OF), "b"(pid) : "memory");

    return result;
}

void u_exit(int32_t status)
{
    for (;;) {
        __asm__ volatile ("int $0x80" : : "a"(SYS_EXIT), "b"(status) : "memory");
    }
}

int32_t u_waitpid(uint32_t pid, int32_t *status)
{
    int32_t result;

    __asm__ volatile ("int $0x80"
                      : "=a"(result)
                      : "a"(SYS_WAITPID), "b"(pid), "c"(status)
                      : "memory");

    return result;
}

bool u_grant_io(void)
{
    int32_t result;

    /* The kernel edits the EFLAGS image on its own stack; the iret that ends
     * this instruction is what loads it. So the very next instruction after
     * this one already runs with the new IOPL. */
    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_GRANT_IO) : "memory");

    return result == 0;
}

bool u_unmask_irq(uint32_t irq)
{
    int32_t result;

    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_UNMASK_IRQ), "b"(irq) : "memory");

    return result == 0;
}

uint32_t u_alloc_dma(uint32_t pages, uint32_t *phys_out)
{
    uint32_t result;

    __asm__ volatile ("int $0x80"
                      : "=a"(result)
                      : "a"(SYS_ALLOC_DMA), "b"(pages), "c"(phys_out)
                      : "memory");

    return result;
}

bool u_claim_irq(uint32_t irq)
{
    int32_t result;

    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_CLAIM_IRQ), "b"(irq) : "memory");

    return result == 0;
}

uint32_t u_map_hw_buffer(void)
{
    uint32_t result;

    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_MAP_HW_BUFFER) : "memory");

    return result;
}

bool u_klog_read(struct sys_klog *request)
{
    int32_t result;

    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_KLOG_READ), "b"(request) : "memory");

    return result == 0;
}

bool u_trust_sender(uint32_t pid)
{
    int32_t result;

    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_TRUST_SENDER), "b"(pid) : "memory");

    return result == 0;
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

bool u_alarm(uint32_t ticks)
{
    int32_t result;

    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_ALARM), "b"(ticks) : "memory");

    return result == 0;
}

uint32_t u_ticks(void)
{
    uint32_t result;

    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_TICKS) : "memory");

    return result;
}

uint64_t u_rdtsc(void)
{
    uint32_t low, high;

    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));

    return ((uint64_t)high << 32) | low;
}
