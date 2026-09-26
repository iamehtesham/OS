/* The network driver, in ring 3.
 *
 * It owns an RTL8139 the way the keyboard driver owns the 8042: entirely from
 * user space. It finds the card by walking the PCI bus, allocates a DMA buffer
 * the card can write into, programs the card's registers over I/O ports, asks
 * the kernel to route the card's interrupt line to it, and then waits for the
 * kernel to forward each interrupt as a message. The kernel never learns what
 * an Ethernet frame is; it only carries an interrupt from a line this process
 * claimed to this process, and hands out the one thing a device needs that a
 * program cannot invent -- a physical address.
 *
 * Two kernel privileges make it possible, granted at boot because the kernel
 * decided this process is a driver: IOPL, so its in/out reach the card and the
 * PCI ports; and DMA allocation, so it can be given contiguous pinned physical
 * memory. The IRQ it claims is discovered, not granted -- the kernel could not
 * have known it before the bus was scanned. */

#include <stdbool.h>
#include <stdint.h>

#include "arch/io.h"
#include "arch/pci.h"
#include "arch/rtl8139.h"
#include "ipc/ipc.h"
#include "net/arp.h"
#include "net/byteorder.h"
#include "net/ethernet.h"
#include "net/ipv4.h"
#include "net/rtl8139.h"
#include "user/ulib.h"

static uint16_t io_base;      /* the card's I/O port window, from BAR0 */
static uint8_t  net_irq;      /* the line it interrupts on, from the PCI config */
static uint8_t *rx_ring;      /* the DMA buffer, in this process's address space */
static uint32_t rx_offset;    /* how far into the 8 KiB ring we have read */
static uint32_t rx_phys;      /* the ring as the card sees it, kept for resets */
static uint32_t packets_seen;

/* ---- finding the card --------------------------------------------------- */

/* Walks PCI looking for the RTL8139. QEMU puts it on bus 0, but a bounded
 * scan of the low buses costs nothing and does not assume that. Fills io_base
 * and net_irq and returns true on the first match. */
static bool find_rtl8139(void)
{
    for (uint16_t bus = 0; bus < 8u; bus++) {
        for (uint8_t dev = 0; dev < 32u; dev++) {
            for (uint8_t func = 0; func < 8u; func++) {
                const uint16_t vendor = pci_read16((uint8_t)bus, dev, func, PCI_VENDOR_ID);

                if (vendor == PCI_NO_DEVICE) {
                    continue;
                }

                const uint16_t device = pci_read16((uint8_t)bus, dev, func, PCI_DEVICE_ID);

                if (vendor != RTL_VENDOR_ID || device != RTL_DEVICE_ID) {
                    continue;
                }

                /* Let the card answer its I/O BAR and master the bus -- the
                 * second is what lets its DMA engine write the RX ring at all. */
                const uint16_t command = pci_read16((uint8_t)bus, dev, func, PCI_COMMAND);

                pci_write32((uint8_t)bus, dev, func, PCI_COMMAND,
                            command | PCI_COMMAND_IO | PCI_COMMAND_BUS_MASTER);

                /* An I/O BAR carries the port base in its upper bits; the low
                 * two are type flags, not address. */
                io_base = (uint16_t)(pci_read32((uint8_t)bus, dev, func, PCI_BAR0) & 0xFFFCu);
                net_irq = pci_read8((uint8_t)bus, dev, func, PCI_INTERRUPT_LINE);

                return true;
            }
        }
    }

    return false;
}

/* ---- bringing the card up ----------------------------------------------- */

/* Resets the card and programs it from scratch, reusing the DMA buffers it
 * already has. Run once at startup, and again whenever the transmitter wedges:
 * a reset is the only thing that puts the card's own transmit pointer back at
 * descriptor 0, and the driver's must match it. It is also a reset of the
 * receive ring, so anything waiting in it is lost -- the price of a reset,
 * paid once, instead of a transmitter that never sends again.
 *
 * No allocation here, on purpose. DMA memory is pinned and never reclaimed, so
 * a reset that allocated fresh buffers would leak a page every time the card
 * stuck. */
static bool program_card(void)
{
    /* Power on, then software reset and wait for the card to clear the bit. */
    outb(io_base + RTL_CONFIG1, 0x00);
    outb(io_base + RTL_CMD, RTL_CMD_RESET);

    for (uint32_t spin = 0; (inb(io_base + RTL_CMD) & RTL_CMD_RESET) != 0; spin++) {
        if (spin > 1000000u) {
            u_print("  [net] the card never came out of reset\n");
            return false;
        }
    }

    /* The DMA engine speaks physical: hand it the frame address, not the one
     * this process sees. Then the mask (accept receive- and transmit-OK), the
     * receive configuration, and finally enable the receiver and transmitter. */
    outl(io_base + RTL_RBSTART, rx_phys);
    outw(io_base + RTL_IMR, RTL_INT_ROK | RTL_INT_TOK);
    outl(io_base + RTL_RCR, RTL_RCR_CONFIG);
    outb(io_base + RTL_CMD, RTL_CMD_RE | RTL_CMD_TE);

    /* A reset puts the card's write head back at the start of the ring. */
    rx_offset = 0;

    return true;
}

static bool init_card(void)
{
    rx_ring = (uint8_t *)(uintptr_t)u_alloc_dma(RTL_RX_RING_PAGES, &rx_phys);

    if (rx_ring == 0) {
        u_print("  [net] could not allocate the DMA receive buffer\n");
        return false;
    }

    if (!program_card()) {
        return false;
    }

    /* Now the transmit side: its own two DMA pages carved into four buffers,
     * and the card's MAC read out of the ID registers. Both belong to the
     * driver module rather than here, because both are things the card is
     * rather than things the ring does. */
    if (!rtl8139_transmit_init(io_base)) {
        return false;
    }

    /* The MAC printed through the same formatter the received frames use -- so
     * the address this machine answers to and the addresses on the wire read
     * alike. The runtime's %x drops leading zeros, which would print the very
     * common 52:54:00:... as 52:54:0:..., a MAC that is not the same address. */
    static char mac_text[18];
    char *const end = net_append_mac(mac_text, U_LIMIT(mac_text), rtl8139_mac());

    *end = '\0';

    /* Two lines: with the MAC no longer losing its zeros, one line ran past
     * the console's eightieth column and wrapped. */
    u_printf("  [net] RTL8139 at I/O 0x%x, IRQ %u, MAC %s\n", io_base, net_irq, mac_text);
    u_printf("  [net] RX ring: %u KiB at phys 0x%x, pinned\n", RTL_RX_RING_PAGES * 4u, rx_phys);

    return true;
}

/* ---- reading a frame ----------------------------------------------------- */

/* True for a frame this card should act on: sent to its own MAC, or to the
 * broadcast address. Multicast is not joined, so it is not ours either. */
static bool addressed_to_us(const uint8_t *destination)
{
    const uint8_t *const own       = rtl8139_mac();
    bool                 is_own    = true;
    bool                 broadcast = true;

    for (uint32_t i = 0; i < ETH_ALEN; i++) {
        is_own    = is_own && destination[i] == own[i];
        broadcast = broadcast && destination[i] == 0xFFu;
    }

    return is_own || broadcast;
}

/* Layer 2. The card's four-byte header is the card's, not the wire's, so the
 * Ethernet frame starts immediately after it; that is the address the header
 * struct is cast over.
 *
 * The cast is the whole trick, and it only works because ethernet_header_t is
 * packed: its members sit at consecutive byte offsets with no padding, so the
 * struct's layout and the wire's layout are the same fourteen bytes. Packed
 * also drops the struct's alignment to 1, which is what makes casting it over
 * an arbitrary offset into the ring correct rather than merely convenient --
 * the compiler is not entitled to assume the EtherType is 2-byte aligned. On
 * x86 that entitlement was worth nothing anyway: misaligned loads are a
 * hardware feature here, and the read compiles to a single movzx either way.
 *
 * The MAC addresses need no conversion: they are byte arrays, and a byte array
 * has no endianness. The EtherType is a 16-bit number and does. */
static void handle_ethernet(const uint8_t *frame, uint32_t frame_bytes)
{
    /* Static, like every line buffer in userland: a process prints from one
     * place at a time, and .bss needs no initialiser for the freestanding link
     * to resolve. */
    static char line[128];

    const ethernet_header_t *const eth = (const ethernet_header_t *)frame;
    const uint16_t ethertype = ntohs(eth->ethertype);
    const char *const protocol = net_protocol_name(ethertype);

    char *const limit = U_LIMIT(line);
    char       *out   = line;

    out = u_append(out, limit, "Ethernet Frame: ");
    out = net_append_mac(out, limit, eth->src_mac);
    out = u_append(out, limit, " -> ");
    out = net_append_mac(out, limit, eth->dst_mac);
    out = u_append(out, limit, " | Protocol: ");
    out = net_append_hex16(out, limit, ethertype);

    /* Named only when we know it. An unrecognised EtherType prints as the bare
     * number, which is the honest thing to show and also keeps the line inside
     * one 80-column row. */
    if (protocol != 0) {
        out = u_append(out, limit, " (");
        out = u_append(out, limit, protocol);
        out = u_append(out, limit, ")");
    }

    out = u_append(out, limit, "\n");
    *out = '\0';

    u_print(line);

    /* Layer 2 is done with the frame. Every frame is printed above -- that is
     * the layer-2 view, and the card is promiscuous so it sees the whole
     * segment -- but only frames actually addressed to this machine, to its own
     * MAC or to broadcast, are handed up. Anything else was addressed to some
     * other host and is not this machine's to act on, however its contents read.
     * The EtherType is already in host order here. */
    if (!addressed_to_us(eth->dst_mac)) {
        return;
    }

    if (ethertype == ETHERTYPE_ARP) {
        arp_receive(frame, frame_bytes);
    } else if (ethertype == ETHERTYPE_IPV4) {
        ipv4_receive(frame, frame_bytes);
    }
}

/* ---- receiving ---------------------------------------------------------- */

/* Steps past one record -- its 4-byte header and its frame -- rounded up to 4
 * bytes the way the card aligns them, and wrapped inside the 8 KiB ring. The
 * WRAP bit lets the card write a boundary-crossing packet contiguously into the
 * slack past the ring, so reading it here needs no split.
 *
 * CAPR trails the write head by a fixed 16 bytes; the subtraction is that bias,
 * not a mistake. It wraps below zero on purpose near the start of the ring --
 * the register is 16 bits and the card reads it modulo the ring. */
static void ring_advance(uint32_t length)
{
    rx_offset = (rx_offset + length + RTL_RX_HEADER_SIZE + 3u) & ~3u;
    rx_offset %= RTL_RX_RING_SIZE;

    outw(io_base + RTL_CAPR, (uint16_t)(rx_offset - RTL_CAPR_BIAS));
}

/* Gives up on following the ring and starts again where the card itself says it
 * is writing. CBR is the card's write head, and once our own read offset has
 * stopped landing on headers it is the one position left that is not a guess.
 *
 * It throws away whatever was queued behind the bad record. That is the honest
 * price of having lost the thread, and it is far smaller than the price of the
 * version this replaced: that one stopped the drain without moving CAPR at all,
 * which parked the read offset on the same unreadable header forever. A review
 * injected a single over-long frame and found the driver silently and
 * permanently deaf afterwards -- every later frame was valid, and not one was
 * ever seen again. That is exactly the "switch the driver off from the wire"
 * outcome the length check was added to prevent, so as written the check had
 * traded a memory fault for a quieter denial of service. */
static void ring_resync(void)
{
    const uint16_t cbr = inw(io_base + RTL_CBR);

    rx_offset = (uint32_t)cbr % RTL_RX_RING_SIZE;

    outw(io_base + RTL_CAPR, (uint16_t)(rx_offset - RTL_CAPR_BIAS));
}

/* Drains every packet the card has left in the ring since the last interrupt.
 * The card writes, ahead of each frame, a 4-byte header (status, then length
 * including the trailing CRC); BUFE in the command register tells us when the
 * ring has caught up to the read pointer.
 *
 * Every path out of this loop leaves the ring somewhere the card agrees with.
 * That is the invariant worth stating, because breaking it is not loud: a drain
 * that returns without having moved CAPR will read the same bytes forever, and
 * nothing faults, nothing logs, and no packet is ever received again.
 *
 * Returns true once the ring is empty, false if a batch ended with records
 * still waiting. A batch is a unit of work, not a verdict on the ring: the card
 * keeps writing while the drain runs -- every CAPR write frees space it fills at
 * once -- so under steady traffic one drain can legitimately see far more
 * records than the ring holds at any instant. The first version treated passing
 * that number as proof the ring had "stopped describing itself" and resynced to
 * the card's write head, discarding every valid frame still queued. A review's
 * ping flood lost exactly 75 frames -- one full ring of 108-byte records --
 * after every 1024 answered, deterministically; with the resync gone, 5000 of
 * 5000. A ring that has really lost its thread is caught by the header checks
 * below, which is where resyncing belongs. */
static bool drain_packets(void)
{
    uint32_t records = 0;

    while ((inb(io_base + RTL_CMD) & RTL_CMD_BUFE) == 0) {
        /* CAPR is already correct -- ring_advance moved it after the last
         * record -- so stopping here leaves nothing to repair. */
        if (++records > RTL_RX_BATCH_RECORDS) {
            return false;
        }

        const uint8_t *const header = rx_ring + rx_offset;
        const uint16_t       status = (uint16_t)(header[0] | ((uint16_t)header[1] << 8));
        const uint16_t       length = (uint16_t)(header[2] | ((uint16_t)header[3] << 8));

        if ((status & RTL_RX_STATUS_ROK) == 0 || length < RTL_RX_HEADER_SIZE) {
            /* Not a header we can believe: the card did not mark it received,
             * or it is too short to hold even the CRC the field counts. The
             * length is the only thing that says where the next record begins,
             * so when it cannot be trusted there is nothing to step by, and the
             * card's own write head is the only honest way back. */
            u_print("  [net] the receive ring lost sync; resyncing to the card\n");
            ring_resync();
            return true;
        }

        if (length > RTL_RX_MAX_LENGTH) {
            /* Longer than Ethernet allows. Not parsed -- reading a frame that
             * claims more than the buffer holds is how a driver walks off its
             * own mapping -- but still stepped over, because that length is the
             * card's own account of how many bytes it wrote here, and it is
             * exactly what says where the next record starts. One frame is
             * dropped and the ring stays in step, which is the difference
             * between losing a packet and losing the network. */
            u_printf("  [net] a %u-byte frame is longer than Ethernet allows; skipping it\n",
                     (uint32_t)length);
            ring_advance(length);
            continue;
        }

        /* length counts the 4-byte Ethernet CRC the card appends. */
        const uint32_t frame_bytes = (uint32_t)length - 4u;

        packets_seen++;
        u_printf("Packet Received! Size: %u bytes (frame #%u)\n", frame_bytes, packets_seen);

        /* Only now, with the length known to be inside the buffer, is there
         * something safe to cast a header over. A frame too short to hold one
         * is reported rather than parsed: the ring is still in step -- the card
         * told us how far to step -- so the drain continues. */
        if (frame_bytes >= ETH_HEADER_SIZE) {
            handle_ethernet(header + RTL_RX_HEADER_SIZE, frame_bytes);
        } else {
            u_printf("  [net] %u bytes is too short to be an Ethernet frame\n", frame_bytes);
        }

        ring_advance(length);
    }

    return true;
}

/* A transmit descriptor that never came back has left the card's pointer and
 * the driver's disagreeing, and the card transmits in order, so nothing more
 * would ever be sent. Reset the card -- which puts its pointer at 0 -- and tell
 * the transmit module to start at 0 too. Called outside the receive drain,
 * because a reset in the middle of one would pull the ring out from under it. */
static void recover_if_wedged(void)
{
    if (!rtl8139_transmit_wedged()) {
        return;
    }

    if (!program_card()) {
        u_print("  [net] the card did not come back from reset; the network is down\n");
        return;
    }

    rtl8139_transmit_reset();
    u_print("  [net] card reset: transmitting again from descriptor 0\n");
}

void _start(void)
{
    if (!u_grant_io()) {
        u_print("  [net] the kernel refused I/O access: not a driver, exiting\n");
        goto park;
    }

    if (!find_rtl8139()) {
        u_print("  [net] no RTL8139 found on the PCI bus\n");
        goto park;
    }

    if (!init_card()) {
        goto park;
    }

    /* Claim the line the card interrupts on, discovered above. Only now, with
     * the card initialised, so a stray interrupt cannot arrive before we can
     * service it. */
    if (!u_claim_irq(net_irq)) {
        u_print("  [net] the kernel refused to route the network IRQ\n");
        goto park;
    }

    u_print("  [net] up: waiting for packets\n");

    /* Say something. Until now this machine has only ever listened, and an ARP
     * request is the smallest useful thing it can say: it is one frame, it needs
     * no state, and any stack on the segment will answer it -- which makes it
     * the shortest proof that the transmit path reaches a real peer rather than
     * just filling a buffer. Under QEMU's user-mode network the gateway replies,
     * and that reply arrives back through the receive ring. */
    arp_request(NET_GATEWAY_IP);
    recover_if_wedged();

    ipc_message_t msg;

    for (;;) {
        if (u_recv(&msg) != IPC_OK) {
            continue;
        }

        /* Only the kernel forwards a hardware interrupt, and only for a line
         * this process claimed. Anything else is not ours to act on. */
        if (msg.type != MSG_HARDWARE_INTERRUPT || msg.sender_pid != IPC_KERNEL_PID) {
            continue;
        }

        if (u_load32(msg.data) != net_irq) {
            continue;
        }

        const uint16_t isr = inw(io_base + RTL_ISR);

        /* Acknowledge on the CARD before unmasking at the kernel: the
         * RTL8139's interrupt is level-driven off ISR, so unmask first and the
         * card is still asserting the same cause, which fires the line the
         * instant it reopens. That is not fatal here -- the kernel re-masks the
         * line before forwarding, so the refire self-limits to one redundant
         * forward-and-recv cycle rather than a storm (a review checked the
         * reversed order and saw no freeze) -- but acking first is still the
         * right order: it keeps the clear and the drain atomic from the
         * driver's view and spares the extra cycle. Write-1-to-clear. */
        outw(io_base + RTL_ISR, isr);

        /* A batch at a time until the ring is empty. Between batches the rest of
         * the system gets the CPU, and a transmitter that wedged during the
         * batch is reset -- between batches, never inside one, because a reset
         * clears the ring the drain is reading. Stopping after a batch and
         * waiting for the next interrupt instead would strand whatever the card
         * wrote before the ack, until some later frame happened to arrive. */
        if (isr & RTL_INT_ROK) {
            while (!drain_packets()) {
                recover_if_wedged();
                u_yield();
            }
        }

        /* And once more after the last batch, before the unmask, so the card
         * is whole again by the time its line can fire. */
        recover_if_wedged();

        if (!u_unmask_irq(net_irq)) {
            u_print("  [net] the kernel refused to unmask the network IRQ\n");
        }
    }

park:
    for (;;) {
        ipc_message_t never;
        u_recv(&never);
    }
}
