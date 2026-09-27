/* A DHCP client (RFC 2131), in ring 3: how this machine gets an address.
 *
 * It boots with none -- 0.0.0.0 -- and asks. Four messages, "DORA": the client
 * broadcasts a DISCOVER; a server answers with an OFFER naming an address; the
 * client broadcasts a REQUEST for that address, naming the server it chose;
 * the server confirms with an ACK. All four carry the transaction id the
 * client picked, which is how the client knows an answer is for its question.
 * Only after the ACK does the machine have an address, and only then does it
 * answer anything on the network at all.
 *
 * Every message from a server is a stranger's claim, so, as in ipv4.c and
 * udp.c, most of this file is refusal, and each check owns one thing. The
 * options are walked with every byte proved inside the message before it is
 * read, and nothing they say is acted on until the whole message has parsed
 * and made sense.
 *
 * Retransmission needs time, which ring 3 had none of: this is the first user
 * of the kernel's alarm (SYS_ALARM). DISCOVER is resent at 4, 8, 16, 32 and
 * then every 64 seconds, REQUEST four times at 4, 8, 16 and 32 before the
 * client gives up, each interval randomised by a second either way (RFC 2131
 * 4.1). The same alarm ends the lease when it runs out: there is no renewal,
 * so the address is dropped and asked for again. Whenever the client starts
 * over -- after a NAK, a give-up or the end of a lease -- it waits a second or
 * two first, so nothing a server sends can make it transmit faster than that. */

#include <stdbool.h>
#include <stdint.h>

#include "net/arp.h"
#include "net/byteorder.h"
#include "net/dhcp.h"
#include "net/ethernet.h"
#include "net/ipv4.h"
#include "net/netcfg.h"
#include "net/rtl8139.h"
#include "net/udp.h"
#include "sys/syscall_abi.h"
#include "user/ulib.h"

typedef enum {
    DHCP_STATE_INIT,       /* no transaction; the alarm will start one      */
    DHCP_STATE_SELECTING,  /* DISCOVER sent; waiting for an OFFER           */
    DHCP_STATE_REQUESTING, /* REQUEST sent; waiting for an ACK or a NAK     */
    DHCP_STATE_BOUND,      /* leased; the alarm is the lease running out    */
} dhcp_state_t;

static dhcp_state_t state;

/* The transaction in progress. `open` is what makes a reply eligible at all:
 * it is set when a DISCOVER starts a transaction and cleared when the
 * transaction ends -- in a lease, a NAK, or giving up -- so every message that
 * arrives in BOUND or INIT is refused by that one check. */
static bool     transaction_open;
static uint32_t transaction_id;

/* What the chosen OFFER said, in host order. */
static uint32_t offered_address;
static uint32_t offered_server;

/* Retransmission. `interval` is the backoff for the next wait, in seconds;
 * `requests_resent` counts REQUEST retransmissions. */
static uint32_t interval;
static uint32_t requests_resent;

/* A lease longer than one alarm can span (2^31 ticks, about 248 days at 100 Hz)
 * is timed in pieces; this is what remains after the current one. */
static uint64_t lease_ticks_left;

/* When the transaction's first REQUEST went out, in ticks. A lease runs from
 * then, not from when its ACK arrives (RFC 2131 4.4.1): the server started
 * counting when it answered, and a late ACK must not stretch the lease. */
static uint32_t request_sent;

/* The alarm as this client set it: whether one is armed and the tick it is
 * due at. The kernel delivers it only through recv, and a flood of frames can
 * keep the network server draining its ring and out of recv for as long as
 * the flood lasts, so the drain loop asks dhcp_poll whether it is due instead
 * of waiting to be told. */
static bool     alarm_armed;
static uint32_t alarm_due;

/* The message being sent. Static, and cleared before every build -- a REQUEST
 * is longer than a DISCOVER, and a DISCOVER built over one would otherwise
 * carry its leftover option bytes after the End. */
static uint8_t message[DHCP_BOOTP_MIN];

#define REQUEST_RETRANSMISSIONS 4u
#define DISCOVER_MAX_INTERVAL   64u
#define FIRST_INTERVAL          4u

/* ---- randomness ----------------------------------------------------------- */

/* There is no entropy source. The transaction id and the jitter come from the
 * timestamp counter -- which starts counting when the machine does, so it
 * differs from boot to boot -- mixed with this card's MAC, which differs from
 * machine to machine, through xorshift32. That is enough for two machines, or
 * two boots, not to pick the same id. It is not a secret, and does not need to
 * be one: every message carrying it is broadcast to the whole segment. */
static uint32_t random_state;

static uint32_t random32(void)
{
    const uint64_t tsc = u_rdtsc();

    if (random_state == 0u) {
        const uint8_t *const mac = rtl8139_mac();

        for (uint32_t i = 0; i < ETH_ALEN; i++) {
            random_state = (random_state << 8 | random_state >> 24) ^ mac[i];
        }
    }

    uint32_t x = random_state ^ (uint32_t)tsc ^ (uint32_t)(tsc >> 32);

    if (x == 0u) {
        x = 0x2545F491u; /* xorshift's one fixed point; any other seed will do */
    }

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    random_state = x;
    return x;
}

/* Every alarm goes through these two, so the client always knows when its
 * alarm is due. `ticks` is at least 1: an alarm of 0 would cancel it. */
static void arm(uint32_t ticks)
{
    alarm_due   = u_ticks() + ticks;
    alarm_armed = true;
    u_alarm(ticks);
}

static void disarm(void)
{
    alarm_armed = false;
    u_alarm(0);
}

/* Arms the alarm for `seconds`, randomised by up to a second either way -- so
 * machines that booted together do not retransmit in step. */
static void arm_jittered(uint32_t seconds)
{
    const uint32_t low    = seconds * SYS_ALARM_HZ - SYS_ALARM_HZ;
    const uint32_t spread = 2u * SYS_ALARM_HZ + 1u;
    const uint32_t ticks  = low + random32() % spread;

    arm(ticks == 0u ? 1u : ticks);
}

/* ---- logging -------------------------------------------------------------- */

static void drop(const char *reason)
{
    static char line[112];

    char *const limit = U_LIMIT(line);
    char       *out   = line;

    out = u_append(out, limit, "  [net] DHCP dropped: ");
    out = u_append(out, limit, reason);
    out = u_append(out, limit, "\n");
    *out = '\0';

    u_print(line);
}

/* "  [net] <what> <address><tail>" */
static void log_address(const char *what, uint32_t address, const char *tail)
{
    static char line[112];

    char *const limit = U_LIMIT(line);
    char       *out   = line;

    out = u_append(out, limit, "  [net] ");
    out = u_append(out, limit, what);
    out = net_append_ipv4(out, limit, address);
    out = u_append(out, limit, tail);
    out = u_append(out, limit, "\n");
    *out = '\0';

    u_print(line);
}

static void log_sent(const char *what)
{
    static char line[96];

    char *const limit = U_LIMIT(line);
    char       *out   = line;

    out = u_append(out, limit, "  [net] DHCP ");
    out = u_append(out, limit, what);
    out = u_append(out, limit, " sent, xid ");
    out = u_append_hex(out, limit, transaction_id);
    out = u_append(out, limit, "\n");
    *out = '\0';

    u_print(line);
}

/* ---- building and sending -------------------------------------------------- */

static uint8_t *put_option(uint8_t *p, uint8_t code, uint8_t length)
{
    *p++ = code;
    *p++ = length;
    return p;
}

/* Builds a DISCOVER or a REQUEST into `message` and broadcasts it, from
 * 0.0.0.0:68 to 255.255.255.255:67. The xid is the transaction's, the same in
 * every message of it -- the brief's constraint, and RFC 2131's. */
static bool send_message(uint8_t type)
{
    u_memset(message, 0, sizeof(message));

    dhcp_header_t *const header = (dhcp_header_t *)message;

    header->op    = DHCP_OP_REQUEST;
    header->htype = DHCP_HTYPE_ETHERNET;
    header->hlen  = ETH_ALEN;
    header->xid   = htonl(transaction_id);

    /* secs stays 0, which RFC 2131 allows, and which keeps it equal in a
     * REQUEST and the DISCOVER before it, as 3.1 requires. BROADCAST asks
     * the server to broadcast its answers: until the lease is bound this
     * machine accepts no unicast at all (4.1: a client that cannot receive
     * unicast before it is configured SHOULD set it). ciaddr, yiaddr, siaddr
     * and giaddr stay 0. */
    header->flags = htons(DHCP_FLAG_BROADCAST);

    u_memcpy(header->chaddr, rtl8139_mac(), ETH_ALEN);

    uint8_t *p = message + DHCP_HEADER_SIZE;

    /* The cookie as a 32-bit number goes out through htonl(), like any other
     * field. Copied as bytes, because `p` is at offset 236 of a static buffer
     * and is not a uint32_t *. */
    const uint32_t cookie = htonl(DHCP_MAGIC_COOKIE);

    u_memcpy(p, &cookie, DHCP_COOKIE_SIZE);
    p += DHCP_COOKIE_SIZE;

    p    = put_option(p, DHCP_OPT_MESSAGE_TYPE, 1);
    *p++ = type;

    /* A REQUEST names the address and the server it is accepting (RFC 2131
     * 4.3.2: both MUST be present in the SELECTING state). Every other server
     * that made an offer sees it and withdraws. */
    if (type == DHCP_REQUEST) {
        p = put_option(p, DHCP_OPT_REQUESTED_IP, 4);
        net_store_be32(p, offered_address);
        p += 4;

        p = put_option(p, DHCP_OPT_SERVER_ID, 4);
        net_store_be32(p, offered_server);
        p += 4;
    }

    /* What this client wants to be told: its subnet mask and its router. A
     * server may send only what is asked for. Then the largest DHCP message it
     * may send back (RFC 2132 9.10): the largest datagram ipv4_receive accepts,
     * so no server should need to overload sname/file, which is refused. */
    p    = put_option(p, DHCP_OPT_PARAMETER_LIST, 2);
    *p++ = DHCP_OPT_SUBNET_MASK;
    *p++ = DHCP_OPT_ROUTER;

    p    = put_option(p, DHCP_OPT_MAX_MESSAGE_SIZE, 2);
    *p++ = (uint8_t)(IPV4_MAX_TOTAL_LENGTH >> 8);
    *p++ = (uint8_t)IPV4_MAX_TOTAL_LENGTH;

    *p = DHCP_OPT_END;

    /* Everything after End is already zero: padding to the BOOTP minimum. */
    static const uint8_t broadcast_mac[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    return udp_send(broadcast_mac, htonl(0u), htonl(NET_BROADCAST_IP), DHCP_CLIENT_PORT,
                    DHCP_SERVER_PORT, message, sizeof(message));
}

static void send_discover(void)
{
    if (send_message(DHCP_DISCOVER)) {
        log_sent("DISCOVER");
    } else {
        u_print("  [net] could not transmit the DHCP DISCOVER\n");
    }
}

static void send_request(void)
{
    if (send_message(DHCP_REQUEST)) {
        log_sent("REQUEST");
    } else {
        u_print("  [net] could not transmit the DHCP REQUEST\n");
    }
}

/* ---- the options ----------------------------------------------------------- */

/* One option's value, gathered across however many times it appears: RFC 2131
 * (4.1) and RFC 3396 say repeated instances of an option are concatenated, so
 * its length is checked on the total, after the walk. Only the first bytes are
 * kept -- enough for any option used here, and for the first address of a
 * router list. */
typedef struct {
    bool     present;
    uint32_t length;
    uint8_t  data[8];
} dhcp_option_t;

typedef struct {
    dhcp_option_t mask;
    dhcp_option_t router;
    dhcp_option_t lease;
    dhcp_option_t type;
    dhcp_option_t server;
    bool          overload;
} dhcp_options_t;

static void accumulate(dhcp_option_t *option, const uint8_t *data, uint32_t length)
{
    option->present = true;

    for (uint32_t i = 0; i < length && option->length + i < sizeof(option->data); i++) {
        option->data[option->length + i] = data[i];
    }

    option->length += length;
}

/* Walks the options between `p` and `end`, recording the ones this client uses.
 * Returns NULL if they are well-formed, or the reason they are not. `end` is
 * the end of the message, which udp_receive proved lies inside the datagram,
 * and every byte is proved to be before it before it is read. */
static const char *parse_options(const uint8_t *p, const uint8_t *const end,
                                 dhcp_options_t *options)
{
    bool saw_end = false;

    u_memset(options, 0, sizeof(*options));

    while (p < end) {
        const uint8_t code = *p++;

        /* The two options with no length byte. Pad is filler; End is the end,
         * and whatever follows it -- zero padding here, possibly junk from
         * another server -- is never read as options. */
        if (code == DHCP_OPT_PAD) {
            continue;
        }

        if (code == DHCP_OPT_END) {
            saw_end = true;
            break;
        }

        if (p == end) {
            return "an option code with no length byte";
        }

        const uint8_t length = *p++;

        /* Compared as "more than what is left", never as "p + length > end":
         * a pointer past the end of the message is undefined behaviour to
         * form, and end - p cannot be negative here. */
        if (length > (uint32_t)(end - p)) {
            return "an option runs past the end of the message";
        }

        switch (code) {
        case DHCP_OPT_SUBNET_MASK:
            accumulate(&options->mask, p, length);
            break;
        case DHCP_OPT_ROUTER:
            accumulate(&options->router, p, length);
            break;
        case DHCP_OPT_LEASE_TIME:
            accumulate(&options->lease, p, length);
            break;
        case DHCP_OPT_MESSAGE_TYPE:
            accumulate(&options->type, p, length);
            break;
        case DHCP_OPT_SERVER_ID:
            accumulate(&options->server, p, length);
            break;
        case DHCP_OPT_OVERLOAD:
            options->overload = true;
            break;
        default:
            break; /* stepped over by its length, never interpreted */
        }

        p += length;
    }

    /* RFC 2131 4.1: the options field ends with End. Without it there is no
     * telling where the options stop and padding begins. */
    if (!saw_end) {
        return "the options have no End";
    }

    /* The options that matter could be hiding in sname or file, and a message
     * half-parsed is worse than one refused. Option 57 asks servers not to. */
    if (options->overload) {
        return "option overload (52) is not supported";
    }

    if (options->type.present && options->type.length != 1u) {
        return "the message type option is not 1 byte";
    }

    if (options->mask.present && options->mask.length != 4u) {
        return "the subnet mask option is not 4 bytes";
    }

    if (options->router.present && (options->router.length == 0u || options->router.length % 4u != 0u)) {
        return "the router option is not a list of 4-byte addresses";
    }

    if (options->lease.present && options->lease.length != 4u) {
        return "the lease time option is not 4 bytes";
    }

    if (options->server.present && options->server.length != 4u) {
        return "the server identifier option is not 4 bytes";
    }

    return 0;
}

/* ---- what the addresses may be --------------------------------------------- */

/* An address a single host could have: not 0.0.0.0/8, loopback, multicast or
 * the reserved 240/4 (which takes in 255.255.255.255). */
static const char *not_a_host(uint32_t address)
{
    if ((address >> 24) == 0u) {
        return "is in 0.0.0.0/8";
    }

    if ((address >> 24) == 127u) {
        return "is a loopback address";
    }

    if ((address >> 28) == 0xEu) {
        return "is a multicast address";
    }

    if ((address >> 28) == 0xFu) {
        return "is in 240/4";
    }

    return 0;
}

/* Contiguous ones then zeroes -- ~mask + 1 is a power of two -- and from /8 to
 * /30: shorter would put a /8's worth of strangers on the link, longer leaves
 * no address that is neither the network nor the broadcast address. */
static bool mask_usable(uint32_t mask)
{
    const uint32_t hosts = ~mask;

    return (hosts & (hosts + 1u)) == 0u && mask >= 0xFF000000u && mask <= 0xFFFFFFFCu;
}

/* The mask a lease implies when it names none: the old class A, B or C. */
static uint32_t classful_mask(uint32_t address)
{
    if ((address >> 31) == 0u) {
        return 0xFF000000u;
    }

    if ((address >> 30) == 2u) {
        return 0xFFFF0000u;
    }

    return 0xFFFFFF00u;
}

/* Could `address` be this machine's, on the subnet `mask` makes it part of? */
static const char *address_problem(uint32_t address, uint32_t mask)
{
    if (not_a_host(address) != 0) {
        return "the offered address is not a host address";
    }

    if ((address & ~mask) == 0u) {
        return "the offered address is its subnet's network address";
    }

    if ((address & ~mask) == ~mask) {
        return "the offered address is its subnet's broadcast address";
    }

    return 0;
}

/* Could `router` be this machine's router? It must be a host on the same
 * subnet, and not this machine. The reasons are short enough that the line
 * they are logged in fits the console's 80 columns with any address. */
static const char *router_problem(uint32_t router, uint32_t address, uint32_t mask)
{
    if (not_a_host(router) != 0) {
        return "not a host address";
    }

    if ((router & mask) != (address & mask)) {
        return "not on this subnet";
    }

    if ((router & ~mask) == 0u || (router & ~mask) == ~mask) {
        return "a network or broadcast address";
    }

    if (router == address) {
        return "this machine's own address";
    }

    return 0;
}

/* ---- the transaction -------------------------------------------------------- */

void dhcp_start(void)
{
    transaction_id   = random32();
    transaction_open = true;
    state            = DHCP_STATE_SELECTING;
    interval         = FIRST_INTERVAL;

    send_discover();

    /* Re-armed whether or not the send got out: a lost DISCOVER is exactly
     * what retransmission is for. */
    arm_jittered(interval);
}

static void handle_offer(const dhcp_header_t *header, const dhcp_options_t *options)
{
    /* RFC 2131 (Table 3): an OFFER MUST carry the server identifier. It is how
     * the REQUEST names which server's offer is being taken. */
    if (!options->server.present) {
        drop("an OFFER without a server identifier (option 54)");
        return;
    }

    const uint32_t server  = net_load_be32(options->server.data);
    const uint32_t address = ntohl(header->yiaddr);

    if (not_a_host(server) != 0) {
        drop("the server identifier is not a host address");
        return;
    }

    /* Judged on the subnet the offer describes -- its own mask, or the
     * classful one if it names none. The ACK is judged again on its own. */
    uint32_t mask = classful_mask(address);

    if (options->mask.present) {
        mask = net_load_be32(options->mask.data);

        if (!mask_usable(mask)) {
            drop("an OFFER with an unusable subnet mask");
            return;
        }
    }

    const char *const problem = address_problem(address, mask);

    if (problem != 0) {
        drop(problem);
        return;
    }

    /* The first sane offer is taken; RFC 2131 lets a client choose however it
     * likes, and waiting for more would only delay. */
    offered_address = address;
    offered_server  = server;
    state           = DHCP_STATE_REQUESTING;
    requests_resent = 0;
    interval        = FIRST_INTERVAL;

    log_address("DHCP OFFER of ", address, "");

    request_sent = u_ticks();
    send_request();
    arm_jittered(interval);
}

/* Starts the lease clock: the whole lease if one alarm can span it, otherwise
 * the longest piece that can, remembering the rest. */
static void arm_lease(uint64_t ticks)
{
    const uint64_t piece = ticks > SYS_ALARM_MAX_TICKS ? SYS_ALARM_MAX_TICKS : ticks;

    lease_ticks_left = ticks - piece;
    arm(piece == 0u ? 1u : (uint32_t)piece);
}

/* Starts again, a second or two from now rather than at once, with no
 * transaction open meanwhile -- so a flood of NAKs finds nothing to match, and
 * no server, however it answers, can drive this client faster than its own
 * timer. After a NAK, a REQUEST nobody answered, or the end of a lease. */
static void start_again_later(void)
{
    transaction_open = false;
    state            = DHCP_STATE_INIT;
    arm(SYS_ALARM_HZ + random32() % (SYS_ALARM_HZ + 1u));
}

static void handle_ack(const dhcp_header_t *header, const dhcp_options_t *options)
{
    /* From the server this client chose, if it says. QEMU's ACK always does. */
    if (options->server.present && net_load_be32(options->server.data) != offered_server) {
        drop("an ACK from a server this client did not choose");
        return;
    }

    const uint32_t address = ntohl(header->yiaddr);

    if (address != offered_address) {
        drop("an ACK for an address this client did not request");
        return;
    }

    /* The ACK's parameters are the lease, so they are judged again, on the
     * mask that will actually be bound: an OFFER's /24 and an ACK's /28 can
     * make the same address a broadcast address. */
    uint32_t mask = classful_mask(address);

    if (options->mask.present) {
        mask = net_load_be32(options->mask.data);

        if (!mask_usable(mask)) {
            drop("an ACK with an unusable subnet mask");
            return;
        }
    } else {
        log_address("DHCP: no subnet mask given; using the classful one for ", address, "");
    }

    const char *const problem = address_problem(address, mask);

    if (problem != 0) {
        drop(problem);
        return;
    }

    /* RFC 2131 requires a lease time in an ACK. One without is taken as
     * infinite rather than refused -- refusing would leave a machine that
     * never gets an address from such a server -- and said so. */
    uint32_t lease = DHCP_LEASE_INFINITE;

    if (options->lease.present) {
        lease = net_load_be32(options->lease.data);
    }

    /* The lease began when the REQUEST went out (RFC 2131 4.4.1), so what is
     * left of it is the lease less the time since. One already over -- a
     * lease of 0, or one shorter than the wait for its ACK -- grants nothing,
     * and taking it would only start the next exchange at once. */
    const uint32_t elapsed     = u_ticks() - request_sent;
    const uint64_t lease_ticks = (uint64_t)lease * SYS_ALARM_HZ;

    if (lease != DHCP_LEASE_INFINITE && lease_ticks <= elapsed) {
        drop("an ACK whose lease has already run out");
        return;
    }

    if (!options->lease.present) {
        u_print("  [net] DHCP: the ACK gives no lease time; taking it as infinite\n");
    }

    uint32_t router = 0;

    if (options->router.present) {
        router = net_load_be32(options->router.data);

        const char *const why = router_problem(router, address, mask);

        if (why != 0) {
            static char line[112];

            char *const limit = U_LIMIT(line);
            char       *out   = line;

            out = u_append(out, limit, "  [net] DHCP router ");
            out = net_append_ipv4(out, limit, router);
            out = u_append(out, limit, " ignored: ");
            out = u_append(out, limit, why);
            out = u_append(out, limit, "\n");
            *out = '\0';

            u_print(line);
            router = 0;
        }
    }

    net_config_t granted;

    granted.bound         = true;
    granted.address       = address;
    granted.netmask       = mask;
    granted.router        = router;
    granted.server        = offered_server;
    granted.lease_seconds = lease;

    net_config_bind(&granted);

    transaction_open = false;
    state            = DHCP_STATE_BOUND;
    disarm();

    log_address("DHCP Lease Acquired: ", address, "");

    static char line[112];

    char *const limit = U_LIMIT(line);
    char       *out   = line;

    out = u_append(out, limit, "  [net] mask ");
    out = net_append_ipv4(out, limit, mask);
    out = u_append(out, limit, ", router ");
    out = router != 0u ? net_append_ipv4(out, limit, router) : u_append(out, limit, "none");
    out = u_append(out, limit, ", lease ");

    if (lease == DHCP_LEASE_INFINITE) {
        out = u_append(out, limit, "infinite");
    } else {
        out = u_append_dec(out, limit, lease);
        out = u_append(out, limit, " s");
    }

    out = u_append(out, limit, "\n");
    *out = '\0';

    u_print(line);

    /* The first thing said from the new address: a question to the router,
     * whose answer proves the address works on the segment. No router, no
     * question. */
    if (router != 0u) {
        arp_request(router);
    }

    if (lease != DHCP_LEASE_INFINITE) {
        arm_lease(lease_ticks - elapsed);
    }
}

static void handle_nak(const dhcp_options_t *options)
{
    /* QEMU's NAK carries no server identifier; one that does must be from the
     * server this client chose, or it is some other server's opinion of a
     * request that was not made to it. */
    if (options->server.present && net_load_be32(options->server.data) != offered_server) {
        drop("a NAK from a server this client did not choose");
        return;
    }

    log_address("DHCP NAK for ", offered_address, ": starting again");
    start_again_later();
}

void dhcp_receive(uint32_t src_port, const uint8_t *msg, uint32_t length)
{
    /* 1. From a server. Other clients' broadcasts come from port 68. */
    if (src_port != DHCP_SERVER_PORT) {
        drop("not from the DHCP server port, 67");
        return;
    }

    /* 2. Long enough for the fixed header and the cookie. Until this passes,
     *    not one field may be read. */
    if (length < DHCP_OPTIONS_OFFSET) {
        drop("too short to be a DHCP message");
        return;
    }

    const dhcp_header_t *const header = (const dhcp_header_t *)msg;

    /* 3-5. A reply, for an Ethernet client. */
    if (header->op != DHCP_OP_REPLY) {
        drop("not a BOOTREPLY");
        return;
    }

    if (header->htype != DHCP_HTYPE_ETHERNET) {
        drop("the hardware type is not Ethernet");
        return;
    }

    if (header->hlen != ETH_ALEN) {
        drop("the hardware address length is not 6");
        return;
    }

    /* 6. For the transaction in progress. With none open -- bound, or waiting
     *    to start again -- nothing is. */
    if (!transaction_open || ntohl(header->xid) != transaction_id) {
        drop("not for this client's transaction (xid)");
        return;
    }

    /* 7. For this card. The xid is guessable from the broadcast it came in, so
     *    the hardware address is checked too. */
    for (uint32_t i = 0; i < ETH_ALEN; i++) {
        if (header->chaddr[i] != rtl8139_mac()[i]) {
            drop("for another client's hardware address");
            return;
        }
    }

    /* 8. DHCP, not plain BOOTP. */
    uint32_t cookie;

    u_memcpy(&cookie, msg + DHCP_HEADER_SIZE, DHCP_COOKIE_SIZE);

    if (ntohl(cookie) != DHCP_MAGIC_COOKIE) {
        drop("no DHCP magic cookie");
        return;
    }

    /* 9. The options, walked whole before any of them is believed. */
    dhcp_options_t    options;
    const char *const malformed = parse_options(msg + DHCP_OPTIONS_OFFSET, msg + length, &options);

    if (malformed != 0) {
        drop(malformed);
        return;
    }

    /* 10. A message this state is waiting for. A missing type reads as 0,
     *     which no state waits for. */
    const uint8_t type = options.type.present ? options.type.data[0] : 0u;

    if (state == DHCP_STATE_SELECTING && type == DHCP_OFFER) {
        handle_offer(header, &options);
        return;
    }

    if (state == DHCP_STATE_REQUESTING && type == DHCP_ACK) {
        handle_ack(header, &options);
        return;
    }

    if (state == DHCP_STATE_REQUESTING && type == DHCP_NAK) {
        handle_nak(&options);
        return;
    }

    drop("not a message this client is waiting for");
}

void dhcp_timer(void)
{
    alarm_armed = false; /* it has gone off; every path below arms it again */

    switch (state) {
    case DHCP_STATE_INIT:
        dhcp_start();
        return;

    case DHCP_STATE_SELECTING:
        send_discover();
        interval = interval * 2u > DISCOVER_MAX_INTERVAL ? DISCOVER_MAX_INTERVAL : interval * 2u;
        arm_jittered(interval);
        return;

    case DHCP_STATE_REQUESTING:
        /* Four retransmissions, at 4, 8, 16 and 32 seconds; then a last short
         * wait, and if nothing has come, the server is gone: start again. RFC
         * 2131 3.1's example gives up after about a minute. */
        if (requests_resent == REQUEST_RETRANSMISSIONS) {
            u_print("  [net] DHCP: no answer to the REQUEST; starting again\n");
            start_again_later();
            return;
        }

        requests_resent++;
        send_request();
        interval = requests_resent == REQUEST_RETRANSMISSIONS ? FIRST_INTERVAL : interval * 2u;
        arm_jittered(interval);
        return;

    case DHCP_STATE_BOUND:
        if (lease_ticks_left != 0u) {
            arm_lease(lease_ticks_left);
            return;
        }

        /* The lease has run out. RFC 2131 4.4.5: stop using the address at
         * once. Without renewal the only way to keep one is to ask again. */
        log_address("DHCP lease on ", net_config()->address, " expired: address dropped");
        net_config_clear();
        start_again_later();
        return;
    }
}

void dhcp_poll(void)
{
    if (alarm_armed && (int32_t)(u_ticks() - alarm_due) >= 0) {
        /* Due: take it here, and cancel the kernel's copy -- which has
         * expired, or is about to -- so recv does not deliver it a second
         * time, as a timer for whatever the client arms next. */
        disarm();
        dhcp_timer();
    }
}
