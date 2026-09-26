/* Transmitting, and the card's own identity.
 *
 * The RTL8139's transmit side is four descriptors used strictly in rotation.
 * Each is a pair of registers: TSAD holds a buffer's physical address, TSD is
 * write-the-length-to-send and read-it-back-for-status. There is no ring and no
 * list -- four slots, and the card works through them strictly in order, so the
 * driver waits for the NEXT one to come back, never for "any free one".
 *
 * The buffers themselves are two contiguous DMA pages from the kernel, carved
 * into four 1536-byte pieces. They must be physically contiguous and pinned
 * for the same reason the receive ring is: the card is given a physical address
 * and keeps
 * using it, and a physical address the kernel could reclaim underneath it is a
 * pointer into whatever process got those frames next. */

#include <stdbool.h>
#include <stdint.h>

#include "arch/io.h"
#include "arch/rtl8139.h"
#include "net/ethernet.h"
#include "net/rtl8139.h"
#include "user/ulib.h"

/* Every frame this machine sends must fit one buffer, and the largest it sends
 * is a full untagged frame. */
_Static_assert(RTL_TX_BUFFER_SIZE >= ETH_MAX_FRAME,
               "a transmit buffer must hold the largest frame this machine sends");

static uint16_t io_base;
static uint8_t  mac[ETH_ALEN];

static uint8_t *tx_buffers;  /* the RTL_TX_PAGES DMA pages, as this process sees them */
static uint32_t tx_phys;     /* and as the card sees them                             */
static uint32_t tx_index;    /* which descriptor is next             */
static uint32_t tx_sent;

/* Whether a descriptor has been handed to the card since the last reset.
 *
 * Not because an untouched descriptor would hang a wait -- that is what the
 * first version of this comment claimed, and it is false where it can be
 * checked: QEMU's card reads 0x00002000 in every TSD after reset, OWN already
 * set, so a bare wait on a fresh descriptor returns at once. It is kept because
 * that is a reset value this code has only observed in an emulator, and "free
 * because I never gave it away" is a fact the driver knows for certain without
 * asking the card. */
static bool tx_queued[RTL_TX_DESCRIPTORS];

/* Set when a descriptor the card was given never came back. See
 * rtl8139_send_packet for why that cannot be fixed by skipping it. */
static bool tx_wedged;

/* How long to wait for a descriptor to come back before giving up on it. A send
 * is called from the interrupt path, so this cannot block indefinitely. */
#define TX_WAIT_SPINS 1000000u

bool rtl8139_transmit_init(uint16_t io_port_base)
{
    io_base = io_port_base;

    for (uint32_t i = 0; i < ETH_ALEN; i++) {
        mac[i] = inb(io_base + RTL_IDR0 + i);
    }

    tx_buffers = (uint8_t *)(uintptr_t)u_alloc_dma(RTL_TX_PAGES, &tx_phys);

    if (tx_buffers == 0) {
        u_print("  [net] could not allocate the DMA transmit buffers\n");
        return false;
    }

    tx_sent = 0;
    rtl8139_transmit_reset();

    return true;
}

void rtl8139_transmit_reset(void)
{
    tx_index  = 0;
    tx_wedged = false;

    for (uint32_t i = 0; i < RTL_TX_DESCRIPTORS; i++) {
        tx_queued[i] = false;
    }
}

bool rtl8139_transmit_wedged(void)
{
    return tx_wedged;
}

const uint8_t *rtl8139_mac(void)
{
    return mac;
}

uint32_t rtl8139_packets_sent(void)
{
    return tx_sent;
}

/* True once the card has finished with this descriptor's buffer. A descriptor
 * never used is free by definition. */
static bool descriptor_free(uint32_t index)
{
    if (!tx_queued[index]) {
        return true;
    }

    return (inl(io_base + RTL_TSD0 + index * 4u) & RTL_TSD_OWN) != 0;
}

bool rtl8139_send_packet(const void *packet, uint32_t length)
{
    if (tx_buffers == 0) {
        return false; /* no transmit buffers: init failed or never ran */
    }

    /* Bounded by what Ethernet allows, not by the buffer, which is larger than
     * a frame and rounded for the card's alignment. A bound on the buffer would
     * let an oversized frame through. Splitting one across descriptors is not
     * a thing the card does -- one descriptor is one frame. */
    if (length == 0u || length > ETH_MAX_FRAME) {
        return false;
    }

    /* Once one descriptor has failed to come back, every send fails fast until
     * the card is reset. Spinning out the full wait again on each frame would
     * spend the interrupt path proving the same thing repeatedly. */
    if (tx_wedged) {
        return false;
    }

    uint32_t spins = 0;

    while (!descriptor_free(tx_index)) {
        if (++spins > TX_WAIT_SPINS) {
            /* The card was given this descriptor and never finished with it.
             *
             * The tempting fix is to skip it and use the next one, and it does
             * not work: the RTL8139 transmits its descriptors strictly in order,
             * 0 to 3 and round, so while it is stuck on this one it will not
             * look at any other. Checked from the QEMU monitor, not assumed --
             * with the card waiting on descriptor 1, a write to descriptor 2
             * sent nothing and left it pending. Skipping would report success
             * for frames that never leave the buffer.
             *
             * The first version returned here without recording anything, so
             * every later send came back to this same descriptor, spun the whole
             * wait again, and failed again: one stuck descriptor silenced the
             * transmitter for good, while the log blamed all four. What it
             * actually needs is the card's own pointer back at 0, which only a
             * reset does, and a reset cannot happen in the middle of a receive
             * drain. So flag it, drop this frame, and let the interrupt loop
             * reset the card once the drain is finished. */
            u_printf("  [net] transmit descriptor %u never completed; resetting the card\n",
                     tx_index);
            tx_wedged = true;
            return false;
        }
    }

    uint8_t *const buffer = tx_buffers + tx_index * RTL_TX_BUFFER_SIZE;

    u_memcpy(buffer, packet, length);

    /* Pad up to the Ethernet minimum. The card does not do this for us, and a
     * 42-byte ARP reply sent as 42 bytes is a runt that a real switch would
     * drop. Zeroes, not whatever the last frame left in this buffer -- padding
     * with stale bytes is how a driver leaks the contents of old packets to
     * everyone on the segment. */
    if (length < ETH_MIN_FRAME) {
        u_memset(buffer + length, 0, ETH_MIN_FRAME - length);
        length = ETH_MIN_FRAME;
    }

    /* The card speaks physical, exactly as it does for the receive ring. */
    outl(io_base + RTL_TSAD0 + tx_index * 4u, tx_phys + tx_index * RTL_TX_BUFFER_SIZE);

    /* Writing the length is the send. It also clears OWN, which is how the card
     * learns the buffer is its to read. */
    outl(io_base + RTL_TSD0 + tx_index * 4u, length | RTL_TSD_ERTXTH);

    tx_queued[tx_index] = true;
    tx_index            = (tx_index + 1u) % RTL_TX_DESCRIPTORS;
    tx_sent++;

    return true;
}
