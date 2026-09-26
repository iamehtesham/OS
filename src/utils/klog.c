#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/klog.h"
#include "utils/stdio.h"

static char     ring[KLOG_CAPACITY];
static uint32_t total; /* bytes ever written; the ring holds the last KLOG_CAPACITY of them */

static void ring_sink(void *context, const char *data, size_t length)
{
    (void)context;

    for (size_t i = 0; i < length; i++) {
        ring[total % KLOG_CAPACITY] = data[i];
        total++;
    }
}

int klog(const char *fmt, ...)
{
    va_list  args;
    int      written;
    uint32_t flags;

    /* Callers arrive from both sides of the interrupt flag: the idle task logs
     * with interrupts on, and a fault, an IRQ or a system call logs with them
     * off. ring_sink keeps `total` in a register for its loop, so a line
     * started by the idle task and interrupted by a handler's line would have
     * the handler's bytes written at the same position and then overwritten,
     * and `total` stored backwards -- which re-delivers bytes a reader has
     * already taken. One log call is therefore one uninterruptible unit. The
     * flag is saved and restored rather than set, because a caller that had
     * interrupts off must get them back off. */
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) : : "memory");

    va_start(args, fmt);
    written = kvformat(ring_sink, NULL, fmt, args);
    va_end(args);

    if (flags & (1u << 9)) {
        __asm__ volatile ("sti" ::: "memory");
    }

    return written;
}

uint32_t klog_total(void)
{
    return total;
}

uint32_t klog_read(uint32_t *offset, char *out, uint32_t length)
{
    const uint32_t oldest = total > KLOG_CAPACITY ? total - KLOG_CAPACITY : 0;

    if (*offset < oldest) {
        *offset = oldest; /* fell behind; resume at what still exists */
    }

    if (*offset >= total) {
        return 0;
    }

    const uint32_t available = total - *offset;
    const uint32_t count     = length < available ? length : available;

    for (uint32_t i = 0; i < count; i++) {
        out[i] = ring[(*offset + i) % KLOG_CAPACITY];
    }

    return count;
}
