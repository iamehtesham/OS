#ifndef NET_ICMP_H
#define NET_ICMP_H

#include <stdint.h>

#include "net/ethernet.h"
#include "net/ipv4.h"

/* An ICMP echo message (RFC 792), as it appears on the wire.
 *
 * Every ICMP message starts with type, code and checksum. What follows depends
 * on the type; for echo request and echo reply it is an identifier and a
 * sequence number, chosen by whoever sent the request and returned unchanged,
 * then arbitrary data, also returned unchanged. That is the only kind of ICMP
 * message this system handles, so the struct describes it and not the general
 * case. `data` is a flexible array member: it takes no space in the struct and
 * names the first byte after the eight-byte header.
 *
 * The checksum covers the whole message -- header and data -- and nothing of
 * the IPv4 header in front of it. */

#define ICMP_HEADER_SIZE 8u

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
    uint8_t  data[];
} __attribute__((packed)) icmp_header_t;

_Static_assert(sizeof(icmp_header_t) == ICMP_HEADER_SIZE,
               "icmp_header_t must be exactly 8 bytes before its data: it is cast over wire data");
_Static_assert(_Alignof(icmp_header_t) == 1u,
               "icmp_header_t must be byte-aligned: it is cast over an arbitrary offset");
_Static_assert(__builtin_offsetof(icmp_header_t, checksum) == 2u, "checksum at 2");
_Static_assert(__builtin_offsetof(icmp_header_t, identifier) == 4u, "identifier at 4");
_Static_assert(__builtin_offsetof(icmp_header_t, sequence) == 6u, "sequence at 6");
_Static_assert(__builtin_offsetof(icmp_header_t, data) == 8u, "data at 8");

#define ICMP_TYPE_ECHO_REPLY   0u
#define ICMP_TYPE_ECHO_REQUEST 8u

/* Handles one ICMP message that ipv4_receive has already validated as far as
 * IPv4 goes: addressed to this machine, from a sane unicast source, not a
 * fragment, and `length` bytes long by the IPv4 header's own account. Answers
 * an echo request with an echo reply; ignores everything else. The Ethernet and
 * IPv4 headers are passed because the reply is addressed from them. In
 * src/user/net_server/icmp.c. */
void icmp_receive(const ethernet_header_t *eth, const ipv4_header_t *ip, const uint8_t *message,
                  uint32_t length);

#endif /* NET_ICMP_H */
