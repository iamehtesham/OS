#ifndef NET_DHCP_H
#define NET_DHCP_H

#include <stdbool.h>
#include <stdint.h>

/* DHCP (RFC 2131), the client half: how a machine with no address asks for one.
 *
 * A DHCP message is a BOOTP message (RFC 951): a fixed 236-byte header, then a
 * four-byte "magic cookie" saying the rest is DHCP options, then the options
 * themselves -- a run of [code][length][data] records, ended by an End option.
 * It travels in UDP, client port 68 to server port 67 and back, and before the
 * client has an address it goes from 0.0.0.0 to the limited broadcast.
 *
 * Every multi-byte field is big-endian. Like the other wire structs here it is
 * packed and cast over bytes at an arbitrary offset, and its alignment is what
 * the assert checks -- its fields happen to fall on natural boundaries (the
 * 32-bit ones at 4, 12, 16, 20 and 24), so GCC would lay it out the same way
 * without the attribute, and only the alignment would change. */

#define DHCP_HEADER_SIZE 236u

typedef struct {
    uint8_t  op;     /* 1 = BOOTREQUEST (client), 2 = BOOTREPLY (server)       */
    uint8_t  htype;  /* hardware type: 1 = Ethernet                            */
    uint8_t  hlen;   /* hardware address length: 6 for Ethernet                */
    uint8_t  hops;   /* relay agents count themselves here; 0 from a client    */
    uint32_t xid;    /* transaction id, chosen by the client, echoed by servers */
    uint16_t secs;   /* seconds since the client began; 0 is allowed           */
    uint16_t flags;  /* the top bit is BROADCAST                               */
    uint32_t ciaddr; /* the client's address, if it already has one            */
    uint32_t yiaddr; /* "your" address: what the server offers or grants       */
    uint32_t siaddr; /* next server (boot); not used here                      */
    uint32_t giaddr; /* relay agent; not used here                             */
    uint8_t  chaddr[16]; /* the client's hardware address, zero-padded         */
    uint8_t  sname[64];  /* server host name, or options when overloaded       */
    uint8_t  file[128];  /* boot file name, or options when overloaded         */
} __attribute__((packed)) dhcp_header_t;

_Static_assert(sizeof(dhcp_header_t) == DHCP_HEADER_SIZE,
               "dhcp_header_t must be exactly 236 bytes: RFC 2131 fixes it");
_Static_assert(_Alignof(dhcp_header_t) == 1u,
               "dhcp_header_t must be byte-aligned: it is cast over an arbitrary offset");
_Static_assert(__builtin_offsetof(dhcp_header_t, xid) == 4u, "xid at 4");
_Static_assert(__builtin_offsetof(dhcp_header_t, secs) == 8u, "secs at 8");
_Static_assert(__builtin_offsetof(dhcp_header_t, flags) == 10u, "flags at 10");
_Static_assert(__builtin_offsetof(dhcp_header_t, ciaddr) == 12u, "ciaddr at 12");
_Static_assert(__builtin_offsetof(dhcp_header_t, yiaddr) == 16u, "yiaddr at 16");
_Static_assert(__builtin_offsetof(dhcp_header_t, siaddr) == 20u, "siaddr at 20");
_Static_assert(__builtin_offsetof(dhcp_header_t, giaddr) == 24u, "giaddr at 24");
_Static_assert(__builtin_offsetof(dhcp_header_t, chaddr) == 28u, "chaddr at 28");
_Static_assert(__builtin_offsetof(dhcp_header_t, sname) == 44u, "sname at 44");
_Static_assert(__builtin_offsetof(dhcp_header_t, file) == 108u, "file at 108");

/* The magic cookie, in host order: 99.130.83.99, the bytes 63 82 53 63 on the
 * wire. It goes out through htonl() and is compared after ntohl(), like any
 * other 32-bit field -- written raw, x86 would put 63 53 82 63 on the wire. */
#define DHCP_MAGIC_COOKIE   0x63825363u
#define DHCP_COOKIE_SIZE    4u
#define DHCP_OPTIONS_OFFSET (DHCP_HEADER_SIZE + DHCP_COOKIE_SIZE) /* 240 */

/* The smallest BOOTP message some servers and relays will accept (RFC 1542
 * 2.1); a client's messages are zero-padded up to it. */
#define DHCP_BOOTP_MIN 300u

#define DHCP_SERVER_PORT 67u
#define DHCP_CLIENT_PORT 68u

#define DHCP_OP_REQUEST     1u
#define DHCP_OP_REPLY       2u
#define DHCP_HTYPE_ETHERNET 1u
#define DHCP_FLAG_BROADCAST 0x8000u

/* Option 53, the message type. */
#define DHCP_DISCOVER 1u
#define DHCP_OFFER    2u
#define DHCP_REQUEST  3u
#define DHCP_DECLINE  4u
#define DHCP_ACK      5u
#define DHCP_NAK      6u
#define DHCP_RELEASE  7u
#define DHCP_INFORM   8u

/* Option codes (RFC 2132). Pad and End are the only ones with no length byte. */
#define DHCP_OPT_PAD              0u
#define DHCP_OPT_SUBNET_MASK      1u
#define DHCP_OPT_ROUTER           3u
#define DHCP_OPT_REQUESTED_IP     50u
#define DHCP_OPT_LEASE_TIME       51u
#define DHCP_OPT_OVERLOAD         52u
#define DHCP_OPT_MESSAGE_TYPE     53u
#define DHCP_OPT_SERVER_ID        54u
#define DHCP_OPT_PARAMETER_LIST   55u
#define DHCP_OPT_MAX_MESSAGE_SIZE 57u
#define DHCP_OPT_END              255u

/* A lease time of all ones means the lease never ends (RFC 2131 3.3). */
#define DHCP_LEASE_INFINITE 0xFFFFFFFFu

/* Starts a lease from nothing: a new transaction id, a DISCOVER broadcast, and
 * the retransmission timer. Called once the card is up, and again whenever a
 * transaction fails or a lease ends. In src/user/net_server/dhcp.c. */
void dhcp_start(void);

/* Handles one UDP datagram that arrived for the client port, 68: `message` is
 * its payload, `length` bytes, inside a datagram whose UDP length has been
 * verified, and whose checksum has been if the sender computed one -- a UDP
 * checksum of 0 means none, and is accepted (RFC 768), so nothing here may
 * assume the bytes are intact. `src_port` is the UDP source port. */
void dhcp_receive(uint32_t src_port, const uint8_t *message, uint32_t length);

/* The alarm the client set has gone off: retransmit, give up and start again,
 * or -- once bound -- the lease has run out. */
void dhcp_timer(void);

/* Runs dhcp_timer if the client's alarm is due, without waiting for recv to
 * deliver it. For the receive drain loop, which may not return to recv for as
 * long as frames keep arriving. */
void dhcp_poll(void);

#endif /* NET_DHCP_H */
