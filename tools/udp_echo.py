#!/usr/bin/env python3
"""Send UDP to the guest's echo service over QEMU's socket netdev, and check
every reply byte by byte.

This plays the host's part the way tools/ping.py does: it builds real datagrams
from the gateway (52:55:0a:00:02:02, 10.0.2.2), writes them into the card
through a socket netdev, and reads the guest's replies off the same socket.
`make udp-host` is the other half -- the host's own netcat through QEMU's
user-mode network -- and proves delivery through a real stack; only here can a
request carry checksum 0, and only a request with checksum 0 proves the guest
computes a checksum at all. For a checksummed request the correct reply's
checksum is equal to the request's -- swapping addresses and ports only
reorders words in the sum -- so a guest that copied it would pass everything
else.

Every reply is checked against the request it answers, with checksums from
tools/inject_frames.py's RFC 768 reference -- written from the RFC, not from
udp.c. Most requests come from the gateway, but some come from another host on
the subnet with its own MAC, and one from off the subnet through the gateway's
MAC, so a guest that always answered the gateway would fail:

  Ethernet   to the requester's MAC, from the guest's, type 0x0800
  IPv4       version 4, IHL 5, TOS with ECN cleared, total length exactly 20 +
             the UDP length, flags and offset 0, TTL 64, protocol 17,
             10.0.2.15 -> the requester, header checksum valid
  UDP        ports swapped, length the request's, data byte-identical, a
             checksum that is present (never 0) and verifies
  frame      exactly one reply, the length Ethernet requires, zeroes in padding
  sequence   every IPv4 reply's identification is the previous one plus one,
             across UDP and the ICMP pings interleaved with it

Then it sends what the guest must refuse, each case built so that ONLY the
check it is named for can stop it -- a checksum that verifies apart from the
one thing wrong with it, or checksum 0 where the check under test comes before
the checksum -- and after each one proves the guest is still alive, alternately
with a UDP echo and an ICMP ping.

With --monitor it also reads the guest's console out of VGA memory through the
QEMU monitor, and checks that the brief's log line appears and that a datagram
full of control characters is shown as dots on a line of its own.

usage: tools/udp_echo.py --port N [--host H] [--wait S] [--monitor PATH]
                         [--count-file PATH]
Exit status 0 only if every case passes.
"""

import argparse
import os
import re
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject_frames as wire  # noqa: E402 -- the one definition of a test frame
import ping  # noqa: E402 -- its Wire, payload() and ICMP checks, not copies of them

GW_MAC, GW_IP, GUEST_IP = wire.GATEWAY_MAC, wire.GATEWAY_IP, wire.GUEST_IP
ETH_MIN = 60
ECHO = 7


def ip_text(raw):
    return ".".join(str(b) for b in raw)


# ---- building requests ------------------------------------------------------

def udp_request(guest_mac, sport, dport, data, *, dst_mac=None, src_mac=GW_MAC, src_ip=GW_IP,
                dst_ip=GUEST_IP, protocol=17, ip=None, beyond=b"", **udp):
    """A complete frame, wrong in at most the one thing the caller overrides.

    `udp` goes to inject_frames.udp_segment -- tail, length, checksum and the
    pseudo-header overrides -- and `ip` to ipv4_header. `beyond` is placed after
    the datagram in the frame, outside total_length. Returns the frame and the
    segment as the guest should echo it, which is the header and `data` without
    any tail."""
    segment = wire.udp_segment(src_ip, dst_ip, sport, dport, data, beyond=beyond, **udp)
    header = wire.ipv4_header(src_ip, dst_ip, protocol, len(segment), **(ip or {}))
    frame = (dst_mac or guest_mac) + src_mac + b"\x08\x00" + header + segment + beyond
    return frame, segment


def zero_sum_data(sport, n=56):
    """Data that makes a datagram from GW_IP:sport to port 7 checksum to
    0x0000 -- which RFC 768 says must be sent as 0xFFFF. The last data word is
    solved for: the one's-complement sum must come out 0xFFFF, whose complement
    is 0. The reply's checksum is the same number, so it must be 0xFFFF too."""
    length = 8 + n
    head = (GW_IP + GUEST_IP + struct.pack(">BBH", 0, 17, length)
            + struct.pack(">HHHH", sport, ECHO, length, 0))
    data = ping.payload(n - 2, salt=5) + b"\x00\x00"
    partial = ~wire.internet_checksum(head + data) & 0xFFFF   # the folded sum so far
    data = data[:-2] + struct.pack(">H", 0xFFFF - partial)
    assert wire.internet_checksum(head + data) == 0, "the solved data does not sum to -0"
    return data


def two_fold_data(sport, n=1472):
    """Data whose checksum needs a SECOND end-around fold: the 64-bit sum's low
    16 bits plus its high bits come to more than 0xFFFF, so folding once still
    leaves a carry. A net_checksum that folded only once would get it wrong by
    one; nothing else in this suite sums enough to reach the case. The last
    data word is solved for, as zero_sum_data does. The reply's sum is the same
    as the request's -- the swaps only reorder words -- so it needs two folds
    too."""
    length = 8 + n
    head = (GW_IP + GUEST_IP + struct.pack(">BBH", 0, 17, length)
            + struct.pack(">HHHH", sport, ECHO, length, 0))
    base = b"\xff" * (n - 2)
    words = head + base
    partial = sum(struct.unpack(f">{len(words) // 2}H", words))
    for last in range(65536):
        total = partial + last
        if (total & 0xFFFF) + (total >> 16) > 0xFFFF:
            return base + struct.pack(">H", last)
    raise RuntimeError("no last word needs a second fold")


# ---- checking a reply -------------------------------------------------------

def problems(reply, guest_mac, sport, dport, data, request_src_ip=GW_IP, request_src_mac=GW_MAC,
             request_tos=0):
    """Everything wrong with `reply` as the echo of `data` sent from sport to
    dport. Empty if none."""
    bad = []
    length = 8 + len(data)
    ip_len = 20 + length

    if len(reply) < 14 + ip_len:
        return [f"reply is {len(reply)} bytes, too short for a {ip_len}-byte datagram"]
    if reply[0:6] != request_src_mac:
        bad.append(f"Ethernet destination {reply[0:6].hex(':')}, not the requester")
    if reply[6:12] != guest_mac:
        bad.append(f"Ethernet source {reply[6:12].hex(':')}, not the guest's MAC")
    if reply[12:14] != b"\x08\x00":
        bad.append(f"EtherType {reply[12:14].hex()}, want 0800")

    ip = reply[14:34]
    version_ihl, tos, total, _ident, ff, ttl, proto, _ck = struct.unpack(">BBHHHBBH", ip[:12])
    if version_ihl != 0x45:
        bad.append(f"version/IHL {version_ihl:#04x}, want 0x45")
    if tos != (request_tos & 0xFC):
        bad.append(f"TOS {tos:#04x}, want the request's DSCP with ECN cleared")
    if total != ip_len:
        bad.append(f"IPv4 total length {total}, want {ip_len}")
    if ff != 0:
        bad.append(f"flags/fragment {ff:#06x}, want 0")
    if ttl != 64:
        bad.append(f"TTL {ttl}, want 64")
    if proto != 17:
        bad.append(f"protocol {proto}, want 17")
    if ip[12:16] != GUEST_IP:
        bad.append(f"IPv4 source {ip_text(ip[12:16])}, want 10.0.2.15")
    if ip[16:20] != request_src_ip:
        bad.append(f"IPv4 destination {ip_text(ip[16:20])}, want {ip_text(request_src_ip)}")
    if wire.internet_checksum(ip) != 0:
        bad.append("IPv4 header checksum does not verify")

    seg = reply[34:34 + length]
    r_sport, r_dport, r_len, r_ck = struct.unpack(">HHHH", seg[:8])
    if (r_sport, r_dport) != (dport, sport):
        bad.append(f"ports {r_sport} -> {r_dport}, want {dport} -> {sport}")
    if r_len != length:
        bad.append(f"UDP length {r_len}, want {length}")
    if seg[8:] != data:
        bad.append("data changed")
    want_ck = wire.udp_checksum(GUEST_IP, request_src_ip, seg[:6] + b"\x00\x00" + seg[8:])
    if r_ck == 0:
        bad.append("the reply carries no checksum (0)")
    elif r_ck != want_ck:
        bad.append(f"UDP checksum {r_ck:#06x}, want {want_ck:#06x}")

    want_len = max(ETH_MIN, 14 + ip_len)
    if len(reply) != want_len:
        bad.append(f"frame is {len(reply)} bytes, want {want_len}")
    if any(reply[14 + ip_len:]):
        bad.append("padding is not zero")
    return bad


# ---- the guest's console, through the QEMU monitor --------------------------

class Monitor:
    """QEMU's human monitor on a Unix socket: send a command, read until the
    next prompt. Used to press Esc (show the system console) and to read the
    80x25 text screen straight out of VGA memory at 0xB8000 -- what is on the
    screen, not what the guest believes it printed."""

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

    def screen(self):
        """The 25 rows of text on the screen, trailing spaces removed. The
        guest is paused while it is read, so no row changes half-way."""
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


def sanitised(data):
    """What udp.c's preview should show for `data`: printable ASCII kept, every
    other byte a dot, at most 40 bytes."""
    text = "".join(chr(b) if 0x20 <= b <= 0x7E else "." for b in data[:40])
    return text + ('"...' if len(data) > 40 else '"')


def find_logged(rows, sport, length, data, checksummed=True):
    """Where the brief's line for this datagram is followed by its preview."""
    brief = f"  [net] UDP Packet | Port {sport} -> 7 | Length: {length}"
    preview = ("  [net] data: \"" if checksummed else "  [net] data (no checksum): \"") + sanitised(data)
    for i in range(len(rows) - 1):
        if rows[i] == brief and rows[i + 1] == preview:
            return i
    return None


# ---- the cases --------------------------------------------------------------

class Run:
    def __init__(self, w):
        self.w = w
        self.failures = []
        self.accepted = 0
        self.next_port = 40000
        self.next_seq = 1
        self.live = 0

    def drain(self, before):
        """Reads whatever the guest sent since the last case finished. There
        should be nothing: every reply belongs to the case that asked for it and
        was read there. A frame here is a late or unasked-for reply -- one that
        arrived after a silence window closed, say -- and it fails the run
        rather than being thrown away."""
        stray = [f for f in self.w.frames(0.05) if len(f) >= 14 and f[12:14] == b"\x08\x00"]
        if stray:
            self.fail(f"nothing sent between cases (before {before})",
                      f"the guest transmitted {len(stray)} IPv4 frame(s) no case asked for")

    def fresh_port(self):
        """A source port no earlier request used, so a reply is matched to its
        request by the port it is addressed to."""
        self.next_port += 1
        return self.next_port

    def expect_echo(self, name, frame, sport, dport, data, pad=True, quiet=False, **context):
        self.drain(name)
        sent = self.w.send(frame, pad)
        want = struct.pack(">H", sport)

        def is_answer(f):
            return len(f) >= 42 and f[23] == 17 and f[36:38] == want

        seen = self.w.guest_ipv4(2.0, stop=is_answer)
        arrived = time.time()
        seen += self.w.guest_ipv4(0.25)       # a second copy would be a bug too
        answers = [f for f in seen if is_answer(f)]
        others = [f for f in seen if not is_answer(f)]

        if len(answers) != 1:
            return self.fail(name, f"{len(answers)} replies, want exactly 1")
        if others:
            return self.fail(name, f"{len(others)} other IPv4 frame(s) as well as the reply")
        bad = problems(answers[0], self.w.guest_mac, sport, dport, data, **context)
        if bad:
            return self.fail(name, "; ".join(bad))
        self.accepted += 1
        if not quiet:
            ck = struct.unpack(">H", answers[0][40:42])[0]
            print(f"  ok   {name}: {len(data)} bytes echoed from 10.0.2.15:{dport}, "
                  f"checksum {ck:#06x}, time={(arrived - sent) * 1000:.1f} ms")
        return answers[0]

    def expect_ping(self, name):
        seq = self.next_seq
        self.next_seq += 1
        ident = 0x7000 + seq
        frame, message = ping.request(self.w.guest_mac, ident, seq, ping.payload(56, salt=seq))
        self.drain(name)
        self.w.send(frame)

        def is_answer(f):
            return len(f) >= 42 and f[23] == 1 and f[34] == 0 and f[38:42] == message[4:8]

        seen = self.w.guest_ipv4(2.0, stop=is_answer) + self.w.guest_ipv4(0.25)
        answers = [f for f in seen if is_answer(f)]
        others = [f for f in seen if not is_answer(f)]
        if len(answers) != 1:
            return self.fail(name, f"{len(answers)} ping replies, want exactly 1")
        if others:
            return self.fail(name, f"{len(others)} other IPv4 frame(s) as well as the ping reply")
        bad = ping.problems(answers[0], self.w.guest_mac, message)
        return self.fail(name, "; ".join(bad)) if bad else answers[0]

    def expect_silence(self, name, frame, pad=True):
        self.drain(name)
        self.w.send(frame, pad)
        seen = self.w.guest_ipv4(1.0)
        if seen:
            return self.fail(name, f"the guest transmitted {len(seen)} IPv4 frame(s); want none")
        # Silence proves nothing on its own -- a guest that had crashed would be
        # silent too. It has to answer straight afterwards: alternately a UDP
        # echo and an ICMP ping, so ICMP and UDP replies interleave and the
        # shared identification counter is exercised across both.
        self.live += 1
        if self.live % 2:
            sport = self.fresh_port()
            data = ping.payload(24, salt=sport & 0xFF)
            live, _ = udp_request(self.w.guest_mac, sport, ECHO, data)
            ok = self.expect_echo(f"{name} (liveness)", live, sport, ECHO, data, quiet=True)
        else:
            ok = self.expect_ping(f"{name} (liveness)")
        if not ok:
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
    ap.add_argument("--monitor", help="QEMU monitor socket, to read the guest's console")
    ap.add_argument("--count-file", help="write the number of UDP replies accepted here")
    args = ap.parse_args()

    w = ping.Wire(args.host, args.port, args.wait)
    if not w.await_guest(args.wait):
        print("the guest never answered ARP for 10.0.2.15")
        return 1
    print(f"UDP to 10.0.2.15:7 over QEMU's socket netdev; guest is at {w.guest_mac.hex(':')}\n")
    w.drain(0.5)

    monitor = None
    if args.monitor:
        monitor = Monitor(args.monitor, args.wait)
        monitor.command("sendkey esc")        # show the system console, where [net] logs
        time.sleep(0.5)

    run = Run(w)
    mac = w.guest_mac

    print("datagrams that must come back, correct:")
    for size in (56, 0, 1, 2, 17, 18, 1471, 1472):
        sport = run.fresh_port()
        data = ping.payload(size, salt=size)
        frame, _ = udp_request(mac, sport, ECHO, data)
        run.expect_echo(f"{size} bytes of data", frame, sport, ECHO, data)

    sport = run.fresh_port()
    data = bytes(range(256))
    frame, _ = udp_request(mac, sport, ECHO, data)
    run.expect_echo("all 256 byte values", frame, sport, ECHO, data)

    sport = run.fresh_port()
    data = ping.payload(56, salt=9)
    frame, _ = udp_request(mac, sport, ECHO, data, checksum=0)
    run.expect_echo("request without a checksum: the guest must compute one", frame, sport, ECHO,
                    data)

    for label, checksum in [("zero-sum data sent with 0xFFFF: -0 accepted, reply 0xFFFF", None),
                            ("zero-sum data sent with no checksum: reply 0xFFFF", 0)]:
        sport = run.fresh_port()
        data = zero_sum_data(sport)
        frame, segment = udp_request(mac, sport, ECHO, data, checksum=checksum)
        if checksum is None:
            assert segment[6:8] == b"\xff\xff", "the reference did not send a computed 0 as 0xFFFF"
        answer = run.expect_echo(label, frame, sport, ECHO, data, quiet=True)
        if answer and answer[40:42] == b"\xff\xff":
            print(f"  ok   {label}: the reply carried 0xffff")
        elif answer:
            run.fail(label, f"the reply carried {answer[40:42].hex()}, not ffff")

    for label, checksum in [("UDP length 4 short of the datagram, checksummed", None),
                            ("UDP length 4 short of the datagram, no checksum", 0)]:
        sport = run.fresh_port()
        data = ping.payload(40, salt=11)
        frame, _ = udp_request(mac, sport, ECHO, data, tail=b"\x11\x22\x33\x44", checksum=checksum)
        run.expect_echo(label, frame, sport, ECHO, data)

    sport = run.fresh_port()
    frame, _ = udp_request(mac, sport, ECHO, b"")
    assert len(frame) == 42
    run.expect_echo("unpadded 42-byte frame", frame, sport, ECHO, b"", pad=False)

    sport = run.fresh_port()
    data = ping.payload(32, salt=13)
    frame, _ = udp_request(mac, sport, ECHO, data, ip={"options": b"\x01\x01\x01\x01"})
    run.expect_echo("IHL 6 with NOP options, answered with IHL 5", frame, sport, ECHO, data)

    for tos in (0xB9, 0xBA, 0xBB):              # ECN 01, 10 and 11: both bits must go
        sport = run.fresh_port()
        data = ping.payload(32, salt=tos)
        frame, _ = udp_request(mac, sport, ECHO, data, ip={"tos": tos})
        run.expect_echo(f"TOS {tos:#04x}: DSCP kept, ECN cleared", frame, sport, ECHO, data,
                        request_tos=tos)

    # The reply's TTL, flags and identification are this machine's own, never
    # the request's: a request with DF set, TTL 1 or 0 and an identification of
    # its own draws a reply with flags 0, TTL 64 and the next number in the
    # guest's sequence (checked for every frame by the Wire).
    for label, fields in [("request with DF set, TTL 1, id 0xBEEF: reply flags 0, TTL 64",
                           {"flags_fragment": 0x4000, "ttl": 1, "ident": 0xBEEF}),
                          ("request with TTL 0: answered, with TTL 64", {"ttl": 0})]:
        sport = run.fresh_port()
        data = ping.payload(24, salt=sport & 0xFF)
        frame, _ = udp_request(mac, sport, ECHO, data, ip=fields)
        run.expect_echo(label, frame, sport, ECHO, data)

    # Not only the gateway. Another host on the subnet, with a MAC of its own,
    # and a host off the subnet whose datagram arrives through the gateway's MAC:
    # each reply must go back to that host's address, at the MAC the request
    # came from, with a checksum over that host's pseudo-header.
    for label, src_ip, src_mac in [
            ("from another host on the subnet, 10.0.2.3, at its own MAC",
             bytes([10, 0, 2, 3]), b"\x02\x00\x00\x00\x00\x03"),
            ("from off the subnet, 198.51.100.7, through the gateway's MAC",
             bytes([198, 51, 100, 7]), GW_MAC)]:
        sport = run.fresh_port()
        data = ping.payload(40, salt=src_ip[-1])
        frame, _ = udp_request(mac, sport, ECHO, data, src_ip=src_ip, src_mac=src_mac)
        run.expect_echo(label, frame, sport, ECHO, data, request_src_ip=src_ip,
                        request_src_mac=src_mac)

    sport = run.fresh_port()
    data = two_fold_data(sport)
    frame, _ = udp_request(mac, sport, ECHO, data)
    run.expect_echo("1472 bytes whose checksum needs a second end-around fold", frame, sport, ECHO,
                    data)

    for label, sport in [("source port 1024, the lowest answered", 1024),
                         ("source port 65535", 65535),
                         ("source port 1025 (04 01, which is 260 read without ntohs)", 1025)]:
        data = ping.payload(24, salt=sport & 0xFF)
        frame, _ = udp_request(mac, sport, ECHO, data)
        run.expect_echo(label, frame, sport, ECHO, data)

    print("\ndatagrams that must draw no reply:")

    def case(sport=None, dport=ECHO, data=None, flip=None, **kw):
        sport = run.fresh_port() if sport is None else sport
        data = ping.payload(20, salt=sport & 0xFF) if data is None else data
        frame, _ = udp_request(mac, sport, dport, data, **kw)
        if flip is not None:
            frame = bytearray(frame)
            frame[flip] ^= 0x01
            frame = bytes(frame)
        return frame

    def zero_sum_bad():
        """A checksum field of 0xFFFF that does NOT verify: the zero-sum
        payload, checksummed (so the field is 0xFFFF), then one data bit
        flipped. Only 0 means "no checksum"; a guest that also skipped 0xFFFF
        would echo corrupted data."""
        sport = run.fresh_port()
        frame, segment = udp_request(mac, sport, ECHO, zero_sum_data(sport))
        assert segment[6:8] == b"\xff\xff"
        frame = bytearray(frame)
        frame[-1] ^= 0x01
        return bytes(frame)

    tail = b"\x11\x22\x33\x44"
    silent = [
        ("bad UDP checksum", case(flip=-1)),
        ("bad UDP checksum that is 0xFFFF, which is not 'none'", zero_sum_bad()),
        ("checksum computed without the pseudo-header", case(pseudo=False)),
        ("pseudo-header protocol 0", case(pseudo_protocol=0)),
        ("pseudo-header reserved and protocol transposed", case(pseudo_reserved=17, pseudo_protocol=0)),
        ("pseudo-header destination 0.0.0.0", case(pseudo_dst=bytes(4))),
        ("pseudo-header source 0.0.0.0", case(pseudo_src=bytes(4))),
        ("checksum over the IPv4 payload length, not the UDP length",
         case(data=ping.payload(40, salt=3), tail=tail, pseudo_length=8 + 40 + 4, cover=8 + 40 + 4)),
        ("UDP length 0, no checksum", case(length=0, checksum=0)),
        ("UDP length 7, no checksum", case(length=7, checksum=0)),
        ("UDP length 1 past the datagram, no checksum", case(length=8 + 20 + 1, checksum=0)),
        ("UDP length 2 past the datagram, no checksum", case(length=8 + 20 + 2, checksum=0)),
        ("UDP length 1 past the datagram, checksummed over the byte beyond it",
         case(length=8 + 20 + 1, beyond=b"\x5a")),
        ("UDP length 2 past the datagram, checksummed over the bytes beyond it",
         case(length=8 + 20 + 2, beyond=b"\x5a\xa5")),
        ("to port 9 (discard)", case(dport=9)),
        ("to port 6, below echo", case(dport=6)),
        ("to port 0", case(dport=0)),
        ("to port 8", case(dport=8)),
        ("to port 1792 (07 00, which is 7 read without ntohs)", case(dport=1792)),
        ("from source port 0", case(sport=0)),
        ("from source port 7: echo to echo", case(sport=7)),
        ("from source port 19: chargen", case(sport=19)),
        ("from source port 53", case(sport=53)),
        ("from source port 1023, the highest refused", case(sport=1023)),
        ("from source port 2049: NFS", case(sport=2049)),
        ("from source port 260 (01 04, which is 1025 read without ntohs)", case(sport=260)),
        ("IP protocol 6 carrying a UDP segment to port 7", case(protocol=6, checksum=0)),
        ("to port 7 via the broadcast MAC", case(dst_mac=b"\xff" * 6, checksum=0)),
        ("to port 7 at 10.0.2.255", case(dst_ip=bytes([10, 0, 2, 255]), checksum=0)),
        ("to port 7 as a fragment", case(ip={"flags_fragment": 0x2000}, checksum=0)),
        ("to port 7 from 255.255.255.255", case(src_ip=bytes([255] * 4), checksum=0)),
    ]
    for name, frame in silent:
        run.expect_silence(name, frame)

    # Seven bytes of IPv4 payload: not even room for the header. Checks 3 and 4
    # would refuse it anyway -- they need 8 -- so no reply depends on check 1.
    # What does is the console, and the case that tests check 1 is there, below:
    # an empty datagram, whose "header" would otherwise be read from the CRC.
    seven = case(checksum=0)
    total = 20 + 7
    header = wire.ipv4_header(GW_IP, GUEST_IP, 17, 7)
    frame = seven[:14] + header + seven[34:34 + 7]
    assert struct.unpack(">H", frame[16:18])[0] == total
    run.expect_silence("an IPv4 payload of 7 bytes", frame)

    if monitor:
        print("\nthe guest's console:")
        sport = run.fresh_port()
        data = b"AB\rCD\bEF\nGH\tIJ\x00KL\x1bMN\x7fOP\xffQR"
        frame, _ = udp_request(mac, sport, ECHO, data)
        if run.expect_echo("control bytes echoed byte for byte", frame, sport, ECHO, data):
            time.sleep(1.0)
            rows = monitor.screen()
            at = find_logged(rows, sport, 8 + len(data), data)
            if at is None:
                run.fail("the console shows the brief's line and the control bytes as dots",
                         "not found; the screen was:\n" + "\n".join(rows))
            elif not rows[at + 2].startswith(f"  [net] UDP echo sent to 10.0.2.2:{sport}"):
                run.fail("the console shows the brief's line and the control bytes as dots",
                         f"the line after the preview is {rows[at + 2]!r}")
            else:
                print(f"  ok   the console shows {rows[at].strip()!r}")
                print(f"  ok   and on its own line {rows[at + 1].strip()!r}")

        # Check 1. A datagram with no payload at all, in an unpadded 34-byte
        # frame: the 8 bytes where its UDP header would be are the card's CRC and
        # whatever follows it in the ring. The guest must say it is too short
        # and log nothing read from there -- no "UDP Packet" line. Located on the
        # screen by a marker echo sent just before it.
        name = "an empty datagram is refused before its header is read (console)"
        sport = run.fresh_port()
        marker = b"marker before an empty datagram"
        frame, _ = udp_request(mac, sport, ECHO, marker)
        if run.expect_echo(f"{name}: the marker", frame, sport, ECHO, marker, quiet=True):
            empty = frame[:14] + wire.ipv4_header(GW_IP, GUEST_IP, 17, 0)
            assert len(empty) == 34
            if run.expect_silence(name, empty, pad=False):
                time.sleep(1.0)
                rows = monitor.screen()
                sent = [i for i, r in enumerate(rows)
                        if r == f"  [net] UDP echo sent to 10.0.2.2:{sport}"]
                after = rows[sent[-1] + 1:sent[-1] + 4] if sent else []
                too_short = "  [net] UDP dropped: the datagram is too short to hold a UDP header"
                if not sent:
                    run.fail(name, "the marker's echo line is not on screen:\n" + "\n".join(rows))
                elif too_short not in after or any("UDP Packet" in r for r in after):
                    run.fail(name, "after the marker the console shows:\n" + "\n".join(after))
                else:
                    print(f"  ok   {name}: {too_short.strip()!r}")

    if w.id_breaks:
        run.fail("IPv4 identification", "not consecutive: " + ", ".join(
            f"{a} then {b}" for a, b in w.id_breaks[:5]))
    elif w.last_id is not None:
        print("\n  ok   every reply's IPv4 identification was the previous one plus one,"
              " across UDP and ICMP")

    if args.count_file:
        with open(args.count_file, "w") as f:
            f.write(f"{run.accepted}\n")

    print(f"\n--- 10.0.2.15 udp echo statistics ---")
    if run.failures:
        print(f"{len(run.failures)} case(s) FAILED: {', '.join(run.failures)}")
        return 1
    print(f"every case passed ({run.accepted} UDP replies checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
