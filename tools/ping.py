#!/usr/bin/env python3
"""Ping the guest over QEMU's socket netdev, and check every reply byte by byte.

WHY NOT THE HOST'S OWN `ping`: under `make qemu` the card sits on QEMU's
user-mode network, which is NAT -- the host cannot reach 10.0.2.15 at all, its
`hostfwd` forwards TCP and UDP but never ICMP, and a TAP device the host could
ping through needs root. So this plays the host's part instead: it builds real
ICMP echo requests from the gateway (52:55:0a:00:02:02, 10.0.2.2), delivers them
to the card through a socket netdev, and reads the guest's replies back off the
same socket. The card cannot tell the difference, and neither can the kernel.
It is not the host's `ping` binary, and nothing here pretends it is.

Every reply is checked against the request it answers, using checksums computed
by tools/inject_frames.py's RFC 1071 reference -- written from the RFC, not from
net.c -- so a check here is a check, not an echo:

  Ethernet   to the requester's MAC, from the guest's, type 0x0800
  IPv4       version 4, IHL 5, TOS with ECN cleared, total length exactly 20 +
             the ICMP message, flags and offset 0, TTL 64, protocol 1, 10.0.2.15 ->
             the requester, header checksum valid
  ICMP       type 0, code 0, identifier, sequence and data unchanged, checksum
             valid
  frame      exactly one reply per request, the length Ethernet requires, and
             zeroes in any padding
  sequence   every reply's IPv4 identification is the previous one plus one

Then it tries to make the guest answer what it must not -- bad checksums,
other addresses, broadcast and multicast sources, fragments, malformed headers,
oversized datagrams, non-echo ICMP -- and after each one proves the guest is
still alive by pinging it properly.

usage: tools/ping.py --port N [--host H] [--wait S]
Exit status 0 only if every case passes.
"""

import argparse
import os
import socket
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject_frames as wire  # noqa: E402 -- the one definition of a test frame

GW_MAC, GW_IP, GUEST_IP = wire.GATEWAY_MAC, wire.GATEWAY_IP, wire.GUEST_IP
ETH_MIN = 60


def ip_text(raw):
    return ".".join(str(b) for b in raw)


class Wire:
    """The host's end of the socket netdev."""

    def __init__(self, host, port, wait):
        deadline = time.time() + wait
        while True:
            try:
                self.sock = socket.create_connection((host, port), timeout=2)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        self.reader = wire.FrameReader(self.sock)
        self.guest_mac = None
        self.last_id = None
        self.id_breaks = []

    def note(self, frame):
        """Checks the IPv4 identification of every datagram the guest sends:
        each must be the previous one plus one. Nothing the guest transmits is
        IPv4 except replies, and every protocol's replies take their number
        from one shared counter, so a gap or a repeat means a reply was built
        some other way. Every frame read off the socket passes through here."""
        if len(frame) >= 34 and frame[12:14] == b"\x08\x00":
            ident = struct.unpack(">H", frame[18:20])[0]
            if self.last_id is not None and ident != (self.last_id + 1) & 0xFFFF:
                self.id_breaks.append((self.last_id, ident))
            self.last_id = ident
        return frame

    def frames(self, seconds, stop=None):
        return (self.note(f) for f in self.reader.frames(seconds, stop))

    def send(self, frame, pad=True):
        if pad and len(frame) < ETH_MIN:
            frame += b"\x00" * (ETH_MIN - len(frame))
        self.sock.sendall(struct.pack(">I", len(frame)) + frame)
        return time.time()

    def drain(self, seconds=0.2):
        for _ in self.frames(seconds):
            pass

    def await_guest(self, seconds):
        """Asks "who has 10.0.2.15?" until the guest answers, and learns its MAC
        from the answer. Replaces a fixed sleep: the guest is ready exactly when
        it can reply, not after some guessed number of seconds."""
        ask = wire.ethernet_frame("arp")
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.send(ask)
            for f in self.frames(0.5):
                if (len(f) >= 42 and f[12:14] == b"\x08\x06" and f[20:22] == b"\x00\x02"
                        and f[28:32] == GUEST_IP):
                    self.guest_mac = f[22:28]
                    return True
        return False

    def guest_ipv4(self, seconds, stop=None):
        """IPv4 frames the guest transmits within `seconds`; ARP chatter is not
        what is being tested here and is ignored.

        Deliberately NOT filtered on the guest's MAC. Everything read off this
        socket was transmitted by the guest, so a reply carrying some other
        source MAC is still the guest's reply -- and a forged source MAC is one
        of the defects this suite exists to catch. The first version kept only
        frames from the guest's own MAC, which made exactly those invisible."""
        return [f for f in self.frames(seconds, stop)
                if len(f) >= 14 and f[12:14] == b"\x08\x00"]


# ---- building requests ------------------------------------------------------

def payload(n, salt=0):
    """Non-zero, position-dependent data. All-zero data can make a wrong-length
    checksum verify by accident -- a run of zeroes contributes nothing to the
    sum -- which once let a malformed case pass for the wrong reason."""
    return bytes(((i * 37 + 11 + salt) & 0xFF) or 1 for i in range(n))


def request(guest_mac, ident, seq, data, *, dst_mac=None, src_mac=GW_MAC, src_ip=GW_IP,
            dst_ip=GUEST_IP, icmp_type=8, code=0, icmp=None, **ip_fields):
    """A complete frame, wrong in at most the one field the caller overrides.

    Both checksums are computed over exactly the bytes the guest will sum: the
    header checksum over IHL*4 bytes whatever IHL says, and the ICMP checksum
    over the message as total_length bounds it. So a malformed field is refused
    by the check named for it, and a guest that lacked that check would answer.
    The first version summed a fixed 20-byte header and the whole message
    regardless, so several refusal cases were really caught by a checksum
    failure, and deleting the check a case was named for left the suite green.
    Returns the frame and the ICMP message as it sits in the frame."""
    message = icmp if icmp is not None else wire.icmp_message(icmp_type, code, ident, seq, data)
    header = wire.ipv4_header(src_ip, dst_ip, 1, len(message), **ip_fields)
    datagram = bytearray(header + message)
    claimed_header = (datagram[0] & 0x0F) * 4
    claimed_total = struct.unpack(">H", datagram[2:4])[0]
    start = len(header)                       # where the message really begins

    if claimed_header == start and start + 4 <= claimed_total <= len(datagram):
        datagram[start + 2:start + 4] = b"\x00\x00"
        ck = wire.internet_checksum(bytes(datagram[start:claimed_total]))
        datagram[start + 2:start + 4] = struct.pack(">H", ck)

    if 12 <= claimed_header <= len(datagram):  # the header checksum is at 10..11
        datagram[10:12] = b"\x00\x00"
        ck = wire.internet_checksum(bytes(datagram[:claimed_header]))
        datagram[10:12] = struct.pack(">H", ck)

    frame = (dst_mac or guest_mac) + src_mac + b"\x08\x00" + bytes(datagram)
    return frame, bytes(datagram[start:])


def ihl3_request(guest_mac):
    """IHL 3: a 12-byte header, so a guest that trusted IHL would look for the
    ICMP message at byte 12 -- where the source address is. The bytes are laid
    out so that it would find a valid echo request there: type 8, code 0 and a
    correct checksum AS the source address (so 8.0.x.y, an ordinary host), then
    the destination 10.0.2.15 as identifier and sequence, then data. The 12-byte
    header checksum is correct too. Only the IHL check stands in the way."""
    body = bytearray(b"\x08\x00\x00\x00" + GUEST_IP + payload(56, salt=3))
    body[2:4] = struct.pack(">H", wire.internet_checksum(bytes(body)))
    header = bytearray(struct.pack(">BBHHHBBH", 0x43, 0, 12 + len(body), 0x3333, 0, 64, 1, 0))
    header[10:12] = struct.pack(">H", wire.internet_checksum(bytes(header)))
    return guest_mac + GW_MAC + b"\x08\x00" + bytes(header) + bytes(body)


def past_frame_request(guest_mac, ident, seq):
    """total_length two bytes longer than the frame, with the ICMP checksum
    solved so that it is correct ONLY IF those two bytes are the first two of the
    CRC the card writes after the frame (little-endian CRC-32 of the frame, which
    is what QEMU's model appends). A guest without the frame-bound check would
    therefore find a valid request and answer it -- reflecting two bytes from
    beyond the frame onto the wire. Found by search: vary the checksum field until
    the sum over message-plus-CRC comes out right."""
    data = payload(56, salt=7)
    for salt in range(64):
        body = bytearray(struct.pack(">BBHHH", 8, 0, 0, ident, seq) + data[:-1] + bytes([salt]))
        header = wire.ipv4_header(GW_IP, GUEST_IP, 1, len(body) + 2, total_length=20 + len(body) + 2)
        head = guest_mac + GW_MAC + b"\x08\x00" + header
        for ck in range(65536):
            body[2:4] = struct.pack(">H", ck)
            frame = head + bytes(body)
            crc = struct.pack("<I", zlib.crc32(frame) & 0xFFFFFFFF)
            if wire.internet_checksum(bytes(body) + crc[:2]) == 0:
                return frame
    raise RuntimeError("no checksum solved the past-the-frame case")


def zero_checksum_data(ident, seq, length):
    """Data chosen so that the REPLY's ICMP checksum comes out as 0x0000 -- a
    legal value that an implementation treating zero as "no checksum" would get
    wrong. The last data word is solved for: the reply's one's-complement sum
    must be 0xFFFF, whose complement is 0."""
    data = bytes((i * 7) & 0xFF for i in range(length - 2)) + b"\x00\x00"
    reply = struct.pack(">BBHHH", 0, 0, 0, ident, seq) + data
    partial = ~wire.internet_checksum(reply) & 0xFFFF   # the folded sum so far
    return data[:-2] + struct.pack(">H", 0xFFFF - partial)


# ---- checking a reply -------------------------------------------------------

def problems(reply, guest_mac, message, request_src_ip=GW_IP, request_src_mac=GW_MAC,
             request_tos=0):
    """Everything wrong with `reply` as the answer to `message`. Empty if none."""
    bad = []
    ip_len = 20 + len(message)

    if len(reply) < 14 + ip_len:
        return [f"reply is {len(reply)} bytes, too short for a {ip_len}-byte datagram"]
    if reply[0:6] != request_src_mac:
        bad.append(f"Ethernet destination {reply[0:6].hex(':')}, not the requester")
    if reply[6:12] != guest_mac:
        bad.append(f"Ethernet source {reply[6:12].hex(':')}, not the guest's MAC")

    ip = reply[14:34]
    version_ihl, tos, total, _ident, ff, ttl, proto, _ck = struct.unpack(">BBHHHBBH", ip[:12])
    if version_ihl != 0x45:
        bad.append(f"version/IHL {version_ihl:#04x}, want 0x45")
    if tos != (request_tos & 0xFC):
        bad.append(f"TOS {tos:#04x}, want the request's DSCP with ECN cleared")
    if total != ip_len:
        bad.append(f"IPv4 total length {total}, want {ip_len}")
    if ff != 0:
        # Not only "not a fragment": the reply's flags are this machine's own,
        # always 0, never the request's -- a request with DF set must not draw a
        # reply with DF set, which is what a copied header would carry.
        bad.append(f"flags/fragment {ff:#06x}, want 0")
    if ttl != 64:
        bad.append(f"TTL {ttl}, want 64")
    if proto != 1:
        bad.append(f"protocol {proto}, want 1")
    if ip[12:16] != GUEST_IP:
        bad.append(f"IPv4 source {ip_text(ip[12:16])}, want 10.0.2.15")
    if ip[16:20] != request_src_ip:
        bad.append(f"IPv4 destination {ip_text(ip[16:20])}, want {ip_text(request_src_ip)}")
    if wire.internet_checksum(ip) != 0:
        bad.append("IPv4 header checksum does not verify")

    icmp = reply[34:34 + len(message)]
    if icmp[0] != 0 or icmp[1] != 0:
        bad.append(f"ICMP type {icmp[0]} code {icmp[1]}, want 0/0")
    if icmp[4:8] != message[4:8]:
        bad.append("identifier or sequence changed")
    if icmp[8:] != message[8:]:
        bad.append("data changed")
    if wire.internet_checksum(icmp) != 0:
        bad.append("ICMP checksum does not verify")

    want_len = max(ETH_MIN, 14 + ip_len)
    if len(reply) != want_len:
        bad.append(f"frame is {len(reply)} bytes, want {want_len}")
    if any(reply[14 + ip_len:]):
        bad.append("padding is not zero")
    return bad


# ---- the cases --------------------------------------------------------------

class Run:
    def __init__(self, w):
        self.w = w
        self.failures = []
        self.next_seq = 1

    def fresh(self):
        seq = self.next_seq
        self.next_seq += 1
        return 0x5000 + seq, seq

    def expect_reply(self, name, frame, message, pad=True, quiet=False, **context):
        self.w.drain(0.05)
        sent = self.w.send(frame, pad)
        want = message[4:8]

        def is_answer(f):
            return len(f) >= 42 and f[23] == 1 and f[34] == 0 and f[38:42] == want

        seen = self.w.guest_ipv4(2.0, stop=is_answer)
        arrived = time.time()
        seen += self.w.guest_ipv4(0.25)       # a second copy would be a bug too
        answers = [f for f in seen if is_answer(f)]
        others = [f for f in seen if not is_answer(f)]

        if len(answers) != 1:
            return self.fail(name, f"{len(answers)} replies, want exactly 1")
        if others:
            return self.fail(name, f"{len(others)} other IPv4 frame(s) as well as the reply")
        bad = problems(answers[0], self.w.guest_mac, message, **context)
        if bad:
            return self.fail(name, "; ".join(bad))
        if not quiet:
            seq = struct.unpack(">H", message[6:8])[0]
            print(f"  ok   {name}: {len(message)} bytes from 10.0.2.15: icmp_seq={seq} ttl=64 "
                  f"time={(arrived - sent) * 1000:.1f} ms")
        return answers[0]

    def expect_silence(self, name, frame, pad=True):
        self.w.drain(0.05)
        self.w.send(frame, pad)
        seen = self.w.guest_ipv4(1.0)
        if seen:
            return self.fail(name, f"the guest transmitted {len(seen)} IPv4 frame(s); want none")
        # Silence proves nothing on its own -- a guest that had crashed would be
        # silent too. It has to answer a proper ping straight afterwards.
        ident, seq = self.fresh()
        live, message = request(self.w.guest_mac, ident, seq, payload(56, salt=seq))
        if not self.expect_reply(f"{name} (liveness)", live, message, quiet=True):
            return False
        print(f"  ok   {name}: no reply, and still answering")
        return True

    def fail(self, name, why):
        print(f"  FAIL {name}: {why}")
        self.failures.append(name)
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--wait", type=float, default=20.0, help="seconds to wait for the guest")
    args = ap.parse_args()

    w = Wire(args.host, args.port, args.wait)
    if not w.await_guest(args.wait):
        print("the guest never answered ARP for 10.0.2.15")
        return 1
    print(f"PING 10.0.2.15 over QEMU's socket netdev; guest is at {w.guest_mac.hex(':')}\n")
    w.drain(0.5)

    run = Run(w)
    mac = w.guest_mac

    print("replies that must come back, correct:")
    for size in (56, 0, 1, 55, 57, 1472):
        ident, seq = run.fresh()
        frame, message = request(mac, ident, seq, bytes((i + seq) & 0xFF for i in range(size)))
        run.expect_reply(f"{size} bytes of data", frame, message)

    ident, seq = run.fresh()
    frame, message = request(mac, ident, seq, b"")
    assert len(frame) == 42
    run.expect_reply("unpadded 42-byte frame", frame, message, pad=False)

    ident, seq = run.fresh()
    frame, message = request(mac, ident, seq, bytes(56))
    run.expect_reply("trailing bytes past the datagram", frame + b"NOT-PART-OF-IT", message)

    ident, seq = run.fresh()
    frame, message = request(mac, ident, seq, bytes(56), options=b"\x01\x01\x01\x01")
    run.expect_reply("IHL 6 with NOP options, answered with IHL 5", frame, message)

    ident, seq = run.fresh()
    frame, message = request(mac, ident, seq, zero_checksum_data(ident, seq, 56))
    answer = run.expect_reply("reply checksum is 0x0000", frame, message, quiet=True)
    if answer and answer[36:38] == b"\x00\x00":
        print("  ok   reply checksum is 0x0000: sent as 0x0000 and verified")
    elif answer:
        run.fail("reply checksum is 0x0000", f"the reply carried {answer[36:38].hex()}, not 0000")

    for label, fields, context in [
        ("don't-fragment set", {"flags_fragment": 0x4000}, {}),
        ("TTL 1", {"ttl": 1}, {}),
        ("TTL 0", {"ttl": 0}, {}),
        ("TOS 0xB9: DSCP kept, ECN cleared", {"tos": 0xB9}, {"request_tos": 0xB9}),
        ("TOS 0xBA: ECN 10 cleared too", {"tos": 0xBA}, {"request_tos": 0xBA}),
        ("TOS 0xBB: ECN 11 cleared too", {"tos": 0xBB}, {"request_tos": 0xBB}),
    ]:
        ident, seq = run.fresh()
        frame, message = request(mac, ident, seq, bytes(56), **fields)
        run.expect_reply(label, frame, message, **context)

    # Not only the gateway: every other request comes from 10.0.2.2, so a guest
    # that always replied to the gateway would pass without these. Another host
    # on the subnet with a MAC of its own, and one off the subnet whose request
    # arrives through the gateway's MAC.
    for label, src_ip, src_mac in [
            ("from another host on the subnet, 10.0.2.3, at its own MAC",
             bytes([10, 0, 2, 3]), b"\x02\x00\x00\x00\x00\x03"),
            ("from off the subnet, 198.51.100.7, through the gateway's MAC",
             bytes([198, 51, 100, 7]), GW_MAC)]:
        ident, seq = run.fresh()
        frame, message = request(mac, ident, seq, payload(56, salt=seq), src_ip=src_ip,
                                 src_mac=src_mac)
        run.expect_reply(label, frame, message, request_src_ip=src_ip, request_src_mac=src_mac)

    print("\nrequests that must draw no reply:")

    def mutated(fn, **kw):
        ident, seq = run.fresh()
        frame, _message = request(mac, ident, seq, payload(56, salt=seq), **kw)
        return fn(bytearray(frame)) if fn else frame

    def flip(offset):
        def f(b):
            b[offset] ^= 0x01
            return bytes(b)
        return f

    silent = [
        ("bad IPv4 header checksum", mutated(flip(25))),
        ("bad ICMP checksum", mutated(flip(37))),
        ("addressed to 10.0.2.16", mutated(None, dst_ip=bytes([10, 0, 2, 16]))),
        ("addressed to another host's MAC", mutated(None, dst_mac=b"\x52\x54\x00\x99\x99\x99")),
        ("sent to the broadcast MAC", mutated(None, dst_mac=b"\xff" * 6)),
        ("from 255.255.255.255", mutated(None, src_ip=bytes([255, 255, 255, 255]))),
        ("from the subnet broadcast 10.0.2.255", mutated(None, src_ip=bytes([10, 0, 2, 255]))),
        ("from multicast 224.0.0.1", mutated(None, src_ip=bytes([224, 0, 0, 1]))),
        ("from loopback 127.0.0.1", mutated(None, src_ip=bytes([127, 0, 0, 1]))),
        ("from 0.0.0.0", mutated(None, src_ip=bytes([0, 0, 0, 0]))),
        ("from 0.1.2.3, elsewhere in 0.0.0.0/8", mutated(None, src_ip=bytes([0, 1, 2, 3]))),
        ("from the subnet's own address 10.0.2.0", mutated(None, src_ip=bytes([10, 0, 2, 0]))),
        ("from 240.0.0.1", mutated(None, src_ip=bytes([240, 0, 0, 1]))),
        ("from this machine's own address", mutated(None, src_ip=GUEST_IP)),
        ("from a multicast MAC", mutated(None, src_mac=b"\x01\x00\x5e\x00\x00\x01")),
        ("a fragment: more-fragments set", mutated(None, flags_fragment=0x2000)),
        ("a fragment: non-zero offset", mutated(None, flags_fragment=0x0001)),
        ("IHL 4", mutated(None, ihl=4)),
        ("IHL 3, a valid echo request where the source address is", ihl3_request(mac)),
        ("IHL 15, longer than the datagram", mutated(None, ihl=15, total_length=28)),
        ("total length 24: a well-formed 4-byte ICMP message", mutated(None, total_length=24)),
        ("total length 2 past the frame, valid only with the card's CRC",
         past_frame_request(mac, *run.fresh())),
        ("version 6 under EtherType 0x0800", mutated(None, version=6)),
    ]
    for name, frame in silent:
        run.expect_silence(name, frame)

    # Over the 1500-byte cap. With IHL 5 the reply would not fit a frame, so a
    # guest without the cap is still stopped later, by the reply-size guard; the
    # cases WITH options are the ones only the cap can stop, because the reply
    # drops the options and fits.
    for name, total, options in [("a 1515-byte frame (datagram 1501)", 1501, b""),
                                 ("a 1518-byte frame (datagram 1504)", 1504, b""),
                                 ("datagram 1501 with IHL 6: the reply would fit", 1501, b"\x01" * 4),
                                 ("datagram 1504 with IHL 6: the reply would fit", 1504, b"\x01" * 4),
                                 ("datagram 1504 with IHL 15: the reply would fit", 1504, b"\x01" * 40)]:
        ident, seq = run.fresh()
        frame, _m = request(mac, ident, seq, payload(total - 20 - len(options) - 8, salt=seq),
                            options=options)
        run.expect_silence(name, frame)

    ident, seq = run.fresh()
    short, _m = request(mac, ident, seq, bytes(56))
    run.expect_silence("a 30-byte frame: shorter than an IPv4 header", short[:30], pad=False)

    for name, icmp_type, code in [("an inbound echo REPLY", 0, 0), ("ICMP type 13 (timestamp)", 13, 0),
                                  ("echo request with code 1", 8, 1)]:
        ident, seq = run.fresh()
        frame, _m = request(mac, ident, seq, payload(56, salt=seq), icmp_type=icmp_type, code=code)
        run.expect_silence(name, frame)

    if w.id_breaks:
        run.fail("IPv4 identification", "not consecutive: " + ", ".join(
            f"{a} then {b}" for a, b in w.id_breaks[:5]))
    elif w.last_id is not None:
        print("  ok   every reply's IPv4 identification was the previous one plus one")

    total = run.next_seq - 1
    print(f"\n--- 10.0.2.15 ping statistics ---")
    if run.failures:
        print(f"{len(run.failures)} case(s) FAILED: {', '.join(run.failures)}")
        return 1
    print(f"every case passed ({total} requests sent)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
