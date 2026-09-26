#ifndef NET_CHECKSUM_H
#define NET_CHECKSUM_H

#include <stdint.h>

/* The Internet checksum (RFC 1071), used by the IPv4 header, ICMP and UDP. UDP
 * sums a pseudo-header of IP fields in front of its segment; see udp.c.
 *
 * It is the 16-bit one's complement of the one's-complement sum of the data
 * taken as 16-bit words, with an odd final byte padded by a zero byte on the
 * right. One's-complement addition is ordinary addition with the carry out of
 * bit 15 fed back into bit 0 -- arithmetic modulo 65535, not 65536 -- and that
 * one difference is why the protocol uses it:
 *
 *   - 256 * 256 = 65536, which is 1 modulo 65535, so multiplying a word by 256
 *     rotates it by eight bits. A byte swap is therefore a multiplication, and
 *     the sum of byte-swapped words is the byte-swapped sum: a little-endian and
 *     a big-endian machine get byte-identical checksums.
 *   - No carry is ever discarded, so every bit position is protected alike.
 *     Modulo 65536, two errors in bit 15 in the SAME direction -- both 0 to 1,
 *     or both 1 to 0 -- change the sum by 65536, which is 0, and go unseen;
 *     modulo 65535 that change is 1 and is caught. Errors in OPPOSITE
 *     directions in the same bit cancel under either arithmetic -- one's
 *     complement does not catch those, it only stops the top bit being worse
 *     than the rest. (An earlier version of this comment claimed any two
 *     top-bit errors were caught; tools/check_checksum.py now tests both
 *     directions.)
 *   - A receiver checks by summing everything, the checksum field included. A
 *     correct message sums to all ones, whose complement is zero, so the checker
 *     never needs to know where the checksum field is.
 *
 * It is a weak check -- reordering 16-bit words, inserting zero words, or
 * swapping 0x0000 for 0xFFFF all go unnoticed -- which is why the Ethernet CRC
 * sits underneath it.
 *
 * CONTRACT
 *   - Returns the checksum as a HOST-order number. Store it with htons().
 *   - To COMPUTE one, zero the checksum field first: the checksum is defined
 *     over the header as if that field were zero, and summing the old value in
 *     produces a complement of garbage.
 *   - To CHECK one, run this over the received bytes with the checksum field
 *     left as it arrived. A correct message gives 0.
 *   - Any length; the accumulator is 64 bits wide, so nothing a network could
 *     deliver comes near overflowing it.
 *
 * Implemented in src/user/lib/net.c. */

uint16_t net_checksum(const void *data, uint32_t length);

#endif /* NET_CHECKSUM_H */
