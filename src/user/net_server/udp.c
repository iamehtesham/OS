/* UDP (RFC 768), and the echo service (RFC 862) on port 7, in ring 3.
 *
 * UDP is IP plus two port numbers, a length and a checksum. The checksum is the
 * interesting part: it covers not only the segment but a "pseudo-header" of
 * fields borrowed from the IP header -- both addresses, the protocol number and
 * the UDP length -- so a datagram that arrives intact but at the wrong machine,
 * or under the wrong protocol, still fails it. The pseudo-header is never sent;
 * it is built here, in a scratch buffer, only to be summed.
 *
 * Like ipv4.c, most of this file is refusal, and the checks run in an order
 * that lets each read only bytes an earlier one proved are there. What survives
 * and is addressed to port 7 is echoed: the same data, back to the port and
 * address it came from, from this machine's own. The echo lives in the network
 * server because there is no socket interface yet for a separate program to
 * receive on; when there is, this is the service that moves out first. */

#include <stdbool.h>
#include <stdint.h>

#include "net/byteorder.h"
#include "net/checksum.h"
#include "net/dhcp.h"
#include "net/ethernet.h"
#include "net/ipv4.h"
#include "net/rtl8139.h"
#include "net/udp.h"
#include "user/ulib.h"

/* Where the pseudo-header, the UDP header and the data are laid end to end for
 * the checksum. The payload's length is only known when a datagram arrives, but
 * its maximum is fixed at compile time: ipv4_receive refuses a datagram over
 * 1500 bytes, its header is at least 20, and udp_receive refuses a segment
 * longer than the datagram around it -- so no segment is longer than
 * UDP_MAX_LENGTH, and this buffer, sized for the longest, fits every one. The
 * runtime length only decides how much of it is filled and summed.
 *
 * Static rather than on the stack or the heap: there is no heap in ring 3, and
 * a fixed buffer is one the compiler sizes and nothing can fail to allocate.
 * One buffer is enough because the network server is single-threaded and
 * handles each frame to completion before the next -- nothing re-enters it. */
static uint8_t checksum_scratch[UDP_PSEUDO_HEADER_SIZE + UDP_MAX_LENGTH];

_Static_assert(sizeof(checksum_scratch) == 1492u,
               "12 bytes of pseudo-header plus the longest segment IPv4 lets through");

/* The echo reply, built fresh, never edited in place in the receive ring. The
 * longest segment makes a reply of exactly one full frame. */
static uint8_t reply[ETH_MAX_FRAME];

/* Datagrams this machine originates (udp_send). Separate from `reply`, though
 * nothing here runs two at once: the server handles one frame, or one timer,
 * to completion before the next. */
static uint8_t outgoing[ETH_MAX_FRAME];

/* The most of a datagram's data the log shows, so that a line stays inside the
 * console's 80 columns. */
#define PREVIEW_BYTES 40u

/* Sums the pseudo-header and the segment at `segment`, whose own length field
 * says how long it is. `src_ip` and `dest_ip` are in network order, exactly as
 * they sit in an IPv4 header.
 *
 * One contract, two uses, like net_checksum's. Over a received segment with its
 * checksum field as it arrived, a correct segment gives 0. Over a segment whose
 * checksum field has been zeroed, the result is the checksum to store.
 *
 * Returns false, and leaves *sum alone, if the length field is longer than the
 * scratch buffer holds. That bound is this function's own: it protects the
 * write into checksum_scratch. The read from `segment` is the caller's to
 * bound -- udp_receive has proved that many bytes are inside the datagram
 * before it calls this. False fails closed: a caller checking a segment treats
 * it as a bad checksum, and a caller building one sends nothing. */
static bool udp_checksum(uint32_t src_ip, uint32_t dest_ip, const uint8_t *segment,
                         uint16_t *sum)
{
    const udp_header_t *const udp    = (const udp_header_t *)segment;
    const uint32_t            length = ntohs(udp->length);

    if (length > UDP_MAX_LENGTH) {
        return false;
    }

    /* Field by field, laid directly over the buffer: the pseudo-header is
     * built where it is summed, so there is no second copy of it to keep in
     * step with the first. */
    udp_pseudo_header_t *const pseudo = (udp_pseudo_header_t *)checksum_scratch;

    pseudo->src_ip   = src_ip;
    pseudo->dest_ip  = dest_ip;
    pseudo->reserved = 0;
    pseudo->protocol = IPV4_PROTOCOL_UDP;

    /* Copied, not converted: the pseudo-header's length IS the header's length
     * field, the same two bytes in the same network order. Taking it from
     * anywhere else -- the IPv4 payload length, or a host-order number -- is
     * how the two come to disagree. */
    pseudo->udp_length = udp->length;

    u_memcpy(checksum_scratch + UDP_PSEUDO_HEADER_SIZE, segment, length);

    /* Twelve and eight are both even, so the data starts on a 16-bit boundary
     * and an odd final byte is padded on the right by net_checksum, as RFC 768
     * asks -- no special case here. */
    *sum = net_checksum(checksum_scratch, UDP_PSEUDO_HEADER_SIZE + length);
    return true;
}

/* Finishes a segment built at `udp` behind the IPv4 header `ip`: computes the
 * checksum over the pseudo-header, the header and the data, and stores it. The
 * caller has zeroed the checksum field -- the checksum is defined over the
 * segment with that field zero -- and the length field is final. One place for
 * the rule every sender must follow: a transmitted 0 means "no checksum" (RFC
 * 768), so a checksum that comes out as 0 is sent as 0xFFFF instead. In one's
 * complement both are zero -- +0 and -0 -- so the receiver's sum still
 * verifies. The reverse cannot arise: net_checksum returns 0xFFFF only for
 * all-zero input, and the pseudo-header always carries protocol 17. */
static bool udp_seal(const ipv4_header_t *ip, udp_header_t *udp)
{
    uint16_t sum;

    if (!udp_checksum(ip->src_ip, ip->dest_ip, (const uint8_t *)udp, &sum)) {
        return false;
    }

    if (sum == 0u) {
        sum = 0xFFFFu;
    }

    udp->checksum = htons(sum);
    return true;
}

/* One line per dropped segment, with the reason. */
static void drop(const char *reason)
{
    static char line[96];

    char *const limit = U_LIMIT(line);
    char       *out   = line;

    out = u_append(out, limit, "  [net] UDP dropped: ");
    out = u_append(out, limit, reason);
    out = u_append(out, limit, "\n");
    *out = '\0';

    u_print(line);
}

/* The line every UDP segment gets once its header is known to be there. */
static void log_packet(uint32_t src_port, uint32_t dest_port, uint32_t length)
{
    static char line[96];

    char *const limit = U_LIMIT(line);
    char       *out   = line;

    out = u_append(out, limit, "  [net] UDP Packet | Port ");
    out = u_append_dec(out, limit, src_port);
    out = u_append(out, limit, " -> ");
    out = u_append_dec(out, limit, dest_port);
    out = u_append(out, limit, " | Length: ");
    out = u_append_dec(out, limit, length);
    out = u_append(out, limit, "\n");
    *out = '\0';

    u_print(line);
}

/* The start of the data, as text, so a message typed into netcat can be seen
 * arriving. Only printable ASCII goes through; every other byte becomes '.'.
 * The data is a stranger's, and the console acts on some bytes rather than
 * showing them -- '\r' returns to the start of the line, '\b' steps back and
 * blanks, '\n' starts a new one, and a NUL would end the string early -- so
 * printed raw, a datagram could overwrite this log or forge lines in it. */
static void log_data(const uint8_t *data, uint32_t bytes, bool checksummed)
{
    static char line[96];

    char *const    limit = U_LIMIT(line);
    char          *out   = line;
    const uint32_t shown = bytes < PREVIEW_BYTES ? bytes : PREVIEW_BYTES;

    out = u_append(out, limit, checksummed ? "  [net] data: \"" : "  [net] data (no checksum): \"");

    for (uint32_t i = 0; i < shown && out < limit; i++) {
        *out++ = (data[i] >= 0x20u && data[i] <= 0x7Eu) ? (char)data[i] : '.';
    }

    out = u_append(out, limit, shown < bytes ? "\"...\n" : "\"\n");
    *out = '\0';

    u_print(line);
}

static void log_peer(const char *what, uint32_t address, uint32_t port, const char *tail)
{
    static char line[96];

    char *const limit = U_LIMIT(line);
    char       *out   = line;

    out = u_append(out, limit, "  [net] ");
    out = u_append(out, limit, what);
    out = net_append_ipv4(out, limit, address);
    out = u_append(out, limit, ":");
    out = u_append_dec(out, limit, port);
    out = u_append(out, limit, tail);
    out = u_append(out, limit, "\n");
    *out = '\0';

    u_print(line);
}

/* Sends `segment` -- `udp_length` bytes, already validated -- back where it came
 * from. */
static void echo_reply(const ethernet_header_t *eth, const ipv4_header_t *ip,
                       const uint8_t *segment, uint32_t udp_length, uint32_t src_port)
{
    const uint32_t source = ntohl(ip->src_ip);

    /* Not to a service port. Two echo servers, or an echo and a chargen, that
     * each answer the other never stop (CVE-1999-0103, the "UDP packet
     * storm"), and a request forged to come from one starts them. Below 1024 is
     * where such services live. NFS (2049) is refused for the second reason
     * echo is dangerous: it would deliver bytes an attacker chose, from this
     * machine's address and a privileged source port, to a service that
     * trusts a client by its port. Port 0 means "no reply wanted" (RFC 768).
     * This is OpenBSD inetd's rule (dg_badinput). It governs only where a reply
     * goes: every echo still leaves FROM port 7, so a service on some other
     * unprivileged port that trusts a privileged source port can still be sent
     * chosen bytes. Nor can it stop a loop with a reflector on an ordinary
     * high port -- nothing that looks only at port numbers can -- and no rate
     * limit stands behind it (the kernel alarm could pace one; none is built). */
    if (src_port < UDP_FIRST_UNPRIVILEGED_PORT || src_port == UDP_PORT_NFS) {
        log_peer("UDP echo refused: ", source, src_port, " is a service port");
        return;
    }

    /* Cannot happen -- udp_length is at most 1480, and 14 + 20 + 1480 is one
     * full frame -- but the copy below is into a fixed buffer, and "cannot
     * happen" is a claim about code in another file. This is the only length
     * test here: the data length is derived from udp_length, never tested on
     * its own. */
    const uint32_t reply_length = ETH_HEADER_SIZE + IPV4_HEADER_SIZE + udp_length;

    if (reply_length > sizeof(reply)) {
        u_print("  [net] a UDP echo too large to answer; dropped\n");
        return;
    }

    /* Ethernet: back to the MAC the request came from, from this card's own.
     * IPv4: a fresh header from this machine's leased address to the
     * request's source. For a request unicast to that address -- the only kind
     * that reaches echo -- that is the same as swapping the request's
     * addresses, but it never copies the request's destination, which for a
     * broadcast would forge a reply "from" the broadcast address. */
    uint8_t *const ip_start =
        net_write_ethernet_header(reply, eth->src_mac, rtl8139_mac(), ETHERTYPE_IPV4);
    uint8_t *const udp_start =
        ipv4_write_reply_header(ip_start, ip, IPV4_PROTOCOL_UDP, udp_length);

    /* UDP: the ports swapped, copied as they are -- network order in, network
     * order out, so neither side is converted. The length is the request's,
     * since the data is the request's. The checksum field is zeroed before the
     * sum: the checksum is defined over the segment with that field zero, and
     * this buffer still holds the previous reply's. */
    const udp_header_t *const in  = (const udp_header_t *)segment;
    udp_header_t *const       out = (udp_header_t *)udp_start;

    out->src_port  = in->dest_port;
    out->dest_port = in->src_port;
    out->length    = in->length;
    out->checksum  = 0;

    u_memcpy(udp_start + UDP_HEADER_SIZE, segment + UDP_HEADER_SIZE,
             udp_length - UDP_HEADER_SIZE);

    /* Over the reply's own addresses, which the pseudo-header must match: the
     * ones the receiver will see in the IPv4 header in front of this. */
    if (!udp_seal((const ipv4_header_t *)ip_start, out)) {
        u_print("  [net] a UDP echo whose checksum could not be computed; dropped\n");
        return;
    }

    /* Send first, log after: the log line is an IPC round trip to the console,
     * and the sender is timing the reply. */
    const bool sent = rtl8139_send_packet(reply, reply_length);

    log_peer(sent ? "UDP echo sent to " : "could not transmit a UDP echo to ", source, src_port,
             "");
}

void udp_receive(const ethernet_header_t *eth, const ipv4_header_t *ip, const uint8_t *segment,
                 uint32_t length, bool broadcast)
{
    /* 1. Room for a UDP header. Until this passes, not one byte of it may be
     *    read. On the wire nothing depends on it -- checks 3 and 4 together
     *    already refuse a short datagram, so no reply could follow -- but they
     *    read the length field to say so, and the log line below prints the
     *    ports. Without this, a datagram of 0 to 7 bytes would be logged with
     *    "ports" and a "length" taken from the card's CRC, the Ethernet padding,
     *    or an older frame in the ring. */
    if (length < UDP_HEADER_SIZE) {
        drop("the datagram is too short to hold a UDP header");
        return;
    }

    /* 2. The header, in host order. ntohs() on the way in: read raw, port 7
     *    arrives as 1792. */
    const udp_header_t *const udp        = (const udp_header_t *)segment;
    const uint32_t            src_port   = ntohs(udp->src_port);
    const uint32_t            dest_port  = ntohs(udp->dest_port);
    const uint32_t            udp_length = ntohs(udp->length);

    log_packet(src_port, dest_port, udp_length);

    /* 3. The length counts the header, so it is at least 8. */
    if (udp_length < UDP_HEADER_SIZE) {
        drop("the length field is shorter than the header");
        return;
    }

    /* 4. And the segment fits inside the datagram that carries it. It may be
     *    shorter -- whatever lies between the UDP length and the end of the
     *    IPv4 payload is ignored, as Linux, FreeBSD and QEMU's own stack do --
     *    but never longer: past the datagram is padding, or the card's CRC, or
     *    an earlier frame in the ring. */
    if (udp_length > length) {
        drop("the length field runs past the end of the datagram");
        return;
    }

    /* 5. The checksum, if the sender computed one. A zero field means it did
     *    not, which RFC 768 allows and RFC 1122 (4.1.3.4) lets a receiver
     *    accept. Anything else must verify: summed as it arrived, with the
     *    pseudo-header in front, a correct segment gives 0. Check 4 proved all
     *    udp_length bytes are inside the datagram. */
    const bool checksummed = udp->checksum != 0u;

    if (checksummed) {
        uint16_t sum;

        if (!udp_checksum(ip->src_ip, ip->dest_ip, segment, &sum) || sum != 0u) {
            drop("bad checksum");
            return;
        }
    }

    /* 6. The DHCP client's port. Its messages are binary and the client logs
     *    what they mean, so no text preview; and this is the one port a
     *    broadcast may reach -- a server has no address to send an OFFER to
     *    until the OFFER has been accepted. */
    if (dest_port == DHCP_CLIENT_PORT) {
        dhcp_receive(src_port, segment + UDP_HEADER_SIZE, udp_length - UDP_HEADER_SIZE);
        return;
    }

    /* Nothing else answers a broadcast. Echo in particular: an echo server
     * that answered one would be the reflector half of a "fraggle" flood. */
    if (broadcast) {
        drop("a broadcast, and only DHCP listens for those");
        return;
    }

    /* 7. What it says, now that it is known to be what was sent. */
    log_data(segment + UDP_HEADER_SIZE, udp_length - UDP_HEADER_SIZE, checksummed);

    /* 8. Whoever is listening on the destination port. Only echo is. Nothing
     *    is sent back for the rest: RFC 1122 says a host SHOULD answer with an
     *    ICMP port-unreachable, and there is no ICMP error generation here. */
    if (dest_port != UDP_PORT_ECHO) {
        static char line[96];

        char *const limit = U_LIMIT(line);
        char       *out   = line;

        out = u_append(out, limit, "  [net] UDP to port ");
        out = u_append_dec(out, limit, dest_port);
        out = u_append(out, limit, ": no service listens there\n");
        *out = '\0';

        u_print(line);
        return;
    }

    echo_reply(eth, ip, segment, udp_length, src_port);
}

bool udp_send(const uint8_t *dst_mac, uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
              uint16_t dst_port, const uint8_t *data, uint32_t length)
{
    /* The longest datagram this machine accepts is the longest it sends. */
    if (length > UDP_MAX_LENGTH - UDP_HEADER_SIZE) {
        return false;
    }

    const uint32_t udp_length   = UDP_HEADER_SIZE + length;
    const uint32_t frame_length = ETH_HEADER_SIZE + IPV4_HEADER_SIZE + udp_length;

    uint8_t *const ip_start =
        net_write_ethernet_header(outgoing, dst_mac, rtl8139_mac(), ETHERTYPE_IPV4);
    uint8_t *const udp_start =
        ipv4_write_header(ip_start, src_ip, dst_ip, IPV4_PROTOCOL_UDP, 0, udp_length);

    udp_header_t *const udp = (udp_header_t *)udp_start;

    udp->src_port  = htons(src_port);
    udp->dest_port = htons(dst_port);
    udp->length    = htons((uint16_t)udp_length);
    udp->checksum  = 0;

    u_memcpy(udp_start + UDP_HEADER_SIZE, data, length);

    if (!udp_seal((const ipv4_header_t *)ip_start, udp)) {
        return false;
    }

    return rtl8139_send_packet(outgoing, frame_length);
}
