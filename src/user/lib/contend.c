/* The contended write, shared by both processes so that each is provably
 * running the same code against the same page -- only the string differs.
 *
 * The write goes in one byte at a time with a pause between bytes, which is
 * what makes the race observable rather than theoretical. A single-instruction
 * copy would almost never be interrupted mid-way; a byte-at-a-time copy that
 * spans several timeslices is interrupted constantly, so an unsynchronised run
 * interleaves the two strings on nearly every round. */

#include "ipc/mutex_proto.h"
#include "user/mutex.h"
#include "user/ulib.h"

/* Long enough that a write takes an appreciable fraction of a 10 ms timeslice
 * -- roughly half of one, measured -- so the timer regularly lands in the
 * middle of a write rather than tidily between rounds. That is what turns the
 * race from theoretical into something that shows up on most rounds. */
#define PAUSE_PER_BYTE 20000u

uint32_t mutex_contend(struct mutex_shared *shared, const char *mine, bool use_lock)
{
    uint32_t corruptions = 0;

    for (uint32_t round = 0; round < MUTEX_ROUNDS; round++) {
        if (use_lock) {
            mutex_lock(&shared->lock);
        }

        /* ---- critical section ------------------------------------------- */
        uint32_t i = 0;

        for (; mine[i] != '\0'; i++) {
            shared->text[i] = mine[i];
            u_spin(PAUSE_PER_BYTE);
        }

        shared->text[i] = '\0';

        /* Read back what is actually in the page. If anything else wrote here
         * while this was in progress, the buffer now holds a mixture. */
        if (u_strcmp((const char *)shared->text, mine) != 0) {
            corruptions++;
        }
        /* ---- end critical section --------------------------------------- */

        if (use_lock) {
            mutex_unlock(&shared->lock);
        }

        /* Outside the lock, so the other process gets a fair chance at it
         * rather than this one immediately re-acquiring. */
        u_yield();
    }

    return corruptions;
}

void mutex_report(const char *who, uint32_t corruptions, bool use_lock)
{
    /* Static, like every other line buffer here: it lands in .bss, which the
     * ELF loader zero-fills, so there is no initialiser for GCC to turn into a
     * memset this freestanding link could not resolve. */
    static char line[192];

    char *p = line;

    p = u_append(p, U_LIMIT(line), "  [");
    p = u_append(p, U_LIMIT(line), who);
    p = u_append(p, U_LIMIT(line), "] ");
    p = u_append_dec(p, U_LIMIT(line), MUTEX_ROUNDS);
    p = u_append(p, U_LIMIT(line), use_lock ? " locked writes, " : " UNLOCKED writes, ");
    p = u_append_dec(p, U_LIMIT(line), corruptions);
    p = u_append(p, U_LIMIT(line), " corrupted, ");
    p = u_append_dec(p, U_LIMIT(line), mutex_contention_count());
    p = u_append(p, U_LIMIT(line), " lock waits -> ");
    p = u_append(p, U_LIMIT(line), corruptions == 0 ? "CLEAN\n" : "TORN\n");
    *p = '\0';

    u_print(line);
}
