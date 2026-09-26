/* IPv4, in ring 3 -- enough of it to know that a datagram is really for this
 * machine, and to hand what is inside it to the protocol that speaks it.
 *
 * Almost all of this file is refusal. A datagram arrives from a stranger, and
 * every field in its header is that stranger's claim: how long the header is,
 * how long the datagram is, where it came from. Each check below reads only
 * bytes an earlier check has already proved are there, which is why their
 * order matters and is written down: a length is checked against the frame
 * before anything is read through it, and nothing past the header is looked at
 * until the header's own length has been checked against the datagram's.
 *
 * Nothing is reassembled and nothing is routed. A fragment is dropped -- there
 * is no reassembly buffer -- and so is any datagram not addressed to 10.0.2.15.
 * The card is promiscuous, but by the time a frame reaches this file
 * handle_ethernet has already kept only those sent to this card's MAC or to the
 * broadcast MAC, so what arrives here was at least addressed to this machine at
 * layer 2. A host that answered datagrams meant for other machines would be
 * impersonating them. */

#include <stdbool.h>
#include <stdint.h>

#include "net/byteorder.h"
#include "net/checksum.h"
#include "net/ethernet.h"
#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/rtl8139.h"
#include "net/udp.h"
#include "user/ulib.h"

/* The IPv4 identification field, for datagrams this machine originates. It
 * only matters to a receiver reassembling fragments, and nothing this machine
 * sends is ever fragmented; it counts up so that consecutive replies never
 * share one -- across protocols too, which is why it lives here and not in each
 * of them -- until it wraps at 65,536, which RFC 6864 permits for datagrams
 * that are never fragmented. */
static uint16_t next_datagram_id;

/* One line per dropped datagram, with the reason. Silent to the sender -- a
 * host does not answer a malformed or misdirected datagram -- but not silent
 * here, because a drop nobody can see is indistinguishable from a network that
 * simply went quiet. */
static void drop(const char *reason)
{
    static char line[96];

    char *const limit = U_LIMIT(line);
    char       *out   = line;

    out = u_append(out, limit, "  [net] IPv4 dropped: ");
    out = u_append(out, limit, reason);
    out = u_append(out, limit, "\n");
    *out = '\0';

    u_print(line);
}

static bool mac_equal(const uint8_t *a, const uint8_t *b)
{
    for (uint32_t i = 0; i < ETH_ALEN; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }

    return true;
}

/* A reason to refuse a source address, or NULL if it is an ordinary unicast
 * host. RFC 1122 (3.2.1.3) requires a host to silently discard a datagram whose
 * source is not a single host, and the echo reply is why it matters here: the
 * reply goes back to the source, and replying to a broadcast, multicast, zero
 * or loopback IP address is a datagram no host may send.
 *
 * What these IP checks do NOT do is stop a reply fanning out to every station
 * -- the first version of this comment claimed they did. The reply's Ethernet
 * destination is copied from the request's source MAC, never derived from the
 * IP address, so a request "from" 255.255.255.255 sent by one station would be
 * answered unicast to that one station. Layer-2 fan-out is prevented by the
 * group-MAC check below, which is the one that refuses a broadcast or multicast
 * source MAC. Nor can any check here stop a forged ordinary source: a request
 * claiming to come from another host's unicast address draws a reply to that
 * host, as it would from any machine that answers ping. `source` is in host
 * order. */
static const char *bad_source(uint32_t source, const uint8_t *source_mac)
{
    /* The low bit of the first octet marks a group address -- broadcast or
     * multicast. No single host sends from one. */
    if ((source_mac[0] & 0x01u) != 0u) {
        return "the source MAC is a group address";
    }

    /* All of 0.0.0.0/8, not just 0.0.0.0: "this host on this network" is only
     * a valid source while a host is learning its own address, and a reply to
     * any of it is a datagram RFC 1122 says must not be sent. */
    if ((source >> 24) == 0u) {
        return "the source is in 0.0.0.0/8";
    }

    if (source == 0xFFFFFFFFu || source == NET_SUBNET_BROADCAST) {
        return "the source is a broadcast address";
    }

    /* The subnet's own address -- host part all zeroes. Not a host; older
     * stacks treat it as a broadcast. */
    if (source == (NET_LOCAL_IP & NET_NETMASK)) {
        return "the source is the subnet's network address";
    }

    if ((source >> 24) == 127u) {
        return "the source is a loopback address";
    }

    if ((source >> 28) == 0xEu) {
        return "the source is a multicast address";
    }

    if ((source >> 28) == 0xFu) {
        return "the source is in the reserved 240/4 range";
    }

    if (source == NET_LOCAL_IP) {
        return "the source claims to be this machine";
    }

    return 0;
}

void ipv4_receive(const uint8_t *frame, uint32_t frame_bytes)
{
    /* 1. Room for the fixed part of a header at all. Until this passes, not
     *    one byte of the IPv4 header may be read. */
    if (frame_bytes < ETH_HEADER_SIZE + IPV4_HEADER_SIZE) {
        drop("the frame is too short to hold an IPv4 header");
        return;
    }

    const ethernet_header_t *const eth = (const ethernet_header_t *)frame;
    const ipv4_header_t *const     ip  = (const ipv4_header_t *)(frame + ETH_HEADER_SIZE);

    /* 2. The header describes itself: version 4, and a length in 32-bit words
     *    that is at least the fixed twenty bytes. */
    const uint32_t header_bytes = (uint32_t)IPV4_IHL(ip->version_ihl) * 4u;

    if (IPV4_VERSION(ip->version_ihl) != 4u) {
        drop("the version field is not 4");
        return;
    }

    if (header_bytes < IPV4_HEADER_SIZE) {
        drop("IHL is below 5");
        return;
    }

    /* 3. The datagram is at least as long as its own header. ntohs() on the
     *    way in: the field is big-endian, and read raw a 60-byte datagram
     *    claims to be 15360. */
    const uint32_t total_length = ntohs(ip->total_length);

    if (total_length < header_bytes) {
        drop("the total length is shorter than the header");
        return;
    }

    /* 4. And the frame is at least as long as the datagram. It may be longer
     *    -- Ethernet pads a short frame to 60 bytes -- and whatever lies past
     *    total_length is padding, never payload. */
    if (ETH_HEADER_SIZE + total_length > frame_bytes) {
        drop("the total length runs past the end of the frame");
        return;
    }

    /* 5. No larger than one untagged Ethernet frame carries. The receive path
     *    tolerates a few bytes more for a VLAN tag, but this machine accepts
     *    no datagram it could not itself have sent in one frame -- and the cap
     *    is also what bounds everything above: at most 1500 - 20 = 1480 bytes
     *    of ICMP or UDP, so a reply is at most 14 + 20 + 1480, exactly one full
     *    frame, and UDP's checksum scratch buffer is at most 12 + 1480. */
    if (total_length > IPV4_MAX_TOTAL_LENGTH) {
        drop("the datagram is larger than one Ethernet frame carries");
        return;
    }

    /* 6. The header checksum, over the whole header, options included. Summed
     *    as it arrived -- checksum field and all -- a correct header gives 0.
     *    Steps 2-4 proved every one of those bytes is inside the frame. */
    if (net_checksum(ip, header_bytes) != 0u) {
        drop("bad header checksum");
        return;
    }

    /* 7. Not a fragment. More-fragments set, or a non-zero offset, means this
     *    is a piece of a datagram, and there is nothing to put pieces back
     *    together with. Don't-fragment is not a fragment marker. */
    if ((ntohs(ip->flags_fragment) & IPV4_FRAGMENT_BITS) != 0u) {
        drop("a fragment, and there is no reassembly");
        return;
    }

    /* 8. Addressed to this machine, at both layers. handle_ethernet has already
     *    passed only frames for this card's MAC or for broadcast, so the MAC
     *    test here exists to refuse the second kind: a datagram for 10.0.2.15
     *    that arrived as a link-layer broadcast. RFC 1122 (3.2.2.6) lets a host
     *    decline to answer an echo request sent to a broadcast address, and a
     *    host that answers them is one half of every broadcast-ping flood. */
    if (ntohl(ip->dest_ip) != NET_LOCAL_IP) {
        drop("addressed to another machine");
        return;
    }

    if (!mac_equal(eth->dst_mac, rtl8139_mac())) {
        drop("not sent to this card's MAC address");
        return;
    }

    /* 9. From a single, ordinary host. */
    const uint32_t    source = ntohl(ip->src_ip);
    const char *const reason = bad_source(source, eth->src_mac);

    if (reason != 0) {
        drop(reason);
        return;
    }

    /* 10. Hand it to whatever speaks the protocol inside. The payload starts
     *     after the header's own length, so any options are stepped over and
     *     never read, and it ends at total_length, not at the frame's end. */
    const uint8_t *const payload       = frame + ETH_HEADER_SIZE + header_bytes;
    const uint32_t       payload_bytes = total_length - header_bytes;

    if (ip->protocol == IPV4_PROTOCOL_ICMP) {
        icmp_receive(eth, ip, payload, payload_bytes);
        return;
    }

    if (ip->protocol == IPV4_PROTOCOL_UDP) {
        udp_receive(eth, ip, payload, payload_bytes);
        return;
    }

    static char line[96];

    char *const limit = U_LIMIT(line);
    char       *out   = line;

    out = u_append(out, limit, "  [net] IPv4 protocol ");
    out = u_append_dec(out, limit, ip->protocol);
    out = u_append(out, limit, " from ");
    out = net_append_ipv4(out, limit, source);
    out = u_append(out, limit, ": nothing here speaks it\n");
    *out = '\0';

    u_print(line);
}

uint8_t *ipv4_write_reply_header(uint8_t *out, const ipv4_header_t *request, uint8_t protocol,
                                 uint32_t payload_bytes)
{
    ipv4_header_t *const header = (ipv4_header_t *)out;

    header->version_ihl    = (uint8_t)((4u << 4) | (IPV4_HEADER_SIZE / 4u));
    header->tos            = (uint8_t)(request->tos & IPV4_TOS_DSCP_MASK);
    header->total_length   = htons((uint16_t)(IPV4_HEADER_SIZE + payload_bytes));
    header->id             = htons(next_datagram_id++);
    header->flags_fragment = 0;
    header->ttl            = IPV4_DEFAULT_TTL;
    header->protocol       = protocol;
    header->src_ip         = htonl(NET_LOCAL_IP);
    header->dest_ip        = request->src_ip; /* network order in, network order out */

    /* The checksum field is zeroed FIRST, then the checksum is computed over
     * the header, then stored. The checksum is defined over the header with
     * that field zero; computing it with anything else there -- whatever a
     * previous reply left in the caller's buffer -- sums the stale value in and
     * produces a complement of garbage. */
    header->checksum = 0;
    header->checksum = htons(net_checksum(header, IPV4_HEADER_SIZE));

    return out + IPV4_HEADER_SIZE;
}
