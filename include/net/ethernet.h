#ifndef NET_ETHERNET_H
#define NET_ETHERNET_H

#include <stdint.h>

/* The Ethernet II frame header (IEEE 802.3), as it appears on the wire.
 *
 * Six bytes of destination MAC, six of source, then a 16-bit EtherType naming
 * whatever is encapsulated after it. Fourteen bytes, no alignment, big-endian.
 *
 * PACKED IS NOT OPTIONAL. Left to itself GCC lays a struct out for the 32-bit
 * x86 ABI, which aligns every member to its own size and rounds the struct's
 * total size up to its strictest member's alignment -- one aligned bus cycle
 * per field instead of two, which is the right default for a struct the
 * compiler owns end to end. It is the wrong default for this one, because these
 * fourteen bytes were laid out by a machine on the other end of a cable and
 * IEEE 802.3, not the System V ABI, decides where each field sits. Casting this
 * struct over a received packet asserts that its layout IS the wire's layout.
 * __attribute__((packed)) is what makes the assertion true: members at
 * consecutive byte offsets, no interior padding, no size rounding.
 *
 * What packing costs here is nothing, which is worth stating precisely because
 * the textbook answer says otherwise. On a target that cannot load a misaligned
 * word, the compiler must split a packed 16-bit read into bytes and shifts; x86
 * does misaligned loads in hardware, so GCC emits the same single movzx it would
 * for an aligned field. That was checked rather than assumed -- the EtherType
 * read compiles to one `movzx eax, WORD PTR [esi+0x10]` at -O0, -O2 and -O3.
 * The attribute earns its place by fixing the layout and by declaring the type's
 * alignment as 1, which is what makes casting it over an arbitrary offset in a
 * DMA ring correct rather than merely lucky -- not by buying anything back at
 * run time, and not by costing anything either.
 *
 * The asserts below are the guarantee rather than the hope. If a member is ever
 * added, reordered, or widened in a way that reintroduces padding, this fails
 * to compile instead of silently misreading every packet at run time. */

#define ETH_ALEN        6u  /* bytes in a MAC address    */
#define ETH_HEADER_SIZE 14u /* 6 + 6 + 2, and nothing else */

/* The shortest frame Ethernet allows on the wire, excluding the CRC. It exists
 * because collision detection needs a frame to still be transmitting when the
 * far end of the cable answers; a shorter one is padded rather than sent short.
 * An ARP reply is 42 bytes and so is always padded. */
#define ETH_MIN_FRAME 60u

/* The longest untagged frame, excluding the CRC: a 1500-byte payload and the
 * 14-byte header. The receive side tolerates 4 bytes more for a VLAN tag, but
 * nothing this machine sends is tagged, so nothing it sends exceeds this. */
#define ETH_MAX_FRAME 1514u

typedef struct {
    uint8_t  dst_mac[ETH_ALEN];
    uint8_t  src_mac[ETH_ALEN];
    uint16_t ethertype; /* BIG-ENDIAN on the wire: read it through ntohs */
} __attribute__((packed)) ethernet_header_t;

_Static_assert(sizeof(ethernet_header_t) == ETH_HEADER_SIZE,
               "ethernet_header_t must be exactly 14 bytes: it is cast over wire data");
/* The offset asserts alone would NOT catch someone deleting the attribute: this
 * struct's members land on the same offsets either way, and only its alignment
 * changes (1 packed, 2 plain). Alignment 1 is also the property the cast needs,
 * since the frame starts at an arbitrary offset in the DMA ring, so assert it
 * directly -- measured, not assumed. */
_Static_assert(_Alignof(ethernet_header_t) == 1u,
               "ethernet_header_t must be byte-aligned: it is cast over an arbitrary offset");
_Static_assert(__builtin_offsetof(ethernet_header_t, dst_mac) == 0u, "dst_mac must be at offset 0");
_Static_assert(__builtin_offsetof(ethernet_header_t, src_mac) == 6u, "src_mac must be at offset 6");
_Static_assert(__builtin_offsetof(ethernet_header_t, ethertype) == 12u,
               "ethertype must be at offset 12");

/* EtherTypes, in host order -- compare them against ntohs(eth->ethertype), not
 * against the raw field. Values below 0x0600 are an 802.3 length field instead
 * of a type; everything here is comfortably above that line. */
#define ETHERTYPE_IPV4 0x0800u
#define ETHERTYPE_ARP  0x0806u
#define ETHERTYPE_IPV6 0x86DDu

/* ---- building and formatting, in src/user/lib/net.c --------------------- */

/* Lays an Ethernet header into `frame` and returns where the payload starts.
 * The EtherType is the only field that is a number rather than bytes, so it is
 * the only one converted; it is given in host order. */
uint8_t *net_write_ethernet_header(uint8_t *frame, const uint8_t *destination,
                                   const uint8_t *source, uint16_t ethertype);

/* Appends "52:54:00:12:34:56" -- six zero-padded lowercase hex bytes, colon
 * separated. Returns the new end and never writes past `limit`, like every
 * u_append in the user runtime; the caller writes the NUL. */
char *net_append_mac(char *out, const char *limit, const uint8_t *mac);

/* Appends a 16-bit value as "0x0800": always 0x and always four digits, so a
 * column of EtherTypes lines up and a leading zero cannot be misread as a
 * shorter number. The runtime's u_append_hex is fixed at 32 bits, which would
 * print an EtherType as 0x00000800. */
char *net_append_hex16(char *out, const char *limit, uint16_t value);

/* "IPv4", "ARP", "IPv6", or NULL for an EtherType this system does not know --
 * NULL rather than "unknown" so a caller can choose to print nothing and leave
 * the raw hex to speak for itself. Takes a HOST-order value. */
const char *net_protocol_name(uint16_t ethertype);

#endif /* NET_ETHERNET_H */
