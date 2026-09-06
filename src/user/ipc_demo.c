/* Everything in this file runs at CPL 3.
 *
 * Two rules follow from that. First, every function and every constant goes in
 * a user section: only .utext and .urodata are mapped for ring 3, and an
 * instruction fetch or a read anywhere else is a page fault. Second, no string
 * literals -- GCC would place them in .rodata.str, which ring 3 cannot read --
 * and no struct initialisers that might tempt it into a memset call. Constants
 * are declared explicitly with the section attribute instead. */

#include <stdint.h>

#include "ipc/ipc.h"
#include "sys/syscall.h"
#include "user/ipc_demo.h"

#define USER_TEXT   __attribute__((section(".utext")))
#define USER_RODATA __attribute__((section(".urodata")))

/* ---- system call wrappers ----------------------------------------------- */

/* EAX carries the call number in and the result out; EBX and ECX carry the
 * arguments. The memory clobber stops GCC caching anything across a call that
 * the kernel may read from or write to. */
static USER_TEXT int32_t u_print(const char *text)
{
    int32_t result;

    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_PRINT), "b"(text) : "memory");

    return result;
}

static USER_TEXT int32_t u_send(uint32_t target_pid, const ipc_message_t *msg)
{
    int32_t result;

    __asm__ volatile ("int $0x80"
                      : "=a"(result)
                      : "a"(SYS_SEND), "b"(target_pid), "c"(msg)
                      : "memory");

    return result;
}

static USER_TEXT int32_t u_recv(ipc_message_t *msg)
{
    int32_t result;

    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(SYS_RECV), "b"(msg) : "memory");

    return result;
}

static USER_TEXT void u_yield(void)
{
    int32_t ignored;

    __asm__ volatile ("int $0x80" : "=a"(ignored) : "a"(SYS_YIELD) : "memory");
}

/* ---- tiny userland helpers ---------------------------------------------- */

/* Passes a string pointer through an empty asm barrier so its contents are
 * opaque to the optimizer. Without this GCC folds a small const array into
 * immediates and emits its OWN copy of the bytes in the ordinary .rodata --
 * the section attribute only governs the object it no longer references --
 * and ring 3 then page-faults reading a supervisor page. */
static USER_TEXT const char *u_text(const char *text)
{
    __asm__ volatile ("" : "+r"(text));

    return text;
}

static USER_TEXT void u_delay(void)
{
    /* hlt is privileged, so a busy loop is the only way ring 3 can pace
     * itself. The timer still preempts this. */
    for (volatile uint32_t i = 0; i < 30000000u; i++) {
    }
}

/* Appends the decimal form of value at out and returns the new end. */
static USER_TEXT char *u_utoa(uint32_t value, char *out)
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

static USER_TEXT char *u_append(char *out, const char *text)
{
    while (*text != '\0') {
        *out++ = *text++;
    }

    return out;
}

/* ---- the two programs --------------------------------------------------- */

static USER_RODATA const char text_recv[]     = "  [B pid ";
static USER_RODATA const char text_from[]     = "] got ";
static USER_RODATA const char text_from2[]    = " from pid ";
static USER_RODATA const char text_nl[]       = "\n";
static USER_RODATA const char text_bad_pid[]  = "  [A] send to pid 99 -> IPC_ERR_NO_TASK (expected)\n";
static USER_RODATA const char text_send_err[] = "  [A] send failed\n";

#define IPC_DEMO_MSG_COUNTER 1u

void USER_TEXT ipc_demo_receiver(void)
{
    ipc_message_t msg;
    char          line[64];

    for (;;) {
        if (u_recv(&msg) != IPC_OK) {
            continue;
        }

        /* The counter travels in the first four payload bytes, little
         * endian, because that is how the sender stored it. */
        const uint32_t counter = (uint32_t)msg.data[0] | ((uint32_t)msg.data[1] << 8) |
                                 ((uint32_t)msg.data[2] << 16) |
                                 ((uint32_t)msg.data[3] << 24);

        char *p = line;

        p  = u_append(p, u_text(text_recv));
        p  = u_utoa(msg.receiver_pid, p);
        p  = u_append(p, u_text(text_from));
        p  = u_utoa(counter, p);
        p  = u_append(p, u_text(text_from2));
        p  = u_utoa(msg.sender_pid, p);
        p  = u_append(p, u_text(text_nl));
        *p = '\0';

        u_print(line);
    }
}

void USER_TEXT ipc_demo_sender(void)
{
    ipc_message_t msg;
    uint32_t      counter = 1;

    /* Fill the whole struct by hand rather than with an initialiser, so GCC
     * has no reason to reach for memset. The kernel overwrites sender_pid and
     * receiver_pid regardless of what is put here -- that is the point. */
    msg.sender_pid   = 0xFFFFFFFFu; /* a lie the kernel must correct */
    msg.receiver_pid = 0;
    msg.type         = IPC_DEMO_MSG_COUNTER;

    for (uint32_t i = 4; i < IPC_PAYLOAD_SIZE; i++) {
        msg.data[i] = 0;
    }

    /* Once: prove a bad pid is refused rather than faulting. */
    if (u_send(99u, &msg) == IPC_ERR_NO_TASK) {
        u_print(u_text(text_bad_pid));
    }

    for (;;) {
        msg.data[0] = (uint8_t)(counter & 0xFFu);
        msg.data[1] = (uint8_t)((counter >> 8) & 0xFFu);
        msg.data[2] = (uint8_t)((counter >> 16) & 0xFFu);
        msg.data[3] = (uint8_t)((counter >> 24) & 0xFFu);

        const int32_t result = u_send(IPC_DEMO_RECEIVER_PID, &msg);

        if (result == IPC_ERR_FULL) {
            /* The receiver has not collected the last one. Give it the CPU
             * rather than spinning on the slot. */
            u_yield();
            continue;
        }

        if (result != IPC_OK) {
            u_print(u_text(text_send_err));
            u_delay();
            continue;
        }

        counter++;
        u_delay();
    }
}
