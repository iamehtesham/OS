/* Address Resolution Protocol, in ring 3.
 *
 * This is the first thing this system has ever said on a network. Everything
 * before it listened: the card was brought up, packets were read out of a DMA
 * ring, headers were parsed and printed. ARP is where the machine answers.
 *
 * It answers exactly one question -- "who has <the address DHCP leased us>?"
 * -- with exactly one fact: this card's MAC address, and only once a lease is
 * bound; before that the machine has no address to answer for. There is no ARP
 * cache: every reply the layers above send goes back to the MAC its request
 * came from, and what the machine originates -- DHCP, and the one question it
 * asks here, for its router -- is broadcast, so nothing looks an address up,
 * and caching answers nobody reads would be storing work for its own sake. */

#include <stdbool.h>
#include <stdint.h>

#include "net/arp.h"
#include "net/byteorder.h"
#include "net/ethernet.h"
#include "net/ipv4.h"
#include "net/netcfg.h"
#include "net/rtl8139.h"
#include "user/ulib.h"

/* The broadcast address: every station on the segment accepts it, which is how
 * a question reaches a machine whose MAC is the thing being asked for. */
static const uint8_t broadcast_mac[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* An ARP frame is a 14-byte Ethernet header and a 28-byte ARP header.
 * rtl8139_send_packet zero-pads it to the 60-byte minimum on the way out; the
 * card itself pads nothing. */
#define ARP_FRAME_SIZE (ETH_HEADER_SIZE + ARP_HEADER_SIZE)

/* Fills in the parts of an ARP header that every ARP frame shares. Every
 * multi-byte field goes through htons: these are numbers, and this machine
 * stores numbers backwards from the way the wire carries them. */
static void write_arp_common(arp_header_t *arp, uint16_t opcode)
{
    arp->hardware_type = htons(HARDWARE_TYPE_ETHERNET);
    arp->protocol_type = htons(ETHERTYPE_IPV4);
    arp->hardware_size = ETH_ALEN;
    arp->protocol_size = 4u;
    arp->opcode        = htons(opcode);
}

/* Answers one request. The reply is the request turned around: what was the
 * sender becomes the target, and this machine becomes the sender. */
static void send_reply(const arp_header_t *request)
{
    static uint8_t frame[ARP_FRAME_SIZE];

    const uint8_t *const our_mac = rtl8139_mac();

    /* Addressed straight back to whoever asked, not broadcast: the answer is
     * only of interest to the machine that posed the question. */
    uint8_t *const payload = net_write_ethernet_header(frame, request->sender_mac, our_mac,
                                                       ETHERTYPE_ARP);
    arp_header_t *const reply = (arp_header_t *)payload;

    write_arp_common(reply, ARP_REPLY);

    /* This machine is the sender now. The MAC is a byte array and copies as
     * bytes; the IP is a number and needs htonl, or the far end reads 15.2.0.10
     * and throws the reply away. */
    u_memcpy(reply->sender_mac, our_mac, ETH_ALEN);
    reply->sender_ip = htonl(net_config()->address);

    /* And the asker is the target. Its address fields are copied straight from
     * the request, still in network order -- converting them to host order and
     * back would be two swaps that cancel, and one of them would eventually be
     * forgotten. */
    u_memcpy(reply->target_mac, request->sender_mac, ETH_ALEN);
    reply->target_ip = request->sender_ip;

    if (rtl8139_send_packet(frame, ARP_FRAME_SIZE)) {
        static char line[96];

        char *const limit = U_LIMIT(line);
        char       *out   = line;

        out = u_append(out, limit, "  [net] ARP reply sent: ");
        out = net_append_ipv4(out, limit, net_config()->address);
        out = u_append(out, limit, " is at ");
        out = net_append_mac(out, limit, our_mac);
        out = u_append(out, limit, "\n");
        *out = '\0';

        u_print(line);
    } else {
        u_print("  [net] could not transmit the ARP reply\n");
    }
}

void arp_receive(const uint8_t *frame, uint32_t frame_bytes)
{
    /* A frame claiming to be ARP that is too short to hold an ARP header is not
     * one. Checking before the cast is the same discipline the Ethernet header
     * gets: the length came off the wire, so it is an assertion by a stranger. */
    if (frame_bytes < ARP_FRAME_SIZE) {
        u_printf("  [net] an ARP frame of %u bytes is too short to hold a header\n", frame_bytes);
        return;
    }

    const arp_header_t *const arp = (const arp_header_t *)(frame + ETH_HEADER_SIZE);

    /* Only Ethernet-and-IPv4 ARP is answerable here, and only with the sizes
     * that combination implies. Anything else is a protocol this system does
     * not speak, not a malformed packet. */
    if (ntohs(arp->hardware_type) != HARDWARE_TYPE_ETHERNET ||
        ntohs(arp->protocol_type) != ETHERTYPE_IPV4 || arp->hardware_size != ETH_ALEN ||
        arp->protocol_size != 4u) {
        u_print("  [net] an ARP frame for some other hardware or protocol; ignoring it\n");
        return;
    }

    const uint16_t opcode    = ntohs(arp->opcode);
    const uint32_t target_ip = ntohl(arp->target_ip);
    const uint32_t sender_ip = ntohl(arp->sender_ip);

    static char line[96];

    char *const limit = U_LIMIT(line);
    char       *out   = line;

    out = u_append(out, limit, opcode == ARP_REQUEST ? "  [net] ARP request: who has "
                                                     : "  [net] ARP reply: ");
    out = net_append_ipv4(out, limit, opcode == ARP_REQUEST ? target_ip : sender_ip);
    out = u_append(out, limit, opcode == ARP_REQUEST ? "? tell " : " is at ");

    if (opcode == ARP_REQUEST) {
        out = net_append_ipv4(out, limit, sender_ip);
    } else {
        out = net_append_mac(out, limit, arp->sender_mac);
    }

    out = u_append(out, limit, "\n");
    *out = '\0';

    u_print(line);

    if (opcode != ARP_REQUEST) {
        return; /* a reply: nothing here asked a question, so nothing is owed */
    }

    /* The comparison that decides whether to answer. Both sides are in host
     * order -- the field was converted above, the lease is kept that way.
     * Comparing the raw field against it would silently never match, which
     * looks exactly like a network that is not asking. And only with a lease:
     * unbound, the "address" is 0.0.0.0, and a request for 0.0.0.0 is another
     * host's probe, not a question for us. */
    const net_config_t *const config = net_config();

    if (!config->bound || target_ip != config->address) {
        return; /* no address yet, or someone else's; not ours to answer for */
    }

    /* The reply goes to the MAC the request names as its sender. A group
     * address there -- broadcast or multicast, the low bit of the first octet
     * -- would turn one request into a reply to every station on the segment,
     * and no single host has one: ipv4.c refuses a group source MAC for the
     * same reason. */
    if ((arp->sender_mac[0] & 0x01u) != 0u) {
        u_print("  [net] ARP request from a group MAC address; not answering it\n");
        return;
    }

    send_reply(arp);
}

bool arp_request(uint32_t target_ip)
{
    static uint8_t frame[ARP_FRAME_SIZE];

    const uint8_t *const our_mac = rtl8139_mac();

    /* A question goes to everyone: the whole point is that the MAC of the
     * machine being asked about is the unknown. */
    uint8_t *const payload =
        net_write_ethernet_header(frame, broadcast_mac, our_mac, ETHERTYPE_ARP);
    arp_header_t *const arp = (arp_header_t *)payload;

    write_arp_common(arp, ARP_REQUEST);

    u_memcpy(arp->sender_mac, our_mac, ETH_ALEN);
    arp->sender_ip = htonl(net_config()->address);

    /* The target MAC is what is being asked for, so it goes out as zeroes --
     * the blank in the question. */
    u_memset(arp->target_mac, 0, ETH_ALEN);
    arp->target_ip = htonl(target_ip);

    if (!rtl8139_send_packet(frame, ARP_FRAME_SIZE)) {
        u_print("  [net] could not transmit the ARP request\n");
        return false;
    }

    static char line[96];

    char *const limit = U_LIMIT(line);
    char       *out   = line;

    out = u_append(out, limit, "  [net] ARP request sent: who has ");
    out = net_append_ipv4(out, limit, target_ip);
    out = u_append(out, limit, "? tell ");
    out = net_append_ipv4(out, limit, net_config()->address);
    out = u_append(out, limit, "\n");
    *out = '\0';

    u_print(line);

    return true;
}
