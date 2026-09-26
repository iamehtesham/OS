#ifndef UTILS_KLOG_H
#define UTILS_KLOG_H

#include <stdint.h>

/* The kernel log.
 *
 * The kernel no longer prints. Its diagnostics -- what it found at boot, which
 * program landed on which pid, who was granted what, and which task died of
 * which fault -- go into this ring buffer, and a ring-3 console server reads
 * them out with SYS_KLOG_READ and shows them on the system terminal. The
 * kernel touches the screen only to panic. A microkernel's kernel is not the
 * thing that owns the display; it is the thing that remembers what happened
 * until whoever does own it asks. */

/* Bytes retained. Older text is overwritten; a reader that falls further
 * behind than this is silently moved up to the oldest byte still held. */
#define KLOG_CAPACITY 8192u

/* Same conversions as panic_print (utils/stdio.h). Returns characters logged. */
int klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Total bytes ever logged. A reader's position is an offset into that count,
 * which is what makes "give me what I have not seen" a single number. */
uint32_t klog_total(void);

/* Copies up to `length` bytes starting at *offset into `out`. If that offset
 * has already been overwritten, *offset is moved up to the oldest byte still
 * retained before copying, so the caller always learns where what it got
 * actually starts. Returns the number of bytes copied; 0 at the end. */
uint32_t klog_read(uint32_t *offset, char *out, uint32_t length);

#endif /* UTILS_KLOG_H */
