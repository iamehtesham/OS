#ifndef NET_BYTEORDER_H
#define NET_BYTEORDER_H

#include <stdint.h>

/* Host and network byte order.
 *
 * x86 is little-endian: it stores 0x0800 as the bytes 00 08. Every protocol
 * from Ethernet upward is big-endian on the wire -- "network byte order" -- so
 * the same value arrives as 08 00. A multi-byte field read straight out of a
 * packet therefore reads backwards, and the failure is quiet: 0x0800 (IPv4)
 * read without swapping is 0x0008, which is not a protocol at all, and 0x0806
 * (ARP) is 0x0608. Nothing faults; the driver just believes the wrong thing.
 *
 * These are value conversions, not memory tricks. They take a number the CPU
 * already holds and produce the number whose bytes, written in this machine's
 * order, are what the wire carries -- and back. That is why they are safe to
 * apply to a field already loaded out of a packed struct.
 *
 * ntoh and hton do the identical swap on this architecture and exist as
 * separate names because a call site should say which way it is going. The
 * distinction is real on a big-endian machine, where all four would compile to
 * nothing; this kernel is x86-only, so all four swap unconditionally.
 *
 * Implemented in src/user/lib/net.c, linked into every ring-3 program. */

uint16_t htons(uint16_t value);
uint16_t ntohs(uint16_t value);
uint32_t htonl(uint32_t value);
uint32_t ntohl(uint32_t value);

/* A big-endian 32-bit value read out of, or written into, bytes at any
 * address -- for fields that are not in a struct, like a DHCP option's data,
 * which can start at any offset and so cannot be read through a uint32_t *.
 * The load returns a HOST-order number; the store takes one. */
uint32_t net_load_be32(const uint8_t *bytes);
void     net_store_be32(uint8_t *bytes, uint32_t value);

#endif /* NET_BYTEORDER_H */
