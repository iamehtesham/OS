/* dummy.elf -- a standalone ring-3 program, loaded from the initrd at runtime.
 *
 * Unlike src/user/ipc_demo.c this is NOT linked into the kernel image. It is a
 * separate ELF executable: the kernel reads it through the VFS, parses its
 * program headers, and maps each segment into a page directory of its own. So
 * none of the .utext / .urodata / u_text() contortions the in-kernel ring-3
 * code needs apply here. Every page of this binary belongs to the process and
 * is user-accessible, which means ordinary string literals simply work.
 *
 * It spins in a while(1) loop as its steady state, but proves three things
 * about the loader first: that .data arrived from the file, that .bss exists
 * and reads as zero although it occupies no file bytes at all, and that .bss
 * is writable. */

#include <stdint.h>

#include "sys/syscall_abi.h"

/* ---- system calls ------------------------------------------------------- */

static int32_t sys_print(const char *text)
{
    int32_t result;

    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_PRINT), "b"(text) : "memory");

    return result;
}

/* ---- the two things the loader has to get right ------------------------- */

/* Initialised, so these bytes exist in the file and must be copied in. */
static volatile uint32_t data_magic = 0xC0FFEE01u;

/* Uninitialised, so these bytes exist NOWHERE in the file: 32 KiB that the
 * loader must allocate frames for and zero, purely because p_memsz says so.
 * Eight pages, well past the point where a loader that used p_filesz would
 * have stopped. */
#define BSS_WORDS 8192u
#define BSS_BYTES (BSS_WORDS * 4u)

static volatile uint32_t bss_probe[BSS_WORDS];

/* ---- string building, since there is no libc ---------------------------- */

static char *append(char *out, const char *text)
{
    while (*text != '\0') {
        *out++ = *text++;
    }

    return out;
}

static char *append_dec(char *out, uint32_t value)
{
    char     digits[10];
    uint32_t count = 0;

    do {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);

    while (count > 0u) {
        *out++ = digits[--count];
    }

    return out;
}

static char *append_hex(char *out, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";

    *out++ = '0';
    *out++ = 'x';

    for (int32_t shift = 28; shift >= 0; shift -= 4) {
        *out++ = digits[(value >> shift) & 0xFu];
    }

    return out;
}

static void spin(uint32_t iterations)
{
    /* hlt is privileged, so a busy loop is the only way ring 3 can pace
     * itself. The timer still preempts this. */
    for (volatile uint32_t i = 0; i < iterations; i++) {
    }
}

/* ---- entry -------------------------------------------------------------- */

void _start(void)
{
    char  line[160];
    char *p = line;

    /* Read every .bss word before writing any of it. volatile keeps the
     * compiler from assuming what it already knows about a zero-initialised
     * array and folding the whole loop away. */
    uint32_t nonzero = 0;

    for (uint32_t i = 0; i < BSS_WORDS; i++) {
        if (bss_probe[i] != 0u) {
            nonzero++;
        }
    }

    p = append(p, "  [dummy.elf] .data=");
    p = append_hex(p, data_magic);
    p = append(p, data_magic == 0xC0FFEE01u ? " ok" : " BAD");
    p = append(p, ", .bss ");
    p = append_dec(p, BSS_BYTES);
    p = append(p, " B nonzero=");
    p = append_dec(p, nonzero);
    p = append(p, nonzero == 0u ? " ok" : " BAD");
    p = append(p, "\n");
    *p = '\0';

    sys_print(line);

    /* The first and last words are eight pages apart, so this only holds if
     * every frame in between was mapped writable, not just the first. */
    bss_probe[0]             = 0x5A5A5A5Au;
    bss_probe[BSS_WORDS - 1] = 0xA5A5A5A5u;

    uint32_t beat = 0;

    for (;;) {
        spin(40000000u);

        beat++;

        const int written_ok = bss_probe[0] == 0x5A5A5A5Au &&
                               bss_probe[BSS_WORDS - 1] == 0xA5A5A5A5u;

        p = line;
        p = append(p, "  [dummy.elf] beat ");
        p = append_dec(p, beat);
        p = append(p, ", .bss writable ");
        p = append(p, written_ok ? "ok" : "BAD");
        p = append(p, "\n");
        *p = '\0';

        sys_print(line);
    }
}
