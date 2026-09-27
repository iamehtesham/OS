#!/usr/bin/env python3
"""Feed raw Ethernet frames to a running QEMU's RTL8139, and read back what it sends.

`make qemu` puts the card on QEMU's user-mode network. Its gateway answers what
the guest sends, and its port forward carries UDP from the host to the echo
service, but it never sends an ARP request, a ping, or anything malformed, so it
cannot exercise most of the reply paths. `make netdemo` swaps it for a socket
netdev and runs this, which writes frames straight into the card as though they
had arrived off a wire, and with --listen decodes what the guest transmits in
return.

The card cannot tell the difference. A frame written here is DMA'd into the RX
ring, raises the same IRQ 11, and is drained by the same loop as anything a
real network would deliver.

QEMU's socket netdev wire format is a 4-byte big-endian length followed by that
many bytes of frame, in both directions.

This file is also the one definition of what a test frame is: tools/ping.py,
tools/udp_echo.py and the review harnesses import its builders and its reader
instead of keeping copies, because two copies of a frame builder drifted apart
once already.

usage: inject_frames.py --monitor PATH [--port N] [--count K] [--gap S]
                        [--kinds arp,ping,udp_echo,ipv4,ipv6,unknown,runt] [--listen S]
"""

import argparse
import re
import socket
import struct
import sys
import time


GATEWAY_MAC = b"\x52\x55\x0a\x00\x02\x02"        # what QEMU's user-mode gateway uses
GUEST_MAC = b"\x52\x54\x00\x12\x34\x56"          # the card's default address
BROADCAST = b"\xff\xff\xff\xff\xff\xff"


GUEST_IP = bytes([10, 0, 2, 15])
GATEWAY_IP = bytes([10, 0, 2, 2])


def internet_checksum(data):
    """RFC 1071, written from the RFC and not from net.c, so that using it to
    check the guest's checksums is a check and not an echo."""
    if len(data) % 2:
        data += b"\x00"
    total = sum(struct.unpack(f">{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return ~total & 0xFFFF


def ipv4_header(src, dst, protocol, payload_length, ident=1, flags_fragment=0, ttl=64,
                tos=0, options=b"", version=4, ihl=None, total_length=None):
    """An IPv4 header with a correct checksum. Every field can be overridden, so
    a test can build a header that is wrong in exactly one way."""
    if ihl is None:
        ihl = (20 + len(options)) // 4
    if total_length is None:
        total_length = 20 + len(options) + payload_length
    header = struct.pack(">BBHHHBBH4s4s", (version << 4) | (ihl & 0xF), tos, total_length,
                         ident, flags_fragment, ttl, protocol, 0, src, dst) + options
    return header[:10] + struct.pack(">H", internet_checksum(header)) + header[12:]


def icmp_message(icmp_type, code, ident, seq, data):
    """An ICMP echo-shaped message with a correct checksum."""
    body = struct.pack(">BBHHH", icmp_type, code, 0, ident, seq) + data
    return body[:2] + struct.pack(">H", internet_checksum(body)) + body[4:]


def udp_checksum(src_ip, dst_ip, covered, *, udp_length=None, protocol=17, reserved=0,
                 pseudo=True):
    """RFC 768, written from the RFC and not from udp.c: the Internet checksum
    of a 12-byte pseudo-header -- source, destination, a zero byte, protocol 17,
    the UDP length -- followed by the segment. `covered` is the segment exactly
    as the receiver will sum it, checksum field zeroed. A result of 0 is
    returned as 0xFFFF, because a transmitted 0 means "no checksum".

    Every pseudo-header input can be overridden, and pseudo=False leaves the
    pseudo-header out altogether, so a test can build a checksum that is wrong
    in exactly one way."""
    if udp_length is None:
        udp_length = len(covered)
    head = src_ip + dst_ip + struct.pack(">BBH", reserved, protocol, udp_length) if pseudo else b""
    return internet_checksum(head + covered) or 0xFFFF


def udp_segment(src_ip, dst_ip, sport, dport, data, *, tail=b"", beyond=b"", length=None,
                checksum=None, cover=None, pseudo_src=None, pseudo_dst=None, pseudo_length=None,
                pseudo_protocol=17, pseudo_reserved=0, pseudo=True):
    """A UDP header, `data`, then `tail`. Returns the bytes.

    `length` is the header's length field and defaults to 8 + len(data), so
    `tail` lies inside the IPv4 payload but outside the UDP segment. `beyond` is
    NOT returned: it names bytes the caller will place after the datagram in the
    frame, so that a length field pointing past the datagram can be given a
    checksum that verifies over what a receiver without the bound would read.

    checksum None computes it over the pseudo-header and the first `cover` bytes
    (default: `length`) of header + data + tail + beyond, with the pseudo-header
    inputs as overridden; 0 means "none"; any other number is used as it is."""
    if length is None:
        length = UDP_HEADER_SIZE + len(data)
    body = struct.pack(">HHHH", sport, dport, length & 0xFFFF, 0) + data + tail
    if checksum is None:
        covered = (body + beyond)[:length if cover is None else cover]
        checksum = udp_checksum(src_ip if pseudo_src is None else pseudo_src,
                                dst_ip if pseudo_dst is None else pseudo_dst, covered,
                                udp_length=length if pseudo_length is None else pseudo_length,
                                protocol=pseudo_protocol, reserved=pseudo_reserved, pseudo=pseudo)
    return body[:6] + struct.pack(">H", checksum) + body[8:]


UDP_HEADER_SIZE = 8


class FrameReader:
    """Reads the frames the guest transmits off QEMU's socket netdev, which
    frames outbound traffic with the same 4-byte big-endian length prefix as
    inbound. Keeps any partial frame between calls, so a caller that reads in
    short windows -- one per request -- never loses or splits a frame."""

    def __init__(self, sock):
        self.sock = sock
        self.buffered = b""

    def frames(self, seconds, stop=None):
        """Yields each frame that arrives within `seconds`. `stop`, if given, is
        called on each frame and ends the wait early when it returns True."""
        deadline = time.time() + seconds
        while True:
            while len(self.buffered) >= 4:
                size = struct.unpack(">I", self.buffered[:4])[0]
                if size > 65535 or len(self.buffered) < 4 + size:
                    break
                frame = self.buffered[4:4 + size]
                self.buffered = self.buffered[4 + size:]
                yield frame
                if stop is not None and stop(frame):
                    return
            remaining = deadline - time.time()
            if remaining <= 0:
                return
            # The timeout is for this read alone. Left on the socket it would
            # also govern the caller's next sendall, where a large burst would
            # time out part-way through a frame and desynchronise the stream.
            previous = self.sock.gettimeout()
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except (TimeoutError, socket.timeout):
                return
            except OSError:
                return
            finally:
                self.sock.settimeout(previous)
            if not chunk:
                return
            self.buffered += chunk


# ---- the QEMU monitor ---------------------------------------------------------

class Monitor:
    """QEMU's human monitor on a Unix socket: send a command, read until the
    next prompt. Every harness starts QEMU paused (-S), connects to the network
    card's socket, waits here until QEMU has accepted that connection, and only
    then lets the guest run -- QEMU's listening socket netdev throws away
    whatever the guest sends before it has a peer, and the guest's first word
    is now a DHCP DISCOVER that must not be lost. Also used to press Esc (show
    the system console) and to read the 80x25 screen out of VGA memory at
    0xB8000 -- what is on the screen, not what the guest believes it printed."""

    ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")

    def __init__(self, path, wait):
        deadline = time.time() + wait
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(path)
                break
            except OSError:
                self.sock.close()
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        self.read_prompt()

    def read_prompt(self, seconds=10.0):
        buffered = b""
        deadline = time.time() + seconds
        while not self.ANSI.sub(b"", buffered).rstrip().endswith(b"(qemu)"):
            remaining = deadline - time.time()
            if remaining <= 0:
                raise TimeoutError("the QEMU monitor stopped answering")
            self.sock.settimeout(remaining)
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("the QEMU monitor closed")
            buffered += chunk
        return self.ANSI.sub(b"", buffered).decode(errors="replace")

    def command(self, text):
        self.sock.sendall(text.encode() + b"\n")
        return self.read_prompt()

    def start_guest(self, seconds=10.0):
        """Resumes a guest started with -S, once the card's socket netdev
        reports a peer. A client's connect() returns as soon as the host kernel
        has queued it, before QEMU's accept() has run, so the connection is
        confirmed from QEMU's side first."""
        deadline = time.time() + seconds
        while "connection from" not in self.command("info network"):
            if time.time() > deadline:
                raise TimeoutError("QEMU never accepted the netdev connection")
            time.sleep(0.05)
        self.command("cont")

    def screen(self):
        """The 25 rows of text on the screen, trailing spaces removed. The
        guest is paused while it is read, so no row changes half-way -- which
        also stops the guest's clock, so never read the screen inside an
        interval being timed."""
        self.command("stop")
        try:
            dump = self.command("xp /2000hx 0xb8000")
        finally:
            self.command("cont")
        cells = []
        for line in dump.splitlines():
            m = re.match(r"^\s*0*b[89a-f][0-9a-f]{3}:\s+(.*)$", line.strip(), re.I)
            if m:
                cells += [int(w, 16) & 0xFF for w in m.group(1).split() if w.startswith("0x")]
        return ["".join(chr(c) if 32 <= c < 127 else " " for c in cells[r * 80:(r + 1) * 80]).rstrip()
                for r in range(25)]


# ---- DHCP (RFC 2131), the server's side ------------------------------------------

DHCP_COOKIE = bytes([0x63, 0x82, 0x53, 0x63])
DHCP_TYPES = {1: "DISCOVER", 2: "OFFER", 3: "REQUEST", 4: "DECLINE", 5: "ACK", 6: "NAK",
              7: "RELEASE", 8: "INFORM"}
BROADCAST_IP = bytes([255, 255, 255, 255])
MASK_24 = bytes([255, 255, 255, 0])
LEASE_INFINITE = 0xFFFFFFFF


def dhcp_parse(frame):
    """Decodes a DHCP message the guest sent, from the Ethernet header up --
    written from RFC 2131, not from dhcp.c. Returns a dict of every field; the
    options as an ordered list of (code, bytes) and as a dict of the first of
    each; what follows End. Raises ValueError for anything that is not a
    well-formed DHCP message at the framing level (wrong EtherType or protocol,
    lengths that do not fit, no cookie, an option running past the end, no
    End). Judging whether the fields are RIGHT is the caller's business."""
    if len(frame) < 14 + 20 + 8 or frame[12:14] != b"\x08\x00":
        raise ValueError("not an IPv4 frame")
    ihl = (frame[14] & 0x0F) * 4
    ip = frame[14:14 + ihl]
    total = struct.unpack(">H", ip[2:4])[0]
    if ip[9] != 17 or len(frame) < 14 + total:
        raise ValueError("not a whole UDP datagram")
    seg = frame[14 + ihl:14 + total]
    sport, dport, ulen, uck = struct.unpack(">HHHH", seg[:8])
    if ulen > len(seg) or ulen < 8:
        raise ValueError("the UDP length does not fit")
    bootp = seg[8:ulen]
    if len(bootp) < 240:
        raise ValueError(f"a {len(bootp)}-byte BOOTP message is shorter than 240")
    (op, htype, hlen, hops, xid, secs, flags, ciaddr, yiaddr, siaddr,
     giaddr) = struct.unpack(">BBBBIHH4s4s4s4s", bootp[:28])
    options, first, i, end_at = [], {}, 240, None
    while i < len(bootp):
        code = bootp[i]
        if code == 0:
            i += 1
            continue
        if code == 255:
            end_at = i
            break
        if i + 1 >= len(bootp):
            raise ValueError("an option code with no length byte")
        n = bootp[i + 1]
        if i + 2 + n > len(bootp):
            raise ValueError(f"option {code} runs past the message")
        value = bootp[i + 2:i + 2 + n]
        options.append((code, value))
        first.setdefault(code, value)
        i += 2 + n
    if end_at is None:
        raise ValueError("no End option")
    zeroed = seg[:6] + b"\x00\x00" + seg[8:ulen]
    return {
        "eth_dst": frame[0:6], "eth_src": frame[6:12],
        "ip_src": ip[12:16], "ip_dst": ip[16:20], "ip_id": struct.unpack(">H", ip[4:6])[0],
        "ip_ttl": ip[8], "ip_flags": struct.unpack(">H", ip[6:8])[0], "ihl": ihl,
        "ip_checksum_ok": internet_checksum(ip) == 0,
        "sport": sport, "dport": dport, "udp_checksum": uck,
        "udp_checksum_ok": uck != 0 and udp_checksum(ip[12:16], ip[16:20], zeroed) == uck,
        "length": len(bootp), "op": op, "htype": htype, "hlen": hlen, "hops": hops,
        "xid": xid, "secs": secs, "flags": flags, "ciaddr": ciaddr, "yiaddr": yiaddr,
        "siaddr": siaddr, "giaddr": giaddr, "chaddr": bootp[28:44], "sname": bootp[44:108],
        "file": bootp[108:236], "cookie": bootp[236:240], "options": options,
        "option": first, "type": first.get(53, b"\x00")[0] if first.get(53) else None,
        "after_end": bootp[end_at + 1:],
    }


def is_dhcp_from_guest(frame):
    """A frame from the guest's DHCP client port -- cheap enough for a filter."""
    return (len(frame) >= 42 and frame[12:14] == b"\x08\x00" and frame[23] == 17
            and frame[14 + (frame[14] & 0x0F) * 4:][:4] == b"\x00\x44\x00\x43")


def dhcp_options(message_type, *, server=GATEWAY_IP, mask=MASK_24, router=GATEWAY_IP,
                 lease=LEASE_INFINITE, extra=b"", end=True):
    """The options of a server reply, in QEMU's order: 53, 54, 1, 3, 51, End.
    Any of server, mask, router and lease may be None to leave it out."""
    out = bytes([53, 1, message_type])
    if server is not None:
        out += bytes([54, 4]) + server
    if mask is not None:
        out += bytes([1, 4]) + mask
    if router is not None:
        out += bytes([3, len(router)]) + router   # one address, or a list of them
    if lease is not None:
        out += bytes([51, 4]) + struct.pack(">I", lease)
    return out + extra + (b"\xff" if end else b"")


def dhcp_reply(xid, yiaddr, *, message_type=2, chaddr=GUEST_MAC, options=None, op=2, htype=1,
               hlen=6, flags=0, siaddr=GATEWAY_IP, cookie=DHCP_COOKIE, pad_to=548, cut_to=None,
               src_mac=GATEWAY_MAC, dst_mac=BROADCAST, src_ip=GATEWAY_IP, dst_ip=BROADCAST_IP,
               sport=67, dport=68, checksum=None, beyond=b"", **option_fields):
    """A server's reply as QEMU's user-mode network sends one: from
    52:55:0a:00:02:02 / 10.0.2.2:67 to ff:ff:ff:ff:ff:ff / 255.255.255.255:68,
    UDP checksummed, the BOOTP message zero-padded to 548 bytes. Every field can
    be overridden, `options` replaces the option bytes whole (End included),
    `cut_to` truncates the BOOTP message, and `beyond` puts bytes after the
    datagram in the frame -- so a test can build a reply wrong in exactly one
    way."""
    if options is None:
        options = dhcp_options(message_type, **option_fields)
    head = struct.pack(">BBBBIHH4s4s4s4s", op, htype, hlen, 0, xid, 0, flags, bytes(4), yiaddr,
                       siaddr, bytes(4))
    message = (head + chaddr.ljust(16, b"\x00") + bytes(64) + bytes(128) + cookie + options)
    if pad_to and len(message) < pad_to:
        message += bytes(pad_to - len(message))
    if cut_to is not None:
        message = message[:cut_to]
    segment = udp_segment(src_ip, dst_ip, sport, dport, message, checksum=checksum, beyond=beyond)
    header = ipv4_header(src_ip, dst_ip, 17, len(segment), tos=0x10)
    return dst_mac + src_mac + b"\x08\x00" + header + segment + beyond


def dhcp_nak(xid, *, server=None, chaddr=GUEST_MAC, **kw):
    """A NAK the way QEMU's DHCP server sends one: yiaddr 255.255.255.255 (it
    fills yiaddr from the broadcast destination), option 56 with a reason, and
    NO server identifier unless one is asked for here."""
    options = bytes([53, 1, 6])
    if server is not None:
        options += bytes([54, 4]) + server
    reason = b"requested address not available"
    options += bytes([56, len(reason)]) + reason + b"\xff"
    return dhcp_reply(xid, BROADCAST_IP, chaddr=chaddr, options=options, **kw)


def serve_lease(frames, send, *, address=GUEST_IP, router=GATEWAY_IP, mask=MASK_24,
                lease=LEASE_INFINITE, server=GATEWAY_IP, seconds=30.0, refused=None):
    """Plays the DHCP server once: waits for the guest's DISCOVER, OFFERs
    `address`, waits for the REQUEST, ACKs it. `frames(seconds)` yields frames
    the guest sends; `send(frame)` delivers one. Returns (discover, request) as
    dhcp_parse gives them, or raises RuntimeError. The lease is infinite by
    default, so the guest has no reason to say anything unasked afterwards.

    The OFFER and ACK are checksummed, as QEMU's are. With `refused` given, a
    guest that ignores one -- no REQUEST after the OFFER, no ARP for the router
    after the ACK -- is reported through refused("OFFER") or refused("ACK") and
    sent it again with no checksum (UDP's 0), which a guest whose checksum
    verification is broken still takes. A test suite can then report the lease
    as failed and still run every case after it, rather than dying before its
    first. Without `refused` nothing is retried: a refusal is a timeout."""
    deadline = time.time() + seconds
    patience = 2.0 if refused else seconds

    def wait_for(accept, what, within):
        until = min(deadline, time.time() + within)
        while time.time() < until:
            for f in frames(min(1.0, max(0.05, until - time.time()))):
                if accept(f):
                    return f
        if within >= seconds:
            raise RuntimeError(f"no {what} from the guest in {seconds:.0f} s")
        return None

    def dhcp(message_type, xid=None):
        def accept(f):
            if not is_dhcp_from_guest(f):
                return False
            parsed = dhcp_parse(f)
            return parsed["type"] == message_type and (xid is None or parsed["xid"] == xid)
        return accept, f"DHCP {DHCP_TYPES[message_type]}"

    def router_arp(f):
        return (len(f) >= 42 and f[12:14] == b"\x08\x06" and f[20:22] == b"\x00\x01"
                and f[38:42] == router[:4])

    def serve(reply, expect, what, answer):
        """Sends `reply`; if nothing `expect`ed comes, reports it and sends it
        again unchecksummed. Returns the frame that answered."""
        send(reply())
        got = wait_for(expect[0], expect[1], patience)
        if got is None:
            refused(answer)
            send(reply(checksum=0))
            got = wait_for(expect[0], expect[1], seconds)
        return got

    discover = dhcp_parse(wait_for(*dhcp(1), seconds))
    xid, chaddr = discover["xid"], discover["chaddr"][:6]
    terms = dict(chaddr=chaddr, server=server, mask=mask, router=router, lease=lease)

    request = dhcp_parse(serve(lambda **kw: dhcp_reply(xid, address, **terms, **kw),
                               dhcp(3, xid), "DHCP REQUEST", "OFFER"))
    ack = lambda **kw: dhcp_reply(xid, address, message_type=5, **terms, **kw)
    if refused and router is not None:
        serve(ack, (router_arp, "ARP for the router"), "ARP", "ACK")
    else:
        send(ack())
    return discover, request


def arp_request(target=GUEST_IP):
    """who-has `target` (10.0.2.15 by default), tell 10.0.2.2 -- the gateway
    asking for THIS machine.

    The direction matters: an ARP request whose target IP is someone else is one
    the driver must ignore, so it proves nothing about the reply path. This asks
    for the address the harness leased the guest, so a correct driver must
    transmit a reply and --listen will show it.
    """
    return (b"\x00\x01"                          # hardware type: Ethernet
            b"\x08\x00"                          # protocol type: IPv4
            b"\x06\x04"                          # hardware length 6, protocol length 4
            b"\x00\x01"                          # opcode: request
            + GATEWAY_MAC + GATEWAY_IP              # sender: the gateway
            + b"\x00\x00\x00\x00\x00\x00" + target)   # target: us, MAC unknown


def arp_request_elsewhere():
    """who-has 10.0.2.99 -- an address this machine does not own. Must be ignored."""
    return (b"\x00\x01\x08\x00\x06\x04\x00\x01"
            + GATEWAY_MAC + GATEWAY_IP
            + b"\x00\x00\x00\x00\x00\x00" + bytes([10, 0, 2, 99]))


def ipv4_packet():
    """A well-formed UDP datagram from the gateway to this machine, 53 -> 53.

    Genuinely well-formed now -- both lengths right and the header checksum
    correct. Its first version carried a zero IP checksum and was addressed to
    another machine's MAC, which did not matter while nothing parsed above
    layer 2. It passes every IPv4 check and reaches UDP, which accepts it -- a
    UDP checksum of 0 means the sender computed none -- and reports that no
    service listens on port 53. Nothing is sent back.
    """
    udp = b"\x00\x35\x00\x35\x00\x10\x00\x00" + b"payload!"
    return ipv4_header(GATEWAY_IP, GUEST_IP, 17, len(udp)) + udp


def ping_packet():
    """An ICMP echo request from the gateway to this machine: id 0x4242, seq 1,
    56 bytes of data -- what a default `ping` sends. Must draw an echo reply."""
    icmp = icmp_message(8, 0, 0x4242, 1, bytes(range(56)))
    return ipv4_header(GATEWAY_IP, GUEST_IP, 1, len(icmp)) + icmp


def udp_echo_packet():
    """A datagram from 10.0.2.2:40000 to the echo service on port 7, checksummed.
    Must come back unchanged, ports swapped."""
    udp = udp_segment(GATEWAY_IP, GUEST_IP, 40000, 7, b"hello, echo")
    return ipv4_header(GATEWAY_IP, GUEST_IP, 17, len(udp)) + udp


def ipv6_packet():
    """An IPv6 header with no next header, ::1 -> ::1."""
    return (b"\x60\x00\x00\x00\x00\x00\x3b\x40"  # version 6, next header 59, hop limit 64
            + b"\x00" * 15 + b"\x01" + b"\x00" * 15 + b"\x01")


# (destination MAC, source MAC, EtherType, payload, wire size). The cycle covers
# every protocol the driver names plus one it does not, so the unnamed path is
# exercised too. Destinations vary: the card is in promiscuous mode, so it takes
# a unicast frame addressed elsewhere as readily as a broadcast. The leading
# zero bytes in these addresses are deliberate -- they catch a formatter that
# drops them and turns 00:0a into 0:a, which is a different address.
# wire size None means "pad up to the 60-byte Ethernet minimum and never
# truncate"; an explicit number means "put exactly this many bytes on the wire",
# which is the only way to build a frame shorter than a header. Those two rules
# have to be stated separately: one expression that both pads and truncates
# toward a single number will silently cut a long frame in half.
FRAME_KINDS = {
    "arp": (BROADCAST, GATEWAY_MAC, 0x0806, arp_request(), None),
    "arp_other": (BROADCAST, GATEWAY_MAC, 0x0806, arp_request_elsewhere(), None),
    "ipv4": (GUEST_MAC, GATEWAY_MAC, 0x0800, ipv4_packet(), None),
    "ping": (GUEST_MAC, GATEWAY_MAC, 0x0800, ping_packet(), None),
    "udp_echo": (GUEST_MAC, GATEWAY_MAC, 0x0800, udp_echo_packet(), None),
    "ipv6": (b"\x33\x33\x00\x00\x00\x01", b"\x02\x00\x00\x00\x00\x09", 0x86DD, ipv6_packet(), None),
    "unknown": (b"\x01\x80\xc2\x00\x00\x00", b"\x00\x0a\x0b\x0c\x0d\x0e", 0x88CC,
                b"an unknown type", None),
    # Shorter than the 14-byte Ethernet header: the driver must report it and
    # keep draining, not parse past the end of it.
    "runt": (b"\xff\xff\xff\xff\xff\xff", b"\x00\x11\x22\x33\x44\x55", 0x0800, b"", 8),
}

ETH_MIN_FRAME = 60

DEFAULT_CYCLE = ["arp", "ping", "udp_echo", "ipv4", "ipv6", "unknown"]


def ethernet_frame(kind):
    dst, src, ethertype, payload, wire_size = FRAME_KINDS[kind]
    frame = dst + src + struct.pack(">H", ethertype) + payload

    if wire_size is None:
        return frame + b"\x00" * max(0, ETH_MIN_FRAME - len(frame))

    return (frame + b"\x00" * max(0, wire_size - len(frame)))[:wire_size]


def mac_text(raw):
    return ":".join(f"{b:02x}" for b in raw)


def describe(frame):
    """One line for a frame the guest transmitted, decoded from the bytes.

    Decoded here rather than trusted from the guest's own console output: the
    console says what the driver believes it sent, and this says what actually
    went out on the wire. When those two disagree, the wire is right.
    """
    if len(frame) < 14:
        return f"{len(frame)}-byte runt: {frame.hex()}"

    dst, src = mac_text(frame[0:6]), mac_text(frame[6:12])
    ethertype = struct.unpack(">H", frame[12:14])[0]
    line = f"{len(frame):4}B  {src} -> {dst}  0x{ethertype:04x}"

    if ethertype == 0x0800 and len(frame) >= 34:
        ihl = (frame[14] & 0x0F) * 4
        total = struct.unpack(">H", frame[16:18])[0]
        ip_ok = internet_checksum(frame[14:14 + ihl]) == 0
        src, dst = ".".join(map(str, frame[26:30])), ".".join(map(str, frame[30:34]))
        line += (f"\n        IPv4 {src} -> {dst} proto {frame[23]} ttl {frame[22]} len {total}"
                 f" header checksum {'ok' if ip_ok else 'BAD'}")
        if frame[23] == 1 and len(frame) >= 14 + total and total >= ihl + 8:
            icmp = frame[14 + ihl:14 + total]
            ident, seq = struct.unpack(">HH", icmp[4:8])
            icmp_ok = internet_checksum(icmp) == 0
            line += (f"\n        ICMP type {icmp[0]} code {icmp[1]} id {ident} seq {seq}"
                     f" data {len(icmp) - 8}B checksum {'ok' if icmp_ok else 'BAD'}")
        if frame[23] == 17 and len(frame) >= 14 + total and total >= ihl + 8:
            seg = frame[14 + ihl:14 + total]
            sport, dport, length, ck = struct.unpack(">HHHH", seg[:8])
            if ck == 0:
                verdict = "none"
            elif 8 <= length <= len(seg):
                zeroed = seg[:6] + b"\x00\x00" + seg[8:length]
                ok = udp_checksum(frame[26:30], frame[30:34], zeroed) == ck
                verdict = "ok" if ok else "BAD"
            else:
                verdict = "unverifiable"
            text = "".join(chr(b) if 32 <= b < 127 else "." for b in seg[8:length][:40])
            line += (f"\n        UDP {sport} -> {dport} length {length} checksum {ck:#06x} {verdict}"
                     f' data "{text}"')
        return line

    if ethertype != 0x0806 or len(frame) < 42:
        return line

    (htype, ptype, hlen, plen, op) = struct.unpack(">HHBBH", frame[14:22])
    sha, spa = mac_text(frame[22:28]), ".".join(str(b) for b in frame[28:32])
    tha, tpa = mac_text(frame[32:38]), ".".join(str(b) for b in frame[38:42])
    name = {1: "request", 2: "reply"}.get(op, f"opcode {op}")
    line += f"\n        ARP {name}: htype {htype} ptype 0x{ptype:04x} hlen {hlen} plen {plen}"
    line += f"\n        sender {sha} / {spa}    target {tha} / {tpa}"

    if op == 2:
        ok = (htype == 1 and ptype == 0x0800 and hlen == 6 and plen == 4)
        line += f"\n        well-formed: {'yes' if ok else 'NO'}"

    return line


def listen(sock, seconds):
    """Prints every frame the guest sends for `seconds`."""
    seen = 0
    for frame in FrameReader(sock).frames(seconds):
        seen += 1
        print(f"  guest sent [{seen}] {describe(frame)}")
    if seen == 0:
        print("  the guest sent nothing")
    return seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=52139)
    ap.add_argument("--monitor", required=True,
                    help="QEMU monitor socket: QEMU starts paused (-S) and is resumed from here")
    ap.add_argument("--count", type=int, default=4)
    ap.add_argument("--gap", type=float, default=0.4, help="seconds between frames")
    ap.add_argument("--kinds", default=",".join(DEFAULT_CYCLE))
    ap.add_argument("--listen", type=float, default=0.0,
                    help="after injecting, decode frames the guest transmits for this long")
    args = ap.parse_args()

    kinds = [k.strip() for k in args.kinds.split(",") if k.strip()]

    for kind in kinds:
        if kind not in FRAME_KINDS:
            print(f"unknown frame kind '{kind}'; known: {', '.join(FRAME_KINDS)}", file=sys.stderr)
            return 2

    try:
        sock = socket.create_connection(("127.0.0.1", args.port), timeout=5)
    except OSError as e:
        print(f"could not reach QEMU's netdev on port {args.port}: {e}", file=sys.stderr)
        return 1

    with sock:
        # Resume the paused guest only now that QEMU has a peer for the card,
        # then be its DHCP server: until it has an address it answers nothing.
        Monitor(args.monitor, 10.0).start_guest()
        reader = FrameReader(sock)
        try:
            serve_lease(reader.frames, lambda f: sock.sendall(struct.pack(">I", len(f)) + f))
        except RuntimeError as e:
            print(f"the guest never leased an address: {e}", file=sys.stderr)
            return 1
        print("leased 10.0.2.15 to the guest by DHCP")
        time.sleep(0.5)
        for i in range(args.count):
            kind = kinds[i % len(kinds)]
            frame = ethernet_frame(kind)
            sock.sendall(struct.pack(">I", len(frame)) + frame)
            print(f"injected {kind}: {len(frame)} bytes")
            time.sleep(args.gap)

        if args.listen > 0:
            print("listening for what the guest transmits:")
            listen(sock, args.listen)

    return 0


if __name__ == "__main__":
    sys.exit(main())
