#ifndef ARCH_RTL8139_H
#define ARCH_RTL8139_H

/* Realtek RTL8139 register map and bits (Realtek RTL8139D datasheet). Hardware
 * facts, no owner; the ring-3 network driver drives them through its I/O BAR
 * under IOPL 3. Register offsets are added to the I/O base read from BAR0. */

#define RTL_VENDOR_ID 0x10ECu
#define RTL_DEVICE_ID 0x8139u

#define RTL_IDR0    0x00 /* MAC address, six bytes                            */
#define RTL_TSD0    0x10 /* u32 x4: transmit status/command, 4 bytes apart   */
#define RTL_TSAD0   0x20 /* u32 x4: transmit buffer physical address         */
#define RTL_RBSTART 0x30 /* u32: physical address of the RX ring             */
#define RTL_CMD     0x37 /* u8:  command register                            */
#define RTL_CAPR    0x38 /* u16: current address of packet read (lags CBR)   */
#define RTL_CBR     0x3A /* u16: current buffer address, the NIC's write head */
#define RTL_IMR     0x3C /* u16: interrupt mask                              */
#define RTL_ISR     0x3E /* u16: interrupt status, write 1 to clear a bit    */
#define RTL_RCR     0x44 /* u32: receive configuration                       */
#define RTL_CONFIG1 0x52 /* u8:  power management                            */

/* Command register. */
#define RTL_CMD_RESET 0x10 /* set to reset; the NIC clears it when done      */
#define RTL_CMD_RE    0x08 /* receiver enable                                */
#define RTL_CMD_TE    0x04 /* transmitter enable                             */
#define RTL_CMD_BUFE  0x01 /* read-only: RX ring empty when set              */

/* Interrupt status/mask bits. */
#define RTL_INT_ROK 0x0001u /* a packet was received OK                      */
#define RTL_INT_TOK 0x0004u /* a packet was transmitted OK                   */

/* Transmit. The card has exactly four descriptors, used strictly in turn: each
 * pairs a TSAD register holding a buffer's PHYSICAL address with a TSD register
 * that is both the command (write the length to start the send) and the status
 * (read it back for progress). The four buffers are 1536 bytes each, carved from
 * two contiguous DMA pages (6 KiB of the 8): big enough for the largest frame
 * this machine sends, a 1514-byte echo reply to a full-size ping. They were
 * 1 KiB in one page until ICMP made a reply exactly as long as its request.
 *
 * OWN is the handshake and it reads backwards from what the name suggests: the
 * driver clears it by writing a length, meaning "the card owns this now", and
 * the card SETS it when it has finished DMAing the buffer out, meaning the
 * driver may reuse it. So waiting for a free descriptor is waiting for OWN to
 * become 1, not 0. */
#define RTL_TX_DESCRIPTORS 4u
#define RTL_TX_BUFFER_SIZE 1536u
#define RTL_TX_PAGES       2u
#define RTL_TX_CHIP_MAX    1792u /* the RTL8139's own limit on one transmit */

/* Nothing else ties the buffer size to the page count, and a mismatch is not
 * loud: a buffer that runs past the allocation is a physical address the card
 * DMAs out of regardless -- whatever the kernel put in the next frame. So the
 * relationships are asserted rather than trusted. TSAD must hold a dword-
 * aligned address, so every buffer must start on a multiple of 4. */
_Static_assert(RTL_TX_DESCRIPTORS * RTL_TX_BUFFER_SIZE <= RTL_TX_PAGES * 4096u,
               "the transmit buffers must fit inside the DMA pages allocated for them");
_Static_assert(RTL_TX_BUFFER_SIZE % 4u == 0u,
               "each transmit buffer must start dword-aligned: TSAD requires it");
_Static_assert(RTL_TX_BUFFER_SIZE <= RTL_TX_CHIP_MAX,
               "a transmit buffer larger than the chip can send is wasted");

#define RTL_TSD_OWN 0x00002000u /* card has finished with the buffer         */
#define RTL_TSD_TUN 0x00004000u /* transmit FIFO underrun                    */
#define RTL_TSD_TOK 0x00008000u /* transmitted OK                            */

/* Early-transmit threshold, bits 16-21, in units of 32 bytes: how much of the
 * frame must be in the card's FIFO before it starts putting it on the wire.
 * Zero means 8 bytes, which on real hardware invites an underrun on a busy bus;
 * 256 bytes is the conventional safe setting. QEMU does not model the FIFO, so
 * this changes nothing here and everything on a real card. */
#define RTL_TSD_ERTXTH_SHIFT 16u
#define RTL_TSD_ERTXTH_UNITS 8u /* 8 * 32 = 256 bytes */
#define RTL_TSD_ERTXTH       (RTL_TSD_ERTXTH_UNITS << RTL_TSD_ERTXTH_SHIFT)

/* Receive configuration. Accept broadcast, multicast, physical-match and all
 * (promiscuous, so an injected frame is taken whatever its destination MAC);
 * WRAP so a packet crossing the ring end is written straight on into the slack
 * past it rather than split; MXDMA unlimited (7<<8); RBLEN 8 KiB (0<<11);
 * no RX FIFO threshold (7<<13, whole packet before it interrupts). */
#define RTL_RCR_AAP  (1u << 0)
#define RTL_RCR_APM  (1u << 1)
#define RTL_RCR_AM   (1u << 2)
#define RTL_RCR_AB   (1u << 3)
#define RTL_RCR_WRAP (1u << 7)
#define RTL_RCR_CONFIG \
    (RTL_RCR_AAP | RTL_RCR_APM | RTL_RCR_AM | RTL_RCR_AB | RTL_RCR_WRAP | (7u << 8) | (7u << 13))

/* The ring proper is 8 KiB (RBLEN=00). WRAP lets the NIC run up to a packet
 * past the end, so the buffer must hold 8 KiB + one max frame; three pages
 * (12 KiB) covers it, and is what sys_alloc_dma(3) asks for. */
#define RTL_RX_RING_SIZE  8192u
#define RTL_RX_RING_PAGES 3u

/* CAPR sits 16 bytes behind the read offset, a fixed hardware bias. */
#define RTL_CAPR_BIAS 16u

/* Every received packet is prefixed in the ring by a four-byte header: a
 * status u16 then a length u16. The length counts the frame AND the four-byte
 * Ethernet CRC the NIC leaves on, so the wire payload is length - 4. */
#define RTL_RX_HEADER_SIZE 4u
#define RTL_RX_STATUS_ROK  0x0001u

/* The largest length that header may legitimately carry: a 1500-byte payload,
 * a 14-byte Ethernet header, a 4-byte VLAN tag and the 4-byte CRC. The field is
 * 16 bits wide, so a confused or hostile card could claim 65535 -- far past the
 * end of the 12 KiB buffer. The driver reads inside that buffer, so the bound
 * is checked rather than assumed: overrunning it would fault this process, and
 * a driver that can be killed by a malformed frame is a driver an attacker on
 * the wire can switch off.
 *
 * Exceeding it means "do not read this frame", not "stop receiving". The length
 * is still the card's account of how far the next record is, so the driver steps
 * over the frame and keeps draining; see drain_packets, where stopping instead
 * turned out to be its own denial of service. */
#define RTL_RX_MAX_LENGTH 1522u

/* How many records one batch of the receive drain processes before handing
 * the CPU back: one ring's worth of the smallest records (a 4-byte header plus
 * the 4-byte minimum the length field can state). It bounds a unit of work, not
 * the ring. It was first written as "the most one drain can legitimately
 * produce", which is false -- the card refills space as fast as the drain frees
 * it -- and treating it as a corruption test threw away a full ring of good
 * frames under sustained traffic. */
#define RTL_RX_BATCH_RECORDS (RTL_RX_RING_SIZE / 8u)

#endif /* ARCH_RTL8139_H */
