#include <stdbool.h>
#include <stdint.h>

#include "mm/paging.h"
#include "sys/uaccess.h"
#include "utils/string.h"

typedef bool (*page_check_t)(uint32_t virtual_addr);

/* Applies the predicate to every page in [start, start + length). Steps by
 * page rather than by byte, so a 44-byte struct costs one or two table walks
 * rather than forty-four. */
static bool range_ok(uint32_t start, uint32_t length, page_check_t check)
{
    if (length == 0) {
        return true;
    }

    if (start + length < start) {
        return false; /* wraps past the top of the address space */
    }

    const uint32_t last_page = (start + length - 1u) & PAGE_FRAME_MASK;

    for (uint32_t page = start & PAGE_FRAME_MASK;; page += PAGE_SIZE) {
        if (!check(page)) {
            return false;
        }

        if (page >= last_page) {
            return true;
        }
    }
}

bool user_range_readable(uint32_t user_addr, uint32_t length)
{
    return range_ok(user_addr, length, paging_user_can_read);
}

bool user_range_writable(uint32_t user_addr, uint32_t length)
{
    return range_ok(user_addr, length, paging_user_can_write);
}

bool copy_from_user(void *kernel_dst, uint32_t user_src, uint32_t length)
{
    if (!user_range_readable(user_src, length)) {
        return false;
    }

    kmemcpy(kernel_dst, (const void *)(uintptr_t)user_src, length);

    return true;
}

bool copy_to_user(uint32_t user_dst, const void *kernel_src, uint32_t length)
{
    if (!user_range_writable(user_dst, length)) {
        return false;
    }

    kmemcpy((void *)(uintptr_t)user_dst, kernel_src, length);

    return true;
}
