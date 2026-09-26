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

usage: inject_frames.py [--port N] [--count K] [--delay S] [--gap S]
                        [--kinds arp,ping,udp_echo,ipv4,ipv6,unknown,runt] [--listen S]
"""

import argparse
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


def arp_request():
    """who-has 10.0.2.15, tell 10.0.2.2 -- the gateway asking for THIS machine.

    The direction matters: an ARP request whose target IP is someone else is one
    the driver must ignore, so it proves nothing about the reply path. This asks
    for 10.0.2.15, which is the address net_server answers to, so a correct
    driver must transmit a reply and --listen will show it.
    """
    return (b"\x00\x01"                          # hardware type: Ethernet
            b"\x08\x00"                          # protocol type: IPv4
            b"\x06\x04"                          # hardware length 6, protocol length 4
            b"\x00\x01"                          # opcode: request
            + GATEWAY_MAC + bytes([10, 0, 2, 2])      # sender: the gateway
            + b"\x00\x00\x00\x00\x00\x00" + bytes([10, 0, 2, 15]))  # target: us, MAC unknown


def arp_request_elsewhere():
    """who-has 10.0.2.99 -- an address this machine does not own. Must be ignored."""
    return (b"\x00\x01\x08\x00\x06\x04\x00\x01"
            + GATEWAY_MAC + bytes([10, 0, 2, 2])
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
    ap.add_argument("--count", type=int, default=4)
    ap.add_argument("--delay", type=float, default=4.0,
                    help="seconds to wait for the driver to bring the card up")
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

    # The card is only listening once the driver has reset it and set RE, so
    # frames sent before that are dropped on the floor by the device model.
    time.sleep(args.delay)

    try:
        sock = socket.create_connection(("127.0.0.1", args.port), timeout=5)
    except OSError as e:
        print(f"could not reach QEMU's netdev on port {args.port}: {e}", file=sys.stderr)
        return 1

    with sock:
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
