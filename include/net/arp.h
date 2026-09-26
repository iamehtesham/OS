#ifndef NET_ARP_H
#define NET_ARP_H

#include <stdbool.h>
#include <stdint.h>

#include "net/ethernet.h"
#include "net/ipv4.h"

/* Address Resolution Protocol (RFC 826), as it appears on the wire.
 *
 * ARP answers the one question Ethernet cannot: a machine knows the IP address
 * it wants to reach, and needs the MAC address to put in the frame. It asks by
 * broadcasting "who has 10.0.2.15?" and the machine that owns that address
 * answers with its own MAC. Twenty-eight bytes, and the same layout serves both
 * the question and the answer -- only the opcode and which fields are filled in
 * differ.
 *
 * THIS is the struct where packing earns its keep. ethernet_header_t happens to
 * lay out identically with and without the attribute, so the asserts under it
 * guard against a future edit. Here the damage is immediate: sender_ip is a
 * uint32_t landing at offset 14, an address GCC may not use for a 4-byte member,
 * so without packing it inserts two bytes before it, shifts everything after,
 * and grows the struct from 28 bytes to 32. Every field from the sender's IP
 * onward would be read from the wrong place, and nothing would warn. The asserts
 * below fail the build instead. */

#define ARP_HEADER_SIZE 28u

typedef struct {
    uint16_t hardware_type;           /* 1 for Ethernet                        */
    uint16_t protocol_type;           /* 0x0800 for IPv4                       */
    uint8_t  hardware_size;           /* 6, the length of a MAC                */
    uint8_t  protocol_size;           /* 4, the length of an IPv4 address      */
    uint16_t opcode;                  /* request or reply                      */
    uint8_t  sender_mac[ETH_ALEN];
    uint32_t sender_ip;
    uint8_t  target_mac[ETH_ALEN];    /* zero in a request: it is the question */
    uint32_t target_ip;
} __attribute__((packed)) arp_header_t;

_Static_assert(sizeof(arp_header_t) == ARP_HEADER_SIZE,
               "arp_header_t must be exactly 28 bytes: it is cast over wire data");
_Static_assert(_Alignof(arp_header_t) == 1u,
               "arp_header_t must be byte-aligned: it is cast over an arbitrary offset");
_Static_assert(__builtin_offsetof(arp_header_t, opcode) == 6u, "opcode must be at offset 6");
_Static_assert(__builtin_offsetof(arp_header_t, sender_mac) == 8u, "sender_mac must be at offset 8");
_Static_assert(__builtin_offsetof(arp_header_t, sender_ip) == 14u,
               "sender_ip must be at offset 14 -- unpadded, which is the whole point");
_Static_assert(__builtin_offsetof(arp_header_t, target_mac) == 18u,
               "target_mac must be at offset 18");
_Static_assert(__builtin_offsetof(arp_header_t, target_ip) == 24u, "target_ip must be at offset 24");

/* Wire values, in host order -- compare them against ntohs(field), and run them
 * back through htons() on the way out. */
#define HARDWARE_TYPE_ETHERNET 1u
#define ARP_REQUEST            1u
#define ARP_REPLY              2u

/* The addresses ARP answers for and asks about -- NET_LOCAL_IP, NET_GATEWAY_IP
 * and NET_IPV4 -- live in net/ipv4.h, with the IP layer they belong to. */

/* Looks at one received frame. If it is an ARP request for this machine's
 * address, builds and transmits the reply. Anything else is reported and
 * ignored. `frame` points at the Ethernet header; `frame_bytes` is its length
 * without the card's CRC. */
void arp_receive(const uint8_t *frame, uint32_t frame_bytes);

/* Broadcasts "who has `target_ip`?". Nothing here remembers the answer -- there
 * is no ARP cache. The IP layer above ARP only ever replies, and a reply goes
 * to the source MAC of the frame that asked, so nothing yet needs to look an
 * address up. This exists to prove the transmit path against a real stack. */
bool arp_request(uint32_t target_ip);

#endif /* NET_ARP_H */
