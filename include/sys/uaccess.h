#ifndef SYS_UACCESS_H
#define SYS_UACCESS_H

#include <stdbool.h>
#include <stdint.h>

/* The only sanctioned way for ring 0 to touch ring-3 memory.
 *
 * Every function here walks the page tables for every page the range spans
 * before moving a byte, in the direction of the access: a read needs present
 * + user, a write additionally needs the writable bit. Validating once at the
 * base address is not enough -- a small struct can straddle a page boundary,
 * and the next page may be unmapped or read-only. Ranges that wrap the
 * address space are refused. */

bool user_range_readable(uint32_t user_addr, uint32_t length);
bool user_range_writable(uint32_t user_addr, uint32_t length);

/* Return false without copying anything if any page fails the check. */
bool copy_from_user(void *kernel_dst, uint32_t user_src, uint32_t length);
bool copy_to_user(uint32_t user_dst, const void *kernel_src, uint32_t length);

#endif /* SYS_UACCESS_H */
