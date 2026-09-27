#ifndef NET_IPV4_H
#define NET_IPV4_H

#include <stdint.h>

/* The IPv4 header (RFC 791), as it appears on the wire.
 *
 * Twenty bytes when it carries no options, up to sixty when it does; IHL says
 * which, in 32-bit words. Every multi-byte field is big-endian. Only the first
 * twenty bytes are a fixed layout, so only they are a struct -- options, when a
 * header has them, are stepped over by IHL and never cast.
 *
 * Unlike arp_header_t this struct happens to be naturally aligned: every field
 * already sits on a multiple of its own size, so GCC would lay it out the same
 * way without the attribute and only its alignment would change (4 plain, 1
 * packed). That makes the alignment assert the one that catches a deleted
 * attribute, and alignment 1 is what the cast needs anyway -- the header starts
 * 14 bytes into a frame that starts at an arbitrary offset in the DMA ring. */

#define IPV4_HEADER_SIZE 20u /* without options */

typedef struct {
    uint8_t  version_ihl;    /* version in the high nibble, IHL (words) in the low */
    uint8_t  tos;            /* DSCP in the top six bits, ECN in the bottom two    */
    uint16_t total_length;   /* header + payload, in bytes -- ntohs() it          */
    uint16_t id;
    uint16_t flags_fragment; /* DF, MF, then a 13-bit offset in 8-byte units     */
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;       /* over the header only, options included           */
    uint32_t src_ip;
    uint32_t dest_ip;
} __attribute__((packed)) ipv4_header_t;

_Static_assert(sizeof(ipv4_header_t) == IPV4_HEADER_SIZE,
               "ipv4_header_t must be exactly 20 bytes: it is cast over wire data");
_Static_assert(_Alignof(ipv4_header_t) == 1u,
               "ipv4_header_t must be byte-aligned: it is cast over an arbitrary offset");
_Static_assert(__builtin_offsetof(ipv4_header_t, total_length) == 2u, "total_length at 2");
_Static_assert(__builtin_offsetof(ipv4_header_t, id) == 4u, "id at 4");
_Static_assert(__builtin_offsetof(ipv4_header_t, flags_fragment) == 6u, "flags_fragment at 6");
_Static_assert(__builtin_offsetof(ipv4_header_t, ttl) == 8u, "ttl at 8");
_Static_assert(__builtin_offsetof(ipv4_header_t, protocol) == 9u, "protocol at 9");
_Static_assert(__builtin_offsetof(ipv4_header_t, checksum) == 10u, "checksum at 10");
_Static_assert(__builtin_offsetof(ipv4_header_t, src_ip) == 12u, "src_ip at 12");
_Static_assert(__builtin_offsetof(ipv4_header_t, dest_ip) == 16u, "dest_ip at 16");

/* The two halves of version_ihl. */
#define IPV4_VERSION(version_ihl) ((uint8_t)((version_ihl) >> 4))
#define IPV4_IHL(version_ihl)     ((uint8_t)((version_ihl) & 0x0Fu))

/* flags_fragment, in host order. A packet is a fragment if MF is set or its
 * offset is non-zero; either bit being present is what IPV4_FRAGMENT_BITS
 * tests. DF is not a fragment marker and is deliberately not in that mask. */
#define IPV4_FLAG_DF          0x4000u
#define IPV4_FLAG_MF          0x2000u
#define IPV4_FRAG_OFFSET_MASK 0x1FFFu
#define IPV4_FRAGMENT_BITS    (IPV4_FLAG_MF | IPV4_FRAG_OFFSET_MASK)

/* The largest datagram this machine accepts or sends: exactly what one
 * untagged Ethernet frame carries. Larger would need fragmentation, and there
 * is none, in either direction. */
#define IPV4_MAX_TOTAL_LENGTH 1500u

#define IPV4_DEFAULT_TTL   64u
#define IPV4_TOS_DSCP_MASK 0xFCu /* keep the service class, drop the ECN bits */

#define IPV4_PROTOCOL_ICMP 1u
#define IPV4_PROTOCOL_UDP  17u

/* An IPv4 address as a number, most significant byte first, so that reading the
 * constant left to right reads the address left to right: NET_IPV4(10, 0, 2, 15)
 * is 0x0A00020F. On this little-endian machine that value sits in memory as
 * 0F 02 00 0A, backwards from the wire, so it goes out through htonl() and comes
 * in through ntohl(), never straight across. */
#define NET_IPV4(a, b, c, d)                                                          \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* The limited broadcast: every host on this link. This machine's own address,
 * mask and router are no longer constants here -- they were, 10.0.2.15/24 via
 * 10.0.2.2, until DHCP (dhcp.c) could lease them. They live in net/netcfg.h. */
#define NET_BROADCAST_IP NET_IPV4(255, 255, 255, 255)

/* Appends a dotted quad from a HOST-order address. In src/user/lib/net.c. */
char *net_append_ipv4(char *out, const char *limit, uint32_t address);

/* Looks at one received frame carrying EtherType 0x0800. Validates the IPv4
 * header -- every check reads only bytes an earlier check proved are there --
 * and hands what is inside to the protocol that speaks it: ICMP to
 * icmp_receive, UDP to udp_receive. Two kinds of datagram get that far: one
 * addressed to this machine's leased address at this card's MAC, once a lease
 * is bound; and a UDP datagram to the limited broadcast 255.255.255.255, which
 * UDP then gives only to DHCP. Anything else is reported and dropped. `frame`
 * points at the Ethernet header, and `frame_bytes` is the frame's length
 * without the card's CRC. In src/user/net_server/ipv4.c. */
void ipv4_receive(const uint8_t *frame, uint32_t frame_bytes);

/* Writes an IPv4 header at `out` and returns where the payload starts. Built
 * fresh: IHL 5, the given TOS, the next identification number, no flags, TTL
 * 64, and a checksum computed with its field zeroed first. `src` and `dst` are
 * in NETWORK order -- an address copied out of a received header goes straight
 * in, never swapped there and back. `payload_bytes` is the length of what
 * follows. Every datagram this machine sends is built here, replies and the
 * ones it originates (DHCP) alike, so they draw their identification numbers
 * from one counter, which repeats only when it wraps at 65,536. In
 * src/user/net_server/ipv4.c. */
uint8_t *ipv4_write_header(uint8_t *out, uint32_t src, uint32_t dst, uint8_t protocol,
                           uint8_t tos, uint32_t payload_bytes);

/* The header of a reply to `request`: from this machine's leased address to
 * the request's source, with the request's DSCP and ECN cleared. The request's
 * options belong to its journey, a source route among them, and are not
 * copied. Only ever called once a lease is bound -- nothing unbound gets far
 * enough to be answered. */
uint8_t *ipv4_write_reply_header(uint8_t *out, const ipv4_header_t *request, uint8_t protocol,
                                 uint32_t payload_bytes);

#endif /* NET_IPV4_H */
