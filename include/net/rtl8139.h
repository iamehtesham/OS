#ifndef NET_RTL8139_H
#define NET_RTL8139_H

#include <stdbool.h>
#include <stdint.h>

/* The card as the network server drives it, in ring 3.
 *
 * Not to be confused with include/arch/rtl8139.h, which is the register map --
 * hardware facts with no owner. This header is the driver module: the state and
 * the operations that belong to src/user/net_server/rtl8139.c.
 *
 * It owns transmission and the card's own identity. Receiving still lives in
 * main.c with the ring it drains. */

/* Binds the module to a card already found on the PCI bus and brought up, reads
 * its MAC out of the ID registers, and allocates the two DMA pages the four
 * transmit buffers are carved from. False if the kernel refuses the DMA. */
bool rtl8139_transmit_init(uint16_t io_port_base);

/* This card's MAC address, six bytes, valid after rtl8139_transmit_init. */
const uint8_t *rtl8139_mac(void);

/* Puts one frame on the wire: copies it into the next transmit buffer, hands
 * the card that buffer's physical address, and writes the length to start the
 * send. Frames shorter than the Ethernet minimum are zero-padded up to it.
 *
 * Returns once the card has been told to send, not once it has sent -- the
 * completion arrives later as a TOK interrupt. False if the frame is longer
 * than Ethernet allows (ETH_MAX_FRAME), or if the next descriptor in order
 * never came back -- in which case the transmitter is flagged as wedged and
 * every send fails at once until the card is reset. See
 * rtl8139_transmit_wedged.
 *
 * `length` excludes the CRC, which the card appends itself. */
bool rtl8139_send_packet(const void *packet, uint32_t length);

/* How many frames have been handed to the card, for the log line. */
uint32_t rtl8139_packets_sent(void);

/* True once a descriptor the card was given has failed to come back. Every send
 * fails immediately from then on, until the card is reset and
 * rtl8139_transmit_reset is called. */
bool rtl8139_transmit_wedged(void);

/* Forgets every descriptor handed out and starts again at 0. Call it after the
 * card has been reset and ONLY then: the card's own pointer goes back to 0 on
 * reset, and the two must agree, because the card transmits in order and will
 * not look at any descriptor but the one it expects. */
void rtl8139_transmit_reset(void);

#endif /* NET_RTL8139_H */
