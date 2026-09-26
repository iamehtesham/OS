/* ICMP echo -- ping -- in ring 3.
 *
 * An echo request carries an identifier, a sequence number and some data, all
 * chosen by the sender, and the reply carries all three back unchanged. That
 * is the whole protocol, and it is the one thing on the internet every host is
 * required to answer (RFC 1122). It is also the first thing this machine does
 * that involves a checksum it must get right: ARP has none, and a reply with a
 * wrong one is discarded by the far end without a word.
 *
 * The reply is built fresh in its own buffer, never edited in place in the
 * receive ring, and it is addressed from this machine's own identity rather
 * than by swapping the request's addresses around. For a request unicast to
 * 10.0.2.15 those come to the same thing; the difference is in what the swap
 * would copy blindly. Only echo requests are answered: an inbound echo REPLY
 * is not, so two machines cannot bounce replies off each other forever. */

#include <stdbool.h>
#include <stdint.h>

#include "net/byteorder.h"
#include "net/checksum.h"
#include "net/ethernet.h"
#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/rtl8139.h"
#include "user/ulib.h"

/* Static, like every buffer in userland: it lands in .bss, which the loader
 * zeroes, so no initialiser becomes a memset the freestanding link cannot
 * resolve. The largest request IPv4 lets through is 1500 bytes of datagram, so
 * the largest reply is exactly one full frame. */
static uint8_t reply[ETH_MAX_FRAME];

static void log_line(const char *what, uint32_t source, uint32_t detail_a, uint32_t detail_b,
                     uint32_t bytes)
{
    static char line[112];

    char *const limit = U_LIMIT(line);
    char       *out   = line;

    out = u_append(out, limit, "  [net] ");
    out = u_append(out, limit, what);
    out = net_append_ipv4(out, limit, source);
    out = u_append(out, limit, ": id ");
    out = u_append_dec(out, limit, detail_a);
    out = u_append(out, limit, " seq ");
    out = u_append_dec(out, limit, detail_b);
    out = u_append(out, limit, ", ");
    out = u_append_dec(out, limit, bytes);
    out = u_append(out, limit, " bytes\n");
    *out = '\0';

    u_print(line);
}

/* Builds and sends the reply to one echo request. `message` is the request's
 * ICMP message -- header and data -- and `length` is its size by the IPv4
 * header's account, which ipv4_receive has already bounded. */
static void send_echo_reply(const ethernet_header_t *eth, const ipv4_header_t *ip,
                            const uint8_t *message, uint32_t length)
{
    const uint32_t reply_length = ETH_HEADER_SIZE + IPV4_HEADER_SIZE + length;

    /* Cannot happen -- a datagram of at most 1500 bytes with a header of at
     * least 20 carries at most 1480 bytes of ICMP, and 14 + 20 + 1480 is one
     * full frame -- but the copy below is into a fixed buffer, and "cannot
     * happen" is a claim about code elsewhere. The check costs nothing. */
    if (reply_length > sizeof(reply)) {
        u_print("  [net] an echo request too large to answer; dropped\n");
        return;
    }

    /* Ethernet: back to the MAC the request came from, from this card's own.
     * NOT the request's destination MAC -- for a frame that was broadcast that
     * would be ff:ff:ff:ff:ff:ff, and a reply "from" the broadcast address is a
     * forged frame. */
    uint8_t *const ip_start =
        net_write_ethernet_header(reply, eth->src_mac, rtl8139_mac(), ETHERTYPE_IPV4);

    /* IPv4: a fresh header rather than the request's copied back, built by
     * the one function every reply uses. A copy would carry the request's
     * options -- a source route among them -- and its decremented TTL and its
     * identification, all of which belong to the request's journey, not the
     * reply's. */
    uint8_t *const icmp_start =
        ipv4_write_reply_header(ip_start, ip, IPV4_PROTOCOL_ICMP, length);

    /* ICMP: the request's message copied whole -- identifier, sequence and
     * data are the sender's and go back untouched -- then turned into a reply.
     * The type changes, so the checksum must be recomputed, and the field is
     * zeroed before the sum: the checksum is defined over the message with
     * that field zero, and the copy just put the request's checksum there. */
    icmp_header_t *const out_icmp = (icmp_header_t *)icmp_start;

    u_memcpy(icmp_start, message, length);

    out_icmp->type     = ICMP_TYPE_ECHO_REPLY;
    out_icmp->code     = 0;
    out_icmp->checksum = 0;
    out_icmp->checksum = htons(net_checksum(icmp_start, length));

    /* Send first, log after: the log line is an IPC round trip to the console,
     * and the sender is timing the reply. */
    const bool sent = rtl8139_send_packet(reply, reply_length);

    log_line(sent ? "ping reply sent to " : "could not transmit a ping reply to ",
             ntohl(ip->src_ip), ntohs(out_icmp->identifier), ntohs(out_icmp->sequence), length);
}

void icmp_receive(const ethernet_header_t *eth, const ipv4_header_t *ip, const uint8_t *message,
                  uint32_t length)
{
    const uint32_t source = ntohl(ip->src_ip);

    if (length < ICMP_HEADER_SIZE) {
        u_printf("  [net] an ICMP message of %u bytes is too short to hold a header\n", length);
        return;
    }

    /* Over the whole message, checksum field included: a correct message sums
     * to 0. ipv4_receive bounded `length` by the datagram, so every byte summed
     * is inside the frame. */
    if (net_checksum(message, length) != 0u) {
        u_print("  [net] ICMP dropped: bad checksum\n");
        return;
    }

    const icmp_header_t *const icmp = (const icmp_header_t *)message;

    if (icmp->type != ICMP_TYPE_ECHO_REQUEST || icmp->code != 0u) {
        static char line[96];

        char *const limit = U_LIMIT(line);
        char       *out   = line;

        out = u_append(out, limit, "  [net] ICMP type ");
        out = u_append_dec(out, limit, icmp->type);
        out = u_append(out, limit, " code ");
        out = u_append_dec(out, limit, icmp->code);
        out = u_append(out, limit, " from ");
        out = net_append_ipv4(out, limit, source);
        out = u_append(out, limit, ": not answered\n");
        *out = '\0';

        u_print(line);
        return;
    }

    send_echo_reply(eth, ip, message, length);
}
