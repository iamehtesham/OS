#include <stddef.h>
#include <stdint.h>

#include "cpu/isr.h"
#include "mm/paging.h"
#include "sys/syscall.h"
#include "utils/stdio.h"

static uint32_t call_count;

/* Prints a string whose pointer came from ring 3, and therefore cannot be
 * trusted to be terminated, mapped, or even to belong to the caller.
 *
 * Two things make it safe. The length is bounded, so an unterminated string
 * cannot walk the kernel off the end of memory. And every page is checked for
 * user accessibility BEFORE it is read, so a pointer aimed at kernel memory is
 * rejected rather than dereferenced -- without that check this call would be a
 * read primitive for arbitrary kernel data. The page is re-checked whenever the
 * string crosses a boundary, since the next page may not be mapped at all.
 *
 * Returns the number of characters printed, or 0 if the pointer was rejected. */
static uint32_t sys_print(uint32_t user_string)
{
    if (user_string == 0) {
        return 0;
    }

    const char *const text = (const char *)(uintptr_t)user_string;
    uint32_t          checked_page = ~0u;
    uint32_t          written      = 0;

    for (uint32_t i = 0; i < SYSCALL_MAX_STRING; i++) {
        const uint32_t address = user_string + i;
        const uint32_t page    = address & PAGE_FRAME_MASK;

        if (page != checked_page) {
            if (!paging_user_can_read(address)) {
                kprintf("\n[sys_print: rejected pointer %p]\n", (void *)(uintptr_t)address);
                return written;
            }

            checked_page = page;
        }

        if (text[i] == '\0') {
            break;
        }

        kprintf("%c", text[i]);
        written++;
    }

    return written;
}

void syscall_handler(struct registers *regs)
{
    call_count++;

    switch (regs->eax) {
    case SYS_PRINT:
        /* Result goes back in eax: popa restores the frame on the way out, so
         * writing the field here is what the caller sees in its own EAX. */
        regs->eax = sys_print(regs->ebx);
        break;

    default:
        kprintf("\n[syscall: unknown call %u from cs=0x%x]\n", regs->eax, regs->cs);
        regs->eax = (uint32_t)-1;
        break;
    }
}

uint32_t syscall_count(void)
{
    return call_count;
}
