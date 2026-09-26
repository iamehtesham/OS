/* Byte order, the Internet checksum, and building and formatting headers, for
 * any ring-3 program that reads or writes a packet.
 *
 * It lives in the user runtime rather than in the network driver because
 * nothing here is the driver's: byte order is a property of the wire and the
 * architecture, the checksum is a property of the protocols, and a MAC address
 * formats the same way whoever is printing it. The network server is the only
 * caller today.
 *
 * There is no kernel involvement anywhere in this file. Byte swapping and
 * checksums are arithmetic, and formatting is bytes in a buffer -- a packet is
 * data, and turning data into a string needs no privilege. */

#include <stdint.h>

#include "net/byteorder.h"
#include "net/checksum.h"
#include "net/ethernet.h"
#include "net/ipv4.h"
#include "user/ulib.h"

/* ---- byte order ---------------------------------------------------------- */

/* Written as shifts over the value, never as a cast through a byte pointer.
 * The shift version says what it means in arithmetic the compiler understands
 * -- at -O2 GCC recognises both of these and emits a single rol $8 or bswap --
 * and it has no alignment requirement, so it is safe on a field just read out
 * of a packed struct sitting at an odd offset in a DMA ring. */

uint16_t htons(uint16_t value)
{
    /* value promotes to int for the shifts; the cast back drops the bits that
     * << 8 pushed above bit 15, which is exactly the swap. */
    return (uint16_t)(((uint32_t)value >> 8) | ((uint32_t)value << 8));
}

uint16_t ntohs(uint16_t value)
{
    return htons(value); /* the same swap; the name records the direction */
}

uint32_t htonl(uint32_t value)
{
    return ((value >> 24) & 0x000000FFu) | ((value >> 8) & 0x0000FF00u) |
           ((value << 8) & 0x00FF0000u) | ((value << 24) & 0xFF000000u);
}

uint32_t ntohl(uint32_t value)
{
    return htonl(value);
}

/* ---- the Internet checksum ------------------------------------------------ */

uint16_t net_checksum(const void *data, uint32_t length)
{
    const uint8_t *const bytes = (const uint8_t *)data;
    uint64_t             sum   = 0;
    uint32_t             i     = 0;

    /* Words are assembled from byte pairs, most significant first -- the wire's
     * order -- so the sum and the result are ordinary host-order numbers and the
     * caller stores the result with htons(). The alternative, loading native
     * uint16_t and storing the result unswapped, is also correct, because the
     * one's-complement sum does not care which way round the bytes are; what is
     * NOT correct is mixing the two, which swaps twice. Byte pairs also carry no
     * alignment requirement, and a header in the receive ring can start on any
     * byte. */
    for (; i + 1u < length; i += 2u) {
        sum += ((uint32_t)bytes[i] << 8) | bytes[i + 1u];
    }

    /* An odd final byte is the high half of a word whose low half is zero. */
    if (i < length) {
        sum += (uint32_t)bytes[i] << 8;
    }

    /* The end-around carry, done once at the end rather than after every add:
     * addition is associative, so carries can pile up above bit 15 and be folded
     * back in together. Folding can itself carry, hence the loop -- it runs at
     * most a handful of times. */
    while ((sum >> 16) != 0u) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

/* ---- building headers ------------------------------------------------------ */

uint8_t *net_write_ethernet_header(uint8_t *frame, const uint8_t *destination,
                                   const uint8_t *source, uint16_t ethertype)
{
    ethernet_header_t *const eth = (ethernet_header_t *)frame;

    u_memcpy(eth->dst_mac, destination, ETH_ALEN);
    u_memcpy(eth->src_mac, source, ETH_ALEN);
    eth->ethertype = htons(ethertype);

    return frame + ETH_HEADER_SIZE;
}

/* ---- formatting ---------------------------------------------------------- */

static const char hex_digits[] = "0123456789abcdef";

/* Both of these are fixed width, unlike the runtime's variable-width %x. A MAC
 * address with a zero byte in it is still six pairs, and an EtherType is still
 * four digits, because these are field widths on the wire rather than numbers
 * whose magnitude means anything. */

char *net_append_mac(char *out, const char *limit, const uint8_t *mac)
{
    for (uint32_t i = 0; i < ETH_ALEN; i++) {
        if (i != 0u && out < limit) {
            *out++ = ':';
        }

        if (out < limit) {
            *out++ = hex_digits[(mac[i] >> 4) & 0xFu];
        }

        if (out < limit) {
            *out++ = hex_digits[mac[i] & 0xFu];
        }
    }

    return out;
}

char *net_append_hex16(char *out, const char *limit, uint16_t value)
{
    if (out < limit) {
        *out++ = '0';
    }

    if (out < limit) {
        *out++ = 'x';
    }

    for (int32_t shift = 12; shift >= 0 && out < limit; shift -= 4) {
        *out++ = hex_digits[(value >> shift) & 0xFu];
    }

    return out;
}

char *net_append_ipv4(char *out, const char *limit, uint32_t address)
{
    for (uint32_t shift = 24; ; shift -= 8) {
        out = u_append_dec(out, limit, (address >> shift) & 0xFFu);

        if (shift == 0u) {
            return out;
        }

        if (out < limit) {
            *out++ = '.';
        }
    }
}

const char *net_protocol_name(uint16_t ethertype)
{
    switch (ethertype) {
    case ETHERTYPE_IPV4:
        return "IPv4";

    case ETHERTYPE_ARP:
        return "ARP";

    case ETHERTYPE_IPV6:
        return "IPv6";

    default:
        /* Not a guess and not "unknown": the raw value is already being
         * printed, and inventing a name for it would only obscure that this
         * system has no idea what it is. */
        return 0;
    }
}
