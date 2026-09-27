/* The kernel alarm, tested from ring 3: SYS_ALARM and the MSG_TIMER it becomes.
 *
 * The DHCP client is the alarm's real user, but a DHCP exchange cannot aim at
 * the cases that matter -- an alarm that expires while its owner is busy, then
 * is cancelled or re-armed before the owner looks; the order a timer and a
 * waiting message come out in -- so they are aimed at here. The program
 * spins in ring 3 so that ticks pass while it is running rather than blocked,
 * and sends messages to itself as the rival to the timer. Run it from the
 * shell (alarmtest.elf); regress.py's `alarm` scenario reads the last line.
 *
 * It learns its own pid from the first timer, which the kernel addresses to
 * it, and it learns how fast the timestamp counter runs by timing that same
 * first alarm, so "spin for five ticks" means five ticks and not a guess. */

#include <stdbool.h>
#include <stdint.h>

#include "ipc/ipc.h"
#include "sys/syscall_abi.h"
#include "user/ulib.h"

#define MSG_MARKER 200u /* what this program sends itself */

static uint32_t self_pid;
static uint64_t tsc_per_tick;
static uint32_t passed;
static uint32_t failed;

static bool is_timer(const ipc_message_t *m)
{
    return m->type == MSG_TIMER && m->sender_pid == IPC_KERNEL_PID;
}

static bool is_marker(const ipc_message_t *m)
{
    return m->type == MSG_MARKER && m->sender_pid == self_pid;
}

/* Busy for `ticks` timer ticks, by the timestamp counter. */
static void spin_ticks(uint32_t ticks)
{
    const uint64_t until = u_rdtsc() + tsc_per_tick * ticks;

    while (u_rdtsc() < until) {
    }
}

static void send_marker(void)
{
    ipc_message_t m;

    u_memset(&m, 0, sizeof(m));
    m.type = MSG_MARKER;
    u_send(self_pid, &m);
}

static void verdict(const char *name, bool ok)
{
    u_printf("  alarm %s: %s\n", name, ok ? "ok" : "FAIL");

    if (ok) {
        passed++;
    } else {
        failed++;
    }
}

void _start(void)
{
    ipc_message_t m;

    /* (a) An alarm expiring while its owner is blocked in recv wakes it, as a
     *     MSG_TIMER from the kernel with nothing in the payload -- the message
     *     is built on the kernel stack, and any byte not zeroed there would be
     *     leaked stack. A new task's kernel stack comes from kmalloc, which
     *     does not zero it, but on a fresh boot it is carved from heap memory
     *     never used before and is zero anyway, so leaked bytes would go unseen
     *     on the first recv; printing first leaves a message full of text on
     *     the stack where the timer will be built. Timing the
     *     alarm calibrates the counter: over 16 ticks, so that per-tick is a
     *     shift -- a 64-bit division would call libgcc's __udivdi3, which this
     *     freestanding link does not have (at -O2 GCC happens to avoid it). */
    u_print("alarm test: starting\n");

    const uint64_t start = u_rdtsc();
    const bool     armed = u_alarm(16);

    u_recv(&m);

    const uint64_t took = u_rdtsc() - start;
    bool           zero = true;

    for (uint32_t i = 0; i < sizeof(m.data); i++) {
        zero = zero && m.data[i] == 0u;
    }

    self_pid     = m.receiver_pid;
    tsc_per_tick = took >> 4;

    verdict("(a) wakes a blocked owner, from the kernel, zero payload",
            armed && is_timer(&m) && zero && tsc_per_tick != 0u);

    if (tsc_per_tick == 0u) {
        u_print("alarm test: cannot calibrate; stopping\n");
        u_exit(1);
    }

    /* (b) It expires while the owner is running, and a cancel afterwards
     *     throws the uncollected expiry away. */
    u_alarm(1);
    spin_ticks(5);
    u_alarm(0);
    send_marker();
    u_recv(&m);
    verdict("(b) a cancel discards an expiry not yet collected", is_marker(&m));

    /* (c) So does re-arming: the new alarm is the only one. */
    u_alarm(1);
    spin_ticks(5);
    u_alarm(1000);
    send_marker();
    u_recv(&m);
    verdict("(c) a re-arm discards an expiry not yet collected", is_marker(&m));
    u_alarm(0);

    /* (d) An expired alarm comes out before a message already waiting. The
     *     second recv is made only if the first was the timer: then the
     *     marker is certainly waiting, where otherwise nothing might be, and
     *     the test would hang instead of saying FAIL. */
    u_alarm(1);
    spin_ticks(5);
    send_marker();
    u_recv(&m);

    const bool timer_first = is_timer(&m);
    bool       then_marker = false;

    if (timer_first) {
        u_recv(&m);
        then_marker = is_marker(&m);
    }

    verdict("(d) the timer is delivered before the mailbox", timer_first && then_marker);

    /* (e) A refused call -- 2^31 ticks is too long -- changes nothing, not
     *     even an expiry waiting to be collected. */
    u_alarm(1);
    spin_ticks(5);

    const bool refused = !u_alarm(SYS_ALARM_MAX_TICKS + 1u);

    send_marker();
    u_recv(&m);

    const bool still_pending = is_timer(&m);
    bool       marker_after  = false;

    if (still_pending) { /* as in (d): only then is a second recv safe */
        u_recv(&m);
        marker_after = is_marker(&m);
    }

    verdict("(e) 2^31 ticks is refused and disturbs nothing",
            refused && still_pending && marker_after);

    /* (f) Cancelled before it is due, it never fires. */
    u_alarm(10);
    u_alarm(0);
    spin_ticks(20);
    send_marker();
    u_recv(&m);
    verdict("(f) a cancelled alarm never fires", is_marker(&m));

    /* (g) Arming again replaces the earlier deadline rather than adding one. */
    u_alarm(10);
    u_alarm(1000);
    spin_ticks(20);
    send_marker();
    u_recv(&m);
    verdict("(g) arming again replaces the earlier deadline", is_marker(&m));
    u_alarm(0);

    /* (h) The longest alarm there is, SYS_ALARM_MAX_TICKS (2^31 - 1), is
     *     accepted: a lease that long is timed in pieces of exactly this much,
     *     and a refusal would leave it never ending. Cancelled at once. */
    const bool longest = u_alarm(SYS_ALARM_MAX_TICKS);

    u_alarm(0);
    verdict("(h) 2^31 - 1 ticks, the longest alarm, is accepted", longest);

    /* (i) SYS_TICKS counts the ticks alarms count: across a 10-tick alarm it
     *     advances by 10 -- 11 if a tick fell between the reading and the
     *     arming, 12 if the scheduler was slow to run this task once woken. */
    const uint32_t before = u_ticks();

    u_alarm(10);
    u_recv(&m);

    const uint32_t took_ticks = u_ticks() - before;

    verdict("(i) the tick count advances with the alarm",
            is_timer(&m) && took_ticks >= 10u && took_ticks <= 12u);

    u_printf("alarm test: %u of %u passed\n", passed, passed + failed);
    u_exit((int32_t)failed);
}
