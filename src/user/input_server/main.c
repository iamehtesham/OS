/* The input server: a focus manager.
 *
 * Every keystroke the driver decodes arrives here exactly once, and leaves for
 * exactly one process -- whichever holds focus. Applications subscribe by
 * sending a message; the kernel-stamped sender pid is the subscription, so
 * nothing here trusts a pid an application claims. Tab is consumed rather than
 * forwarded and moves focus to the next subscriber.
 *
 * The one hard case is a subscriber that has died. There is no notification
 * for that: the kernel marks a faulting task dead at once and reaps it later,
 * and the only thing a process can observe is that sending to it fails with
 * IPC_ERR_NO_TASK -- deterministically, both before and after reaping, because
 * pids are never reused. So death is detected at the first send that would
 * have reached the corpse: the pid is unsubscribed, focus moves to the next
 * subscriber, and the keystroke that exposed it is dropped rather than
 * delivered to whoever comes next. The user was typing into the process that
 * died; handing that character to a different one is the wrong recovery.
 *
 * A full mailbox is a different failure and gets a different answer: the
 * application is alive and merely slow, so the send is retried a bounded
 * number of times and then the key is dropped with the subscription intact.
 *
 * Keys reach this server through a reserved mailbox slot that only the driver
 * can fill, and reach each application through one that only this server can
 * fill. Without that, a review showed an unprivileged process looping a send
 * at this pid taking every keystroke on the machine with it. */

#include <stdbool.h>
#include <stdint.h>

#include "ipc/input_proto.h"
#include "ipc/ipc.h"
#include "ipc/kbd_proto.h"
#include "ipc/vga_proto.h"
#include "user/ulib.h"

static uint32_t subscribed_pids[INPUT_MAX_SUBSCRIBERS];
static uint32_t subscriber_count;

/* 0 means nothing has focus. No ring-3 process is ever pid 0. */
static uint32_t focused_pid;

static uint32_t keys_forwarded;
static uint32_t keys_ignored;       /* nobody subscribed */
static uint32_t keys_dropped_full;  /* focused app alive but not reading */
static uint32_t keys_dropped_dead;  /* focused app found dead */
static uint32_t notes_dropped_full; /* a focus note an app did not take */

/* Tells the console which terminal to show: the focused process's, or the
 * system console for pid 0. Focus and screen move together, so a Tab is
 * both; Escape is only this. Bounded and unchecked -- a console that is gone
 * or busy costs the user a screen switch, not this server its loop. */
static void show_terminal_of(uint32_t pid)
{
    ipc_message_t msg;

    msg.sender_pid   = 0;
    msg.receiver_pid = 0;
    msg.type         = MSG_SWITCH_VT;
    u_memset(msg.data, 0, IPC_PAYLOAD_SIZE);
    u_store32(msg.data, pid);

    (void)u_send_bounded(VGA_SERVER_PID, &msg, INPUT_SEND_ATTEMPTS);
}

static char line[128];

static void say(const char *before, uint32_t number, const char *after)
{
    char *p = line;

    p = u_append(p, U_LIMIT(line), "  [input] ");
    p = u_append(p, U_LIMIT(line), before);
    p = u_append_dec(p, U_LIMIT(line), number);
    p = u_append(p, U_LIMIT(line), after);
    p = u_append(p, U_LIMIT(line), "\n");
    *p = '\0';

    u_print(line);
}

static int32_t index_of(uint32_t pid)
{
    for (uint32_t i = 0; i < subscriber_count; i++) {
        if (subscribed_pids[i] == pid) {
            return (int32_t)i;
        }
    }

    return -1;
}

static void unsubscribe(uint32_t pid)
{
    const int32_t at = index_of(pid);

    if (at < 0) {
        return;
    }

    /* Close the gap so subscription order -- which is rotation order -- is
     * preserved for everyone else. */
    for (uint32_t i = (uint32_t)at; i + 1u < subscriber_count; i++) {
        subscribed_pids[i] = subscribed_pids[i + 1u];
    }

    subscriber_count--;
}

typedef enum {
    DELIVERED,
    DROPPED_FULL, /* alive, not reading; still subscribed */
    DEAD,         /* unsubscribed by this call */
} delivery_t;

/* The one place a send result is interpreted, so the dead-pid recovery cannot
 * be forgotten on one path and remembered on another. */
static delivery_t deliver(uint32_t pid, ipc_message_t *msg)
{
    const int32_t result = u_send_bounded(pid, msg, INPUT_SEND_ATTEMPTS);

    if (result == IPC_OK) {
        return DELIVERED;
    }

    if (result == IPC_ERR_NO_TASK) {
        /* Dead, reaped, or never existed. All three mean the same thing to a
         * focus manager: this pid will never read another key. */
        say("pid ", pid, " is gone; unsubscribed");
        unsubscribe(pid);
        return DEAD;
    }

    /* IPC_ERR_FULL after the retries. Not a reason to give up on the process. */
    return DROPPED_FULL;
}

static delivery_t notify_focus(uint32_t pid, uint8_t gained)
{
    ipc_message_t note;

    note.sender_pid   = 0;
    note.receiver_pid = 0;
    note.type         = MSG_FOCUS_SWITCH;
    u_memset(note.data, 0, IPC_PAYLOAD_SIZE);
    note.data[0] = gained;

    const delivery_t result = deliver(pid, &note);

    /* A note nobody took is worth a line: the application now believes
     * something about its focus that is no longer true, and a silent drop is
     * the one outcome an operator could never diagnose. */
    if (result == DROPPED_FULL) {
        notes_dropped_full++;
        say("pid ", pid, gained ? " did not take its focus-gained note" : " did not take its focus-lost note");
    }

    return result;
}

/* Tries to make `pid` the focused process. The candidate is told FIRST, and
 * only if it turns out to be alive is the previous holder told it lost focus
 * and the change announced. In that order, because the GAINED send is what
 * discovers a dead candidate: told the other way round, rotating onto a corpse
 * would tell a live application it had lost focus, then hand focus straight
 * back with a second note, when nothing had happened at all.
 *
 * Returns false if the candidate was dead; it has been unsubscribed and focus
 * is where it was. */
static bool focus_on(uint32_t pid)
{
    const uint32_t previous = focused_pid;

    if (notify_focus(pid, INPUT_FOCUS_GAINED) == DEAD) {
        return false;
    }

    focused_pid = pid;

    if (previous != 0 && previous != pid && index_of(previous) >= 0) {
        (void)notify_focus(previous, INPUT_FOCUS_LOST);
    }

    show_terminal_of(pid);
    say("focus -> pid ", pid, "");

    return true;
}

/* Ensures focused_pid names a live subscriber, or is 0 when there are none.
 * Tries the subscriber at `from` first. A loop rather than a single attempt
 * because a candidate can turn out dead, which removes it and slides the next
 * one into the same index; every iteration either returns or shrinks the
 * table by one, so it terminates. */
static void settle_focus(uint32_t from)
{
    while (subscriber_count > 0) {
        if (index_of(focused_pid) >= 0) {
            return;
        }

        if (focus_on(subscribed_pids[from % subscriber_count])) {
            return;
        }
    }

    if (focused_pid != 0) {
        focused_pid = 0;
        show_terminal_of(0);
        u_print("  [input] no subscribers left; keys are ignored until one appears\n");
    }
}

static void handle_subscribe(uint32_t pid)
{
    if (index_of(pid) >= 0) {
        return; /* already in; a repeat is not a second slot */
    }

    if (subscriber_count == INPUT_MAX_SUBSCRIBERS) {
        say("table full; pid ", pid, " refused");
        return;
    }

    subscribed_pids[subscriber_count++] = pid;
    say("pid ", pid, " subscribed");

    /* The first subscriber gets focus at once, so typing works the moment one
     * application exists rather than after a Tab nobody knows to press. */
    if (subscriber_count == 1) {
        settle_focus(0);
    }
}

/* Tab. Walks forward from the current holder and stops at the first live
 * subscriber that is not the current holder. A dead one is unsubscribed by the
 * attempt, which slides the following subscriber into the same index, so the
 * walk simply tries that index again rather than snapping back to the front. */
static void rotate_focus(void)
{
    const int32_t at   = index_of(focused_pid);
    uint32_t      next = (at < 0) ? 0u : ((uint32_t)at + 1u) % subscriber_count;

    while (subscriber_count > 0) {
        next %= subscriber_count;

        if (subscribed_pids[next] == focused_pid) {
            return; /* back where we started: nobody else is alive */
        }

        if (focus_on(subscribed_pids[next])) {
            return;
        }
    }

    settle_focus(0); /* everyone was dead; this records that nothing has focus */
}

static void handle_keypress(const ipc_message_t *msg)
{
    /* Keys come from the driver and from nowhere else. Without this any
     * process could type into any other by sending it a MSG_KEYPRESS via
     * this server. */
    if (msg->sender_pid != KBD_SERVER_PID) {
        return;
    }

    if (subscriber_count == 0) {
        keys_ignored++;
        return;
    }

    const char c = (char)msg->data[0];

    if (c == INPUT_FOCUS_KEY) {
        rotate_focus();
        return;
    }

    if (c == INPUT_CONSOLE_KEY) {
        show_terminal_of(0); /* look at the console; focus stays where it is */
        return;
    }

    /* Should already hold; this is the cheap insurance that it does. */
    if (index_of(focused_pid) < 0) {
        settle_focus(0);

        if (subscriber_count == 0) {
            keys_ignored++;
            return;
        }
    }

    ipc_message_t forward;

    forward.sender_pid   = 0;
    forward.receiver_pid = 0;
    forward.type         = MSG_KEYPRESS;
    u_memcpy(forward.data, msg->data, IPC_PAYLOAD_SIZE);

    const uint32_t   target = focused_pid;
    const int32_t    at     = index_of(target);
    const delivery_t result = deliver(target, &forward);

    switch (result) {
    case DELIVERED:
        keys_forwarded++;
        break;

    case DROPPED_FULL:
        keys_dropped_full++;
        break;

    case DEAD:
        /* The key is gone with the process it was meant for. Focus moves to
         * whoever followed the dead process in the table -- which, after the
         * removal, is the element now sitting at its old index. */
        keys_dropped_dead++;
        settle_focus((uint32_t)at);
        break;
    }
}

void _start(void)
{
    /* The driver's keys get the reserved slot. Everything else -- including a
     * process trying to keep this server busy -- shares the ordinary one. */
    (void)u_trust_sender(KBD_SERVER_PID);

    u_print("  [input] focus manager up; Tab moves focus between subscribers\n");

    ipc_message_t msg;

    for (;;) {
        if (u_recv(&msg) != IPC_OK) {
            continue;
        }

        switch (msg.type) {
        case MSG_SUBSCRIBE_INPUT:
            /* The kernel stamped sender_pid; nothing in the payload is used,
             * so there is nothing a subscriber could lie about. */
            handle_subscribe(msg.sender_pid);
            break;

        case MSG_KEYPRESS:
            handle_keypress(&msg);
            break;

        default:
            break; /* not a message this server speaks */
        }
    }
}
