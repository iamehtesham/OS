#ifndef NET_UDP_H
#define NET_UDP_H

#include <stdbool.h>
#include <stdint.h>

#include "net/ethernet.h"
#include "net/ipv4.h"

/* The UDP header (RFC 768), as it appears on the wire.
 *
 * Eight bytes: two ports, a length, and a checksum, all big-endian. `length`
 * counts the header and the data together, so it is never below 8. UDP adds
 * nothing else to IP -- no connection, no ordering, no retransmission -- only
 * the ports, which say which program on each machine a datagram belongs to.
 *
 * Like ipv4_header_t this struct is naturally aligned: every field is 16 bits
 * at an even offset, so GCC would lay it out the same way without the
 * attribute. The alignment assert is therefore the one that catches a deleted
 * `packed`, and alignment 1 is what the cast needs -- the header sits 14 + IHL*4
 * bytes into a frame at an arbitrary offset in the DMA ring. */

#define UDP_HEADER_SIZE 8u

typedef struct {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t length;   /* header + data, in bytes -- ntohs() it          */
    uint16_t checksum; /* over the pseudo-header, header and data; 0 = none */
} __attribute__((packed)) udp_header_t;

_Static_assert(sizeof(udp_header_t) == UDP_HEADER_SIZE,
               "udp_header_t must be exactly 8 bytes: it is cast over wire data");
_Static_assert(_Alignof(udp_header_t) == 1u,
               "udp_header_t must be byte-aligned: it is cast over an arbitrary offset");
_Static_assert(__builtin_offsetof(udp_header_t, dest_port) == 2u, "dest_port at 2");
_Static_assert(__builtin_offsetof(udp_header_t, length) == 4u, "length at 4");
_Static_assert(__builtin_offsetof(udp_header_t, checksum) == 6u, "checksum at 6");

/* The pseudo-header: twelve bytes that are never sent, only summed.
 *
 * The UDP checksum covers these fields from the IP header as well as the UDP
 * segment itself, so a datagram delivered to the wrong address, or carried
 * under the wrong protocol number, fails its checksum even though the segment
 * arrived intact. RFC 768 fixes the layout: source, destination, a zero byte,
 * the protocol (17), and the UDP length -- the same value as the header's own
 * length field, in the same network byte order.
 *
 * Naturally aligned too (4, 4, 1, 1, 2), so again only the alignment assert
 * notices a missing `packed`. It is laid over a byte buffer, never declared as
 * a local, so it needs alignment 1. */

#define UDP_PSEUDO_HEADER_SIZE 12u

typedef struct {
    uint32_t src_ip;     /* network order, as it sits in the IPv4 header */
    uint32_t dest_ip;
    uint8_t  reserved;   /* always 0                                     */
    uint8_t  protocol;   /* always IPV4_PROTOCOL_UDP                     */
    uint16_t udp_length; /* the UDP header's length field, byte for byte */
} __attribute__((packed)) udp_pseudo_header_t;

_Static_assert(sizeof(udp_pseudo_header_t) == UDP_PSEUDO_HEADER_SIZE,
               "udp_pseudo_header_t must be exactly 12 bytes: RFC 768 fixes it");
_Static_assert(_Alignof(udp_pseudo_header_t) == 1u,
               "udp_pseudo_header_t must be byte-aligned: it is laid over a byte buffer");
_Static_assert(__builtin_offsetof(udp_pseudo_header_t, dest_ip) == 4u, "dest_ip at 4");
_Static_assert(__builtin_offsetof(udp_pseudo_header_t, reserved) == 8u, "reserved at 8");
_Static_assert(__builtin_offsetof(udp_pseudo_header_t, protocol) == 9u, "protocol at 9");
_Static_assert(__builtin_offsetof(udp_pseudo_header_t, udp_length) == 10u, "udp_length at 10");

/* The longest UDP segment this machine accepts: everything a 1500-byte datagram
 * with a 20-byte header can carry. ipv4_receive refuses anything longer, and a
 * segment may not be longer than the datagram around it, so this bound is
 * what sizes the checksum's scratch buffer. */
#define UDP_MAX_LENGTH (IPV4_MAX_TOTAL_LENGTH - IPV4_HEADER_SIZE)

_Static_assert(UDP_MAX_LENGTH == 1480u, "one untagged Ethernet frame carries 1480 bytes of UDP");
_Static_assert(ETH_HEADER_SIZE + IPV4_HEADER_SIZE + UDP_MAX_LENGTH == ETH_MAX_FRAME,
               "the largest echo reply must be exactly one full frame");

/* The echo service (RFC 862): whatever arrives on port 7 is sent back. */
#define UDP_PORT_ECHO 7u

/* Source ports echo refuses to answer: every port below 1024, where the
 * well-known services that also answer unconditionally live, and NFS, which
 * trusts a client by its port. This is OpenBSD inetd's dg_badinput rule;
 * NetBSD's inetd refuses a shorter list (7, 9, 13, 19, 37). */
#define UDP_FIRST_UNPRIVILEGED_PORT 1024u
#define UDP_PORT_NFS                2049u

/* Handles one UDP segment that ipv4_receive has already validated as far as
 * IPv4 goes: from a sane unicast source, not a fragment, `length` bytes long by
 * the IPv4 header's account -- at most UDP_MAX_LENGTH -- and either addressed
 * to this machine's leased address at its MAC, or, with `broadcast` true, sent
 * to the limited broadcast. Validates the UDP header and checksum, gives a
 * datagram for port 68 to the DHCP client, echoes one sent to port 7, and
 * reports anything else. A broadcast goes to DHCP or nowhere: nothing else here
 * answers one. The Ethernet and IPv4 headers are passed because a reply is
 * addressed from them and the checksum covers the IP addresses. In
 * src/user/net_server/udp.c. */
void udp_receive(const ethernet_header_t *eth, const ipv4_header_t *ip, const uint8_t *segment,
                 uint32_t length, bool broadcast);

/* Sends `length` bytes of data as one UDP datagram from `src_port` to
 * `dst_port` (host order), at `src_ip` to `dst_ip` (NETWORK order, as
 * ipv4_write_header takes them), in an Ethernet frame to `dst_mac`. For
 * datagrams this machine originates rather than answers -- DHCP's, sent from
 * 0.0.0.0 before there is an address. Always checksummed. False if it could
 * not be built or sent. */
bool udp_send(const uint8_t *dst_mac, uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
              uint16_t dst_port, const uint8_t *data, uint32_t length);

#endif /* NET_UDP_H */
