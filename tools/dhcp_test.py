#!/usr/bin/env python3
"""Play the DHCP server to the guest's client, and try to break it.

The guest boots with no address and broadcasts a DISCOVER. This tool is the
only server on its segment -- a socket netdev -- so it decides everything the
client sees: which offers arrive, which are malformed, which are answered,
when, and with what. It checks every message the client sends field by field
against RFC 2131 (with inject_frames.dhcp_parse, written from the RFC, not from
dhcp.c), times its retransmissions, and reads its console out of VGA memory.

Three boots:
  1. The whole life of a lease, in order: the DISCOVER and its backoff; silence
     while unbound; offers that must be refused, each broken in exactly one
     way; the REQUEST; acknowledgements that must be refused; a NAK and the
     restart; a server that never answers, and the client giving up; leases
     that end -- timed from the REQUEST, and on time under a flood -- with and
     without a mask and a router, with options padded and split, and with
     routers that must be ignored; and finally a lasting lease, and broadcasts
     it must not answer.
  2. A different lease -- 10.0.2.42/16, with 63 routers in one 252-byte option,
     10.0.2.3 first -- proving the address is the one leased and not a
     constant: answered at .42, silent at .15.
  3. Alongside the other two, the wrap kernel, whose tick counter starts 10 s
     short of 2^32: nobody answers, and the DISCOVER backoff is timed all the
     way to its 64-s cap, across the wrap.

Silence is judged by state: while SELECTING the client may only retransmit its
DISCOVER, while REQUESTING only its REQUEST, and anything else it sends -- above
all an ARP for a router, which it sends only once bound -- fails the case being
run. Intervals are timed from frame arrival, with the RFC's +-1 s jitter plus a
quarter of a second, and the screen is never read inside a timed interval
(reading it pauses the guest's clock).

Afterwards tcpdump re-decodes every frame the guest sent, from pcaps QEMU wrote.

usage: tools/dhcp_test.py --kernel K --wrap-kernel W --initrd LIST --build DIR [--wait S]
Exit status 0 only if every case passes.
"""

import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject_frames as wire  # noqa: E402 -- the one definition of a test frame
import ping  # noqa: E402
import udp_echo  # noqa: E402

GW_MAC, GW_IP, GUEST_IP = wire.GATEWAY_MAC, wire.GATEWAY_IP, wire.GUEST_IP
BROADCAST_IP = wire.BROADCAST_IP
OTHER_MAC = b"\x02\x00\x00\x00\x00\x07"
MARGIN = 0.25


def ip(*octets):
    return bytes(octets)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Boot:
    """One QEMU, paused until this end of its card's socket is connected."""

    def __init__(self, name, args, kernel=None):
        self.pcap = os.path.join(args.build, f"dhcp-{name}.pcap")
        monitor = os.path.join(args.build, f"dhcp-{name}-monitor.sock")
        for path in (self.pcap, monitor):
            if os.path.exists(path):
                os.remove(path)
        port = free_port()
        self.qemu = subprocess.Popen(
            ["qemu-system-i386", "-kernel", kernel or args.kernel, "-initrd", args.initrd,
             "-netdev", f"socket,id=net0,listen=127.0.0.1:{port}",
             "-device", "rtl8139,netdev=net0",
             "-object", f"filter-dump,id=dump0,netdev=net0,queue=rx,file={self.pcap}",
             "-display", "none", "-no-reboot", "-S",
             "-monitor", f"unix:{monitor},server=on,wait=off"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.w = ping.Wire("127.0.0.1", port, args.wait)
        self.mon = wire.Monitor(monitor, args.wait)
        self.mon.start_guest()
        self.mac = None

    def close(self):
        self.qemu.terminate()
        try:
            self.qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.qemu.kill()


class Suite:
    def __init__(self):
        self.failures = []

    def fail(self, name, why):
        print(f"  FAIL {name}: {why}")
        self.failures.append(name)
        return False

    def ok(self, name, detail=""):
        print(f"  ok   {name}{': ' + detail if detail else ''}")
        return True


class HeldSuite(Suite):
    """A Suite for a boot run on a thread of its own: its lines are kept and
    printed when the other boots are done, so the two never interleave."""

    def __init__(self):
        super().__init__()
        self.lines = []

    def fail(self, name, why):
        self.lines.append(f"  FAIL {name}: {why}")
        self.failures.append(name)
        return False

    def ok(self, name, detail=""):
        self.lines.append(f"  ok   {name}{': ' + detail if detail else ''}")
        return True


# ---- judging the client's messages -------------------------------------------

PARAMETERS = bytes([1, 3])
MAX_SIZE = struct.pack(">H", 1500)


def client_problems(m, mac, kind, *, xid=None, offer=None, server=None, discover=None):
    """Everything wrong with a DISCOVER or REQUEST, field by field."""
    bad = []

    def want(what, got, expect):
        if got != expect:
            bad.append(f"{what} {got!r}, want {expect!r}")

    want("Ethernet destination", m["eth_dst"], wire.BROADCAST)
    want("Ethernet source", m["eth_src"], mac)
    want("IP source", m["ip_src"], bytes(4))
    want("IP destination", m["ip_dst"], BROADCAST_IP)
    if not m["ip_checksum_ok"]:
        bad.append("IP header checksum does not verify")
    want("IHL", m["ihl"], 20)
    want("IP flags", m["ip_flags"], 0)
    want("TTL", m["ip_ttl"], 64)
    want("UDP ports", (m["sport"], m["dport"]), (68, 67))
    if not m["udp_checksum_ok"]:
        bad.append(f"UDP checksum {m['udp_checksum']:#06x} missing or wrong")
    want("op", m["op"], 1)
    want("htype", m["htype"], 1)
    want("hlen", m["hlen"], 6)
    want("hops", m["hops"], 0)
    want("secs", m["secs"], 0)
    want("flags", m["flags"], 0x8000)
    for field in ("ciaddr", "yiaddr", "siaddr", "giaddr"):
        want(field, m[field], bytes(4))
    want("chaddr", m["chaddr"], mac + bytes(10))
    if any(m["sname"]) or any(m["file"]):
        bad.append("sname or file is not zero")
    want("cookie", m["cookie"], wire.DHCP_COOKIE)
    if m["length"] < 300:
        bad.append(f"{m['length']} bytes, under the 300-byte BOOTP minimum")
    if any(m["after_end"]):
        bad.append("bytes after End are not zero padding")
    if kind == 1:
        want("options", m["options"], [(53, b"\x01"), (55, PARAMETERS), (57, MAX_SIZE)])
    else:
        want("options", m["options"], [(53, b"\x03"), (50, offer), (54, server), (55, PARAMETERS),
                                        (57, MAX_SIZE)])
    if xid is not None:
        want("xid", m["xid"], xid)
    if discover is not None:
        want("secs (the DISCOVER's)", m["secs"], discover["secs"])
        want("parameter list (the DISCOVER's)", m["option"].get(55), discover["option"].get(55))
    return bad


class Client:
    """The conversation with one guest: reading what it sends, timed."""

    def __init__(self, boot, suite):
        self.boot, self.w, self.s = boot, boot.w, suite
        self.mac = None
        self.offence = None   # what the client sent that failed the last quiet()

    def send(self, frame, pad=True):
        return self.w.send(frame, pad)

    def next_frames(self, seconds, stop=None):
        """(frame, arrival time) for everything the guest sends within `seconds`,
        or until `stop(frame)` is true."""
        return [(f, time.time()) for f in self.w.frames(seconds, stop)]

    def next_dhcp(self, seconds, kind=None):
        """The next DHCP message from the guest (of `kind`, if given), its
        arrival time, and every other frame that came first."""
        others = []
        deadline = time.time() + seconds
        while time.time() < deadline:
            for f in self.w.frames(min(0.5, max(0.01, deadline - time.time()))):
                arrived = time.time()
                if wire.is_dhcp_from_guest(f):
                    m = wire.dhcp_parse(f)
                    if kind is None or m["type"] == kind:
                        return m, arrived, others
                others.append(f)
        return None, None, others

    def quiet(self, name, seconds, state, xid, **context):
        """Nothing but a retransmission for the state: DISCOVERs with `xid`
        while SELECTING, REQUESTs with `xid` (and the same request) while
        REQUESTING. Each one allowed is still checked in full."""
        allowed = 1 if state == "selecting" else 3
        self.offence = None
        for f, _ in self.next_frames(seconds):
            if wire.is_dhcp_from_guest(f):
                m = wire.dhcp_parse(f)
                self.offence = m
                if m["type"] == allowed and m["xid"] == xid:
                    bad = client_problems(m, self.mac, allowed, xid=xid, **context)
                    if bad:
                        return self.s.fail(name, "a retransmission went wrong: " + "; ".join(bad))
                    self.offence = None
                    continue
                return self.s.fail(name, f"the client sent a DHCP {wire.DHCP_TYPES.get(m['type'])}"
                                         f" (xid {m['xid']:#x}) while {state}")
            self.offence = f
            kind = "an ARP" if f[12:14] == b"\x08\x06" else "an IPv4 frame"
            return self.s.fail(name, f"the client sent {kind} while {state}")
        return True

    def screen(self):
        return self.boot.mon.screen()

    def last_drop(self):
        rows = [r for r in self.screen() if r.startswith("  [net] DHCP dropped: ")]
        return rows[-1][len("  [net] DHCP dropped: "):] if rows else None

    # ---- being answered, once bound ---------------------------------------------

    def expect_ping(self, name, address, src_ip=GW_IP):
        seq = int(time.time() * 1000) & 0xFFFF
        frame, message = ping.request(self.mac, 0x6100, seq, ping.payload(40, salt=seq & 0xFF),
                                      dst_ip=address, src_ip=src_ip)
        self.send(frame)
        want = message[4:8]

        def answer(f):
            return len(f) >= 42 and f[12:14] == b"\x08\x00" and f[23] == 1 and f[38:42] == want

        for f, _ in self.next_frames(2.0, answer):
            if answer(f):
                bad = ping.problems(f, self.mac, message, request_src_ip=src_ip, guest_ip=address)
                return self.s.fail(name, "; ".join(bad)) if bad else self.s.ok(name)
        return self.s.fail(name, "no ping reply")

    def expect_echo(self, name, address):
        sport = 40000 + (int(time.time() * 1000) % 20000)
        data = ping.payload(24, salt=sport & 0xFF)
        frame, _ = udp_echo.udp_request(self.mac, sport, 7, data, dst_ip=address)
        self.send(frame)
        want = struct.pack(">H", sport)

        def answer(f):
            return len(f) >= 42 and f[12:14] == b"\x08\x00" and f[23] == 17 and f[36:38] == want

        for f, _ in self.next_frames(2.0, answer):
            if answer(f):
                bad = udp_echo.problems(f, self.mac, sport, 7, data, guest_ip=address)
                return self.s.fail(name, "; ".join(bad)) if bad else self.s.ok(name)
        return self.s.fail(name, "no UDP echo")

    def expect_arp_reply(self, name, address):
        self.send(wire.BROADCAST + GW_MAC + b"\x08\x06" + wire.arp_request(address))

        def answer(f):
            return f[12:14] == b"\x08\x06" and f[20:22] == b"\x00\x02" and f[28:32] == address

        for f, _ in self.next_frames(1.5, answer):
            if answer(f):
                return self.s.ok(name)
        return self.s.fail(name, "no ARP reply")

    def expect_nothing(self, name, frame, seconds=1.0, pad=True):
        self.send(frame, pad)
        seen = [f for f, _ in self.next_frames(seconds)
                if not wire.is_dhcp_from_guest(f)]
        if seen:
            return self.s.fail(name, f"the guest answered with {len(seen)} frame(s)")
        return self.s.ok(name, "nothing")


def arp_who_has(target):
    return wire.BROADCAST + GW_MAC + b"\x08\x06" + wire.arp_request(target)


def router_arp(frames, router):
    """The ARP request the client sends for its router once bound."""
    for f in frames:
        if f[12:14] == b"\x08\x06" and f[20:22] == b"\x00\x01" and f[38:42] == router:
            return f
    return None


def interval_ok(s, name, gap, nominal):
    return window_ok(s, name, gap, nominal - 1, nominal + 1)


def window_ok(s, name, gap, low, high):
    low, high = low - MARGIN, high + MARGIN
    if low <= gap <= high:
        return s.ok(name, f"{gap:.2f} s")
    return s.fail(name, f"{gap:.2f} s, want {low:.2f}-{high:.2f}")


def arp_who_has_from(sender_mac, target):
    """who-has `target`, tell 10.0.2.2 -- with `sender_mac` in the ARP sender
    field, whatever the Ethernet source says."""
    return (wire.BROADCAST + GW_MAC + b"\x08\x06" + b"\x00\x01\x08\x00\x06\x04\x00\x01"
            + sender_mac + GW_IP + bytes(6) + target)


# ---- boot 1: the life of a lease ------------------------------------------------

def boot_one(args, s):
    boot = Boot("1", args)
    c = Client(boot, s)
    try:
        print("the DISCOVER, and its retransmissions:")
        d1, t1, _ = c.next_dhcp(20, kind=1)
        if d1 is None:
            s.fail("the first DISCOVER", "none arrived")
            return
        c.mac = d1["eth_src"]
        xid = d1["xid"]
        # The servers are up once the network server speaks: show the system
        # console, where [net] logs, for every console check to come. Pressing
        # a key does not stop the guest's clock, so this is safe here.
        boot.mon.command("sendkey esc")
        bad = client_problems(d1, c.mac, 1)
        if d1["ip_id"] != 0:
            bad.append(f"IP id {d1['ip_id']}, want 0 -- the first datagram from the shared counter")
        if bad:
            s.fail("the first DISCOVER", "; ".join(bad))
        else:
            s.ok("the first DISCOVER", f"every field right, xid {xid:#010x}")

        d2, t2, others = c.next_dhcp(9, kind=1)
        if d2 is None or others:
            s.fail("DISCOVER retransmitted after 4 s +-1", "no retransmission" if d2 is None
                   else f"{len(others)} other frame(s) first")
        else:
            bad = client_problems(d2, c.mac, 1, xid=xid)
            if bad:
                s.fail("DISCOVER retransmitted after 4 s +-1", "; ".join(bad))
            else:
                interval_ok(s, "DISCOVER retransmitted after 4 s +-1, same xid", t2 - t1, 4)
        d3, t3, others = c.next_dhcp(12, kind=1)
        if d3 is None or others:
            s.fail("second retransmission after 8 s +-1", "none" if d3 is None else "other frames")
        elif client_problems(d3, c.mac, 1, xid=xid):
            s.fail("second retransmission after 8 s +-1", "; ".join(client_problems(d3, c.mac, 1, xid=xid)))
        else:
            interval_ok(s, "second retransmission after 8 s +-1 (the backoff doubles)", t3 - t2, 8)

        print("\nunbound: nothing answered, not even at 0.0.0.0:")
        cases = [
            ("unbound: ping to 10.0.2.15", ping.request(c.mac, 0x6200, 1, ping.payload(40))[0]),
            ("unbound: ping to 0.0.0.0 at our MAC",
             ping.request(c.mac, 0x6200, 2, ping.payload(40), dst_ip=bytes(4))[0]),
            ("unbound: UDP echo to 10.0.2.15", udp_echo.udp_request(c.mac, 41000, 7, b"x" * 20)[0]),
            ("unbound: UDP echo to 0.0.0.0 at our MAC",
             udp_echo.udp_request(c.mac, 41001, 7, b"x" * 20, dst_ip=bytes(4))[0]),
            ("unbound: ARP who-has 10.0.2.15", arp_who_has(GUEST_IP)),
            ("unbound: ARP who-has 0.0.0.0", arp_who_has(bytes(4))),
        ]
        for name, frame in cases:
            c.send(frame)
            if c.quiet(name, 1.0, "selecting", xid):
                s.ok(name, "nothing")

        print("\nOFFERs the client must refuse -- no REQUEST may follow:")
        S, M, R = GW_IP, wire.MASK_24, GW_IP

        def offer(**kw):
            fields = dict(message_type=2)
            fields.update(kw)
            address = fields.pop("yiaddr", GUEST_IP)
            return wire.dhcp_reply(fields.pop("xid", xid), address, **fields)

        def opts(*parts):
            return b"".join(parts)

        def restart(state):
            """After a case the client wrongly acted on, starts it afresh in
            `state` -- a new transaction, SELECTING, and for "requesting" an
            OFFER taken -- so the cases after it test what they are named for,
            not a client already somewhere else. A client that took an OFFER is
            NAKed; one that restarted has said so with its new DISCOVER; one
            that took an ACK is bound for that ACK's 2-s lease and restarts when
            it runs out. False, with the case failed, if it does not."""
            nonlocal xid, first_request_at
            fresh = c.offence if isinstance(c.offence, dict) else None
            if fresh is None or fresh["type"] != 1 or fresh["xid"] == xid:
                c.send(wire.dhcp_nak(xid))
                fresh, deadline = None, time.time() + 8
                while fresh is None and time.time() < deadline:
                    m, _, _ = c.next_dhcp(deadline - time.time(), kind=1)
                    if m is None:
                        break
                    if m["xid"] != xid:
                        fresh = m
            if fresh is None:
                return s.fail("(restarting the client after a failed case)",
                              "no new DISCOVER; the cases after this one cannot be run")
            xid = fresh["xid"]
            if state == "requesting":
                c.send(offer())
                r, first_request_at, _ = c.next_dhcp(3, kind=3)
                if r is None or r["xid"] != xid:
                    return s.fail("(restarting the client after a failed case)", "no REQUEST")
            print(f"  ..   restarted the client: xid {xid:#010x}, {state}")
            return True

        def moved():
            """Did the client leave SELECTING -- REQUEST an offer, or start a
            new transaction? Anything else it sent changed nothing."""
            m = c.offence
            return isinstance(m, dict) and (m["type"] == 3 or m["xid"] != xid)

        base = [bytes([53, 1, 2]), bytes([54, 4]) + S]
        tail = [bytes([3, 4]) + R, bytes([51, 4]) + struct.pack(">I", 3600), b"\xff"]
        # Built afresh for every case, because a restart changes the xid.
        refusals = lambda: [
            ("OFFER with the wrong xid", offer(xid=xid ^ 1)),
            ("OFFER for another hardware address", offer(chaddr=OTHER_MAC)),
            ("OFFER with op 1 (a BOOTREQUEST)", offer(op=1)),
            ("OFFER with htype 6", offer(htype=6)),
            ("OFFER with hlen 16", offer(hlen=16)),
            ("OFFER from UDP source port 1067", offer(sport=1067)),
            ("OFFER with a bad UDP checksum", offer(checksum=0x1234)),
            ("OFFER with the cookie byte-swapped (63 53 82 63)",
             offer(cookie=bytes([0x63, 0x53, 0x82, 0x63]))),
            ("an ACK while SELECTING", offer(message_type=5)),
            ("OFFER whose options have no End (zero padding follows)",
             offer(options=wire.dhcp_options(2, end=False))),
            ("OFFER with option overload (52)", offer(options=wire.dhcp_options(2, extra=bytes([52, 1, 3])))),
            ("OFFER with option 53 twice", offer(options=wire.dhcp_options(2, extra=bytes([53, 1, 2])))),
            ("OFFER with a 3-byte subnet mask",
             offer(options=opts(*base, bytes([1, 3, 255, 255, 255]), *tail))),
            ("OFFER with a 3-byte lease time",
             offer(options=opts(*base, bytes([1, 4]) + M, bytes([3, 4]) + R,
                                bytes([51, 3, 0, 14, 16]), b"\xff"))),
            ("OFFER with a 3-byte server identifier",
             offer(options=opts(bytes([53, 1, 2]), bytes([54, 3, 10, 0, 2]), bytes([1, 4]) + M,
                                *tail))),
            ("OFFER whose server identifier is 0.0.0.0", offer(server=bytes(4))),
            ("OFFER with a 6-byte router option",
             offer(options=opts(*base, bytes([1, 4]) + M, bytes([3, 6]) + R + b"\x00\x00",
                                bytes([51, 4]) + struct.pack(">I", 3600), b"\xff"))),
            ("OFFER of 0.1.2.3 (0.0.0.0/8)", offer(yiaddr=ip(0, 1, 2, 3))),
            ("OFFER of 127.0.0.1 (loopback)", offer(yiaddr=ip(127, 0, 0, 1))),
            ("OFFER of 224.0.0.1 (multicast)", offer(yiaddr=ip(224, 0, 0, 1))),
            ("OFFER of 240.0.0.1 (240/4)", offer(yiaddr=ip(240, 0, 0, 1))),
            ("OFFER of 10.0.2.0, the /24's network address", offer(yiaddr=ip(10, 0, 2, 0))),
            ("OFFER of 10.0.2.255, the /24's broadcast address", offer(yiaddr=ip(10, 0, 2, 255))),
            ("OFFER with mask 255.0.255.0 (not contiguous)", offer(mask=ip(255, 0, 255, 0))),
            ("OFFER with mask 254.0.0.0 (/7)", offer(mask=ip(254, 0, 0, 0))),
            ("OFFER with mask /31", offer(mask=ip(255, 255, 255, 254))),
            ("OFFER with mask /32", offer(mask=ip(255, 255, 255, 255))),
            # With the current xid: only the state check can refuse it, and a
            # NAK taken shows only when the client starts again, 1-2 s on.
            ("a NAK while SELECTING", wire.dhcp_nak(xid)),
        ]
        for i in range(len(refusals())):
            name, frame = refusals()[i]
            c.send(frame)
            if c.quiet(name, 2.5 if "NAK" in name else 1.0, "selecting", xid):
                s.ok(name, "refused")
            elif moved() and not restart("selecting"):
                return

        print("\nOFFERs refused for a reason the console shows (the wire cannot tell):")
        overrun = opts(*base, bytes([1, 4]) + M, bytes([12, 200]) + b"host")
        no_length = opts(*base, bytes([1, 4]) + M, bytes([12]))
        console_cases = lambda: [
            ("an option running past the message (console)",
             offer(options=overrun, pad_to=0), "an option runs past the end of the message"),
            ("an option code with no length byte (console)",
             offer(options=no_length, pad_to=0), "an option code with no length byte"),
            ("a message shorter than 240 bytes (console)", offer(cut_to=239),
             "too short to be a DHCP message"),
            # A missing identifier reads as 0.0.0.0, which the host-address
            # check refuses too: only the reason tells the two checks apart.
            ("OFFER without a server identifier (54) (console)", offer(server=None),
             "an OFFER without a server identifier (option 54)"),
            # 255 bytes where 8 are kept: refused for its length, after the walk
            # has gathered it -- and a server that overran the buffer on the way
            # would never get as far as saying so.
            ("OFFER whose server identifier is 255 bytes (console)",
             offer(options=opts(bytes([53, 1, 2]), bytes([54, 255]) + bytes(range(255)),
                                bytes([1, 4]) + M, *tail)),
             "the server identifier option is not 4 bytes"),
        ]
        for i in range(len(console_cases())):
            name, frame, reason = console_cases()[i]
            c.send(frame)
            if not c.quiet(name, 1.0, "selecting", xid):
                if moved() and not restart("selecting"):
                    return
                continue
            got = c.last_drop()
            if got == reason:
                s.ok(name, f"dropped: {reason}")
            else:
                s.fail(name, f"the console's last drop reason is {got!r}, want {reason!r}")

        print("\nthe REQUEST:")
        c.send(offer())
        r1, tr1, others = c.next_dhcp(3, kind=3)
        first_request_at = tr1
        if r1 is None:
            s.fail("the REQUEST", "none arrived after a valid OFFER")
            return
        bad = client_problems(r1, c.mac, 3, xid=xid, offer=GUEST_IP, server=S, discover=d1)
        if others:
            bad.append(f"{len(others)} other frame(s) came first")
        if bad:
            s.fail("the REQUEST", "; ".join(bad))
        else:
            s.ok("the REQUEST", "every field right: same xid and secs, 50 = the offer, 54 = the server")
        r2, tr2, others = c.next_dhcp(7, kind=3)
        if r2 is None or r2["xid"] != xid:
            s.fail("REQUEST retransmitted after 4 s +-1", "none")
        else:
            interval_ok(s, "REQUEST retransmitted after 4 s +-1, same xid", tr2 - tr1, 4)

        print("\nwhile REQUESTING, refused -- no lease, no restart:")
        ctx = dict(offer=GUEST_IP, server=S, discover=d1)

        def ack(**kw):
            # A lease that has 3 s left: the client counts it from the first
            # REQUEST of the transaction, so it is that long plus 3 s. Long
            # enough not to be refused as already over -- which would stop the
            # ACK before the check its case is named for -- and short enough
            # that a client that wrongly takes one is soon unbound again.
            fields = dict(message_type=5, lease=int(time.time() - first_request_at) + 3)
            fields.update(kw)
            return offer(**fields)

        requesting = lambda: [
            ("a second OFFER while REQUESTING (of 10.0.2.16)", offer(yiaddr=ip(10, 0, 2, 16))),
            ("an ACK with the wrong xid", ack(xid=xid ^ 1)),
            ("an ACK for another hardware address", ack(chaddr=OTHER_MAC)),
            ("an ACK from another server (54 = 10.0.2.99)", ack(server=ip(10, 0, 2, 99))),
            ("an ACK for another address (10.0.2.16)", ack(yiaddr=ip(10, 0, 2, 16))),
            ("an ACK whose /28 makes 10.0.2.15 a broadcast address",
             ack(mask=ip(255, 255, 255, 240))),
            ("an ACK with mask 255.0.255.0 (not contiguous)", ack(mask=ip(255, 0, 255, 0))),
            ("an ACK with mask 254.0.0.0 (/7)", ack(mask=ip(254, 0, 0, 0))),
            ("an ACK whose lease, 0 s, has already run out", ack(lease=0)),
            ("a NAK from another server (54 = 10.0.2.99)", wire.dhcp_nak(xid, server=ip(10, 0, 2, 99))),
        ]
        for i in range(len(requesting())):
            name, frame = requesting()[i]
            c.send(frame)
            # A NAK taken shows only when the client starts again, 1-2 s on.
            window = 2.5 if "NAK" in name else 1.0
            if c.quiet(name, window, "requesting", xid, **ctx):
                s.ok(name, "refused")
            elif not restart("requesting"):
                return

        print("\na NAK, the way QEMU sends one:")
        sent = time.time()
        for _ in range(5):
            c.send(wire.dhcp_nak(xid))
        d4, t4, others = c.next_dhcp(4, kind=1)
        if d4 is None:
            s.fail("after a NAK, a new DISCOVER in 1-2 s", "none")
            return
        bad = client_problems(d4, c.mac, 1)
        if d4["xid"] == xid:
            bad.append("the xid did not change")
        if others:
            bad.append(f"{len(others)} other frame(s) first")
        if bad:
            s.fail("after a NAK, a new DISCOVER in 1-2 s", "; ".join(bad))
        else:
            gap = t4 - sent
            if 1 - MARGIN - 0.1 <= gap <= 2 + MARGIN + 0.3:
                s.ok("after a NAK, a new DISCOVER in 1-2 s", f"{gap:.2f} s, new xid, every field right")
            else:
                s.fail("after a NAK, a new DISCOVER in 1-2 s", f"{gap:.2f} s")
        xid = d4["xid"]
        extra, _, _ = c.next_dhcp(2.4, kind=1)
        if extra is not None and extra["xid"] != xid:
            s.fail("five NAKs make one DISCOVER", "a second new transaction started")
        else:
            s.ok("five NAKs make one DISCOVER")

        # SELECTING again, but the NAKed offer -- 10.0.2.15 from 10.0.2.2 -- is
        # still what the client last requested, so an ACK for it with the new
        # xid passes every check but the one that it is not waiting for an ACK.
        name = "an ACK while SELECTING, for the offer a NAK ended"
        c.send(ack())
        if c.quiet(name, 1.0, "selecting", xid):
            s.ok(name, "refused")
        elif not restart("selecting"):
            return

        print("\na server that never answers the REQUEST:")
        c.send(offer())
        r, t_prev, _ = c.next_dhcp(3, kind=3)
        if r is None:
            s.fail("REQUEST after the OFFER", "none")
            return
        for k, nominal in enumerate((4, 8, 16, 32), start=1):
            r, t, others = c.next_dhcp(nominal + 3, kind=3)
            name = f"REQUEST retransmission {k} after {nominal} s +-1"
            if r is None or r["xid"] != xid:
                s.fail(name, "none")
                return
            bad = client_problems(r, c.mac, 3, xid=xid, offer=GUEST_IP, server=S, discover=d4)
            if bad:
                s.fail(name, "; ".join(bad))
            else:
                interval_ok(s, name, t - t_prev, nominal)
            t_prev = t
        d5, t5, _ = c.next_dhcp(10, kind=1)
        name = ("gives up 4 s +-1 after the fourth retransmission, and 1-2 s later: "
                "new DISCOVER, new xid")
        if d5 is None or d5["xid"] == xid:
            s.fail(name, "no new transaction")
            return
        window_ok(s, name, t5 - t_prev, 4 - 1 + 1, 4 + 1 + 2)
        xid = d5["xid"]

        requested_at = None

        def lease(address=GUEST_IP, hold=0.0, ack_options=None, patience=3.0, **terms):
            """OFFER, check the REQUEST, ACK -- `hold` seconds after the REQUEST
            came. Returns the frames after the ACK; the REQUEST's arrival is
            left in requested_at. `patience` is how long the REQUEST may take."""
            nonlocal xid, requested_at
            c.send(offer(yiaddr=address, **terms))
            req, requested_at, _ = c.next_dhcp(patience, kind=3)
            if req is None or req["xid"] != xid:
                return None
            if hold:
                time.sleep(hold)
            if ack_options is not None:
                c.send(offer(yiaddr=address, options=ack_options))
            else:
                c.send(offer(yiaddr=address, message_type=5, **terms))
            return [f for f, _ in c.next_frames(1.0)]

        def expires(name, seconds):
            """The lease runs out: a new transaction, and silence at the old
            address. Not timed -- these leases' checks read the screen, which
            pauses the guest's clock; the timed lease below reads none."""
            nonlocal xid
            d, _, _ = c.next_dhcp(seconds + 6, kind=1)
            if d is None or d["xid"] == xid:
                return s.fail(name, "no new DISCOVER after the lease ended")
            xid = d["xid"]
            c.send(ping.request(c.mac, 0x6300, 1, ping.payload(40))[0])
            if not c.quiet(name + ": then silent at 10.0.2.15", 1.0, "selecting", xid):
                return False
            return s.ok(name, "address dropped, a new DISCOVER")

        def timed_lease(seconds, hold=0.0):
            """A lease whose end is timed from the REQUEST, which is when RFC
            2131 4.4.1 says it began: answering until then, silent after, and a
            new DISCOVER 1-2 s later. The ACK is held back `hold` seconds, so a
            lease counted from the ACK would end that much too late."""
            nonlocal xid
            after = lease(lease=seconds, hold=hold)
            if after is None:
                return s.fail(f"the timed {seconds} s lease", "no REQUEST")
            end = requested_at + seconds
            time.sleep(max(0.0, end - 0.8 - time.time()))
            c.expect_ping("timed lease: still answering 0.8 s before its end", GUEST_IP)
            time.sleep(max(0.0, end + 0.3 - time.time()))
            c.send(ping.request(c.mac, 0x6600, 1, ping.payload(40))[0])
            name = "timed lease: silent 0.3 s after its end"
            replies = [f for f, _ in c.next_frames(0.4) if f[12:14] == b"\x08\x00" and f[23] == 1]
            if replies:
                s.fail(name, "the ping was answered: the lease is still in use")
            else:
                s.ok(name)
            d, t, _ = c.next_dhcp(end + 4 - time.time(), kind=1)
            name = "timed lease: a new DISCOVER 1-2 s after its end"
            if d is None or d["xid"] == xid:
                return s.fail(name, "none")
            xid = d["xid"]
            return window_ok(s, name, t - requested_at, seconds + 1, seconds + 2)

        def flooded_lease(seconds):
            """A lease that ends while its owner's receive ring is never empty,
            so the receive loop never runs dry and never returns to recv: the
            end must still come on time. The flood is ARP requests for the
            leased address, 150 of them unanswered at any moment -- more than
            the ring holds, so it stays full, however fast or slow the guest
            is running, and no more than it can clear in a second or two once
            it stops answering. It stops answering exactly when the lease ends,
            which is the other thing timed."""
            nonlocal xid
            after = lease(lease=seconds)
            if after is None:
                return s.fail(f"the flooded {seconds} s lease", "no REQUEST")
            ask = arp_who_has(GUEST_IP)
            end, stop = requested_at + seconds, requested_at + seconds + 5
            found, sent, answered, last_answer = None, 0, 0, None
            while found is None and time.time() < stop:
                while sent - answered < 150:
                    c.send(ask)
                    sent += 1
                for f, t in c.next_frames(0.01):
                    if f[12:14] == b"\x08\x06" and f[20:22] == b"\x00\x02" and f[28:32] == GUEST_IP:
                        answered, last_answer = answered + 1, t
                    elif wire.is_dhcp_from_guest(f):
                        m = wire.dhcp_parse(f)
                        if m["type"] == 1 and m["xid"] != xid:
                            found = (m, t)
            name = "flooded lease: answering until its end, silent from then on"
            if not answered:
                s.fail(name, "no ARP request of the flood was ever answered")
            elif last_answer > end + 0.3:
                s.fail(name, f"still answering {last_answer - end:.2f} s after the lease ended "
                             f"({answered} of {sent} answered)")
            else:
                s.ok(name, f"{answered} of {sent} answered, the last {last_answer - end:+.2f} s "
                           f"from the end")
            name = "flooded lease: a new DISCOVER 1-2 s after its end, flood or not"
            if found is None:
                return s.fail(name, f"none while the flood lasted ({sent} frames sent)")
            xid = found[0]["xid"]
            return window_ok(s, name, found[1] - requested_at, seconds + 1, seconds + 2)

        def split_options(kind, lease_seconds):
            """53, then Pads, and the server identifier, mask, router and lease
            each sent as two instances of two bytes, which RFC 3396 says are
            one option: their bytes concatenated."""
            L = struct.pack(">I", lease_seconds)
            return opts(bytes([53, 1, kind, 0, 0]), bytes([54, 2]) + S[:2], bytes([0]),
                        bytes([1, 2]) + M[:2], bytes([54, 2]) + S[2:], bytes([3, 2]) + R[:2],
                        bytes([1, 2]) + M[2:], bytes([51, 2]) + L[:2], bytes([0, 0]),
                        bytes([3, 2]) + R[2:], bytes([51, 2]) + L[2:], b"\xff")

        print("\na lease of 8 s, /24, router 10.0.2.2:")
        after = lease(lease=8)
        if after is None:
            s.fail("bound", "the REQUEST never came")
            return
        if router_arp(after, GW_IP) is None:
            s.fail("bound: ARP for the router 10.0.2.2, from 10.0.2.15", "none")
        else:
            arp = router_arp(after, GW_IP)
            if arp[28:32] == GUEST_IP:
                s.ok("bound: ARP for the router 10.0.2.2, from 10.0.2.15")
            else:
                s.fail("bound: ARP for the router 10.0.2.2, from 10.0.2.15", f"sender {arp[28:32].hex()}")
        rows = c.screen()
        for want in ("  [net] DHCP Lease Acquired: 10.0.2.15",
                     "  [net] mask 255.255.255.0, router 10.0.2.2, lease 8 s"):
            if want in rows:
                s.ok(f"console: {want.strip()!r}")
            else:
                s.fail(f"console: {want.strip()!r}", "not on screen:\n" + "\n".join(rows))
        c.expect_arp_reply("bound: ARP for 10.0.2.15 answered", GUEST_IP)
        c.expect_nothing("bound: ARP for 10.0.2.15 from a broadcast sender MAC draws nothing",
                         arp_who_has_from(wire.BROADCAST, GUEST_IP))
        c.expect_ping("bound: ping at 10.0.2.15 answered", GUEST_IP)
        c.expect_echo("bound: UDP echo at 10.0.2.15 answered", GUEST_IP)
        for name, frame in [("bound: an OFFER changes nothing", offer(yiaddr=ip(10, 0, 2, 16))),
                            ("bound: an ACK for 10.0.2.16 changes nothing",
                             offer(yiaddr=ip(10, 0, 2, 16), message_type=5)),
                            ("bound: a NAK changes nothing", wire.dhcp_nak(xid))]:
            c.send(frame)
            c.expect_ping(name, GUEST_IP)
        expires("the 8 s lease expires", 8)

        print("\na lease of 4 s, its ACK held 1.5 s, timed -- nothing reads the screen:")
        timed_lease(4, hold=1.5)

        print("\na lease of 4 s, timed while a flood keeps the receive ring full:")
        flooded_lease(4)

        print("\na lease whose options are padded and split across repeats (RFC 3396):")
        # The flood's last requests may still be queued ahead of this OFFER.
        after = lease(options=split_options(2, 3), ack_options=split_options(5, 3), patience=15)
        name = "Pads and split options: a REQUEST, and the lease they add up to"
        if after is None:
            s.fail(name, "no REQUEST: the OFFER's options were not read as one")
            return
        rows = c.screen()
        want = "  [net] mask 255.255.255.0, router 10.0.2.2, lease 3 s"
        if router_arp(after, GW_IP) is None:
            s.fail(name, "bound, but no ARP for the router 10.0.2.2")
        elif want not in rows:
            s.fail(name, "the console does not show " + repr(want.strip()) + ":\n" + "\n".join(rows))
        else:
            s.ok(name, "mask, router, lease and server each rebuilt from two pieces")
        expires("the padded and split 3 s lease expires", 3)

        print("\na lease with a mask and no router (QEMU with restrict=on):")
        after = lease(lease=3, router=None)
        name = "bound with a mask and no router: no ARP, and the console says none"
        if after is None:
            s.fail(name, "no REQUEST")
            return
        arps = [f for f in after if f[12:14] == b"\x08\x06"]
        rows = c.screen()
        if arps:
            s.fail(name, f"{len(arps)} ARP(s)")
        elif "  [net] mask 255.255.255.0, router none, lease 3 s" not in rows:
            s.fail(name, "\n".join(rows))
        else:
            s.ok(name)
        expires("the 3 s lease with no router expires", 3)

        print("\na lease with no mask and no router (a server that sends neither):")
        after = lease(lease=4, mask=None, router=None)
        if after is None:
            s.fail("bound without options 1 and 3", "no REQUEST")
            return
        arps = [f for f in after if f[12:14] == b"\x08\x06"]
        if arps:
            s.fail("bound without options 1 and 3: no router, so no ARP", f"{len(arps)} ARP(s)")
        else:
            s.ok("bound without options 1 and 3: no router, so no ARP")
        rows = c.screen()
        want = "  [net] mask 255.0.0.0, router none, lease 4 s"
        if want in rows and any("classful" in r for r in rows):
            s.ok("console: the classful mask 255.0.0.0, and no router")
        else:
            s.fail("console: the classful mask 255.0.0.0, and no router", "\n".join(rows))
        expires("the 4 s lease expires", 4)

        for label, router, reason in [
                ("off the subnet (192.168.1.1)", ip(192, 168, 1, 1), "ignored: not on this subnet"),
                ("equal to the leased address", GUEST_IP, "ignored: this machine's own address"),
                ("the subnet's broadcast address", ip(10, 0, 2, 255),
                 "ignored: a network or broadcast address"),
                ("the subnet's network address", ip(10, 0, 2, 0),
                 "ignored: a network or broadcast address")]:
            name = f"a router {label} is ignored"
            print(f"\n{name}:")
            after = lease(lease=3, router=router)
            if after is None:
                s.fail(name, "no REQUEST")
                return
            arps = [f for f in after if f[12:14] == b"\x08\x06"]
            rows = c.screen()
            logged = any(reason in r for r in rows) and "  [net] mask 255.255.255.0, router none, lease 3 s" in rows
            if arps:
                s.fail(name, f"the client ARPed for it ({len(arps)} ARP)")
            elif not logged:
                s.fail(name, "the console does not say so:\n" + "\n".join(rows))
            else:
                s.ok(name, f"bound with no router; logged: {reason}")
            expires(f"the 3 s lease with a router {label} expires", 3)

        print("\na lasting lease -- its ACK gives no lease time -- and broadcasts it must not answer:")
        after = lease(lease=None)
        name = "an ACK with no lease time is taken as infinite, and said so"
        if after is None or router_arp(after, GW_IP) is None:
            s.fail(name, "no lease, or no ARP for the router 10.0.2.2")
            return
        rows = c.screen()
        if ("  [net] DHCP: the ACK gives no lease time; taking it as infinite" in rows
                and "  [net] mask 255.255.255.0, router 10.0.2.2, lease infinite" in rows):
            s.ok(name)
        else:
            s.fail(name, "\n".join(rows))
        c.expect_nothing("UDP to 255.255.255.255:7 is not echoed",
                         wire.BROADCAST + GW_MAC + b"\x08\x00"
                         + wire.ipv4_header(GW_IP, BROADCAST_IP, 17, 28)
                         + wire.udp_segment(GW_IP, BROADCAST_IP, 42000, 7, b"x" * 20))
        icmp = wire.icmp_message(8, 0, 0x6400, 1, ping.payload(40))
        c.expect_nothing("ICMP echo to 255.255.255.255 is not answered",
                         wire.BROADCAST + GW_MAC + b"\x08\x00"
                         + wire.ipv4_header(GW_IP, BROADCAST_IP, 1, len(icmp)) + icmp)
        c.expect_ping("still answering at 10.0.2.15", GUEST_IP)

        if c.w.id_breaks:
            s.fail("boot 1: IPv4 identification", "not consecutive: " + ", ".join(
                f"{a} then {b}" for a, b in c.w.id_breaks[:5]))
        else:
            s.ok("boot 1: every IPv4 identification the previous one plus one")
    finally:
        boot.close()
    return boot.pcap


# ---- boot 2: a different lease --------------------------------------------------

def boot_two(args, s):
    boot = Boot("2", args)
    c = Client(boot, s)
    lease42, router3, mask16 = ip(10, 0, 2, 42), ip(10, 0, 2, 3), ip(255, 255, 0, 0)
    # 63 routers in one 252-byte option, 10.0.2.3 first: 8 bytes are kept, and
    # the bound that keeps it at 8 is all that stands between the other 244 and
    # the stack. A fresh boot, so a server boot 1 already broke cannot hide it.
    routers = router3 + b"".join(ip(10, 0, 3, k) for k in range(1, 63))
    name = "boot 2: 63 routers in a 252-byte option; the first, 10.0.2.3, is ARPed from 10.0.2.42"
    try:
        print("\nboot 2: leased 10.0.2.42/16, 63 routers from 10.0.2.3:")
        try:
            discover, _ = c.w.lease(address=lease42, router=routers, mask=mask16, server=GW_IP)
        except RuntimeError as e:
            s.fail(name, str(e))
            return boot.pcap
        c.mac = discover["eth_src"]
        after = [f for f, _ in c.next_frames(1.0)]
        arp = router_arp(after, router3)
        if arp is not None and arp[28:32] == lease42:
            s.ok(name)
        else:
            s.fail(name, "not seen")
        c.expect_arp_reply("boot 2: ARP for 10.0.2.42 answered", lease42)
        c.expect_ping("boot 2: ping at 10.0.2.42 answered", lease42)
        c.expect_echo("boot 2: UDP echo at 10.0.2.42 answered", lease42)
        c.expect_nothing("boot 2: ping at 10.0.2.15 draws nothing",
                         ping.request(c.mac, 0x6500, 1, ping.payload(40))[0])
        c.expect_nothing("boot 2: ARP for 10.0.2.15 draws nothing", arp_who_has(GUEST_IP))
        c.expect_ping("boot 2: from 10.0.2.255, an ordinary host under /16, answered", lease42,
                      src_ip=ip(10, 0, 2, 255))
        c.expect_nothing("boot 2: from 10.0.255.255, the /16's broadcast, refused",
                         ping.request(c.mac, 0x6500, 2, ping.payload(40), dst_ip=lease42,
                                      src_ip=ip(10, 0, 255, 255))[0])
    finally:
        boot.close()
    return boot.pcap


# ---- boot 3: the wrap kernel ---------------------------------------------------------

def boot_three(args, s):
    """The wrap kernel's tick counter starts 1,000 ticks short of 2^32, so one
    of the first DISCOVER retransmissions is armed with a deadline past the
    wrap -- which a deadline compared unsigned would take as long past, and
    fire at once, every tick, until the counter wrapped. Nobody answers, and
    every interval is timed, out to the 64-s cap and once more past it."""
    boot = Boot("3", args, kernel=args.wrap_kernel)
    c = Client(boot, s)
    # One case for the whole schedule: which interval straddles the wrap
    # depends on how long the boot took, so no single interval can be named.
    name = "wrap boot: DISCOVERs 4, 8, 16, 32, 64 and 64 s +-1 apart, across the wrap"
    wrong = []
    try:
        d, t, _ = c.next_dhcp(20, kind=1)
        if d is None:
            s.fail(name, "no DISCOVER at all")
            return boot.pcap
        c.mac, xid = d["eth_src"], d["xid"]
        for nominal in (4, 8, 16, 32, 64, 64):
            d2, t2, others = c.next_dhcp(nominal + 1 + MARGIN + 0.5, kind=1)
            if d2 is None or others:
                wrong.append(f"after {t2 - t if t2 else nominal:.2f} s: "
                             + ("nothing" if d2 is None else f"{len(others)} other frame(s)"))
                break
            gap, bad = t2 - t, client_problems(d2, c.mac, 1, xid=xid)
            if bad:
                wrong.append(f"the DISCOVER after {gap:.2f} s: " + "; ".join(bad))
            if not nominal - 1 - MARGIN <= gap <= nominal + 1 + MARGIN:
                wrong.append(f"{gap:.2f} s where {nominal} s +-1 was due")
            else:
                s.ok(f"wrap boot: DISCOVER again after {nominal} s +-1", f"{gap:.2f} s")
            t = t2
    finally:
        boot.close()
    if wrong:
        s.fail(name, "; ".join(wrong[:4]))
    else:
        s.ok(name)
    return boot.pcap


# ---- tcpdump, over every pcap ------------------------------------------------------

def tcpdump_gate(s, pcap, lease):
    name = f"tcpdump: {os.path.basename(pcap)}"
    def run(*extra):
        return subprocess.run(["tcpdump", "-nn", "-r", pcap, *extra], capture_output=True,
                              text=True).stdout
    frames = len(run("udp").splitlines())
    verbose = run("-vv")
    ok = verbose.count("udp sum ok")
    complaints = re.findall(r"bad cksum|bad udp cksum|no cksum|\[\||Unknown \(", verbose)
    # One packet per unindented line and its indented continuation lines, so
    # each BOOTP message is judged on its own: exactly one message type, and
    # only the two a client in this state machine sends.
    packets = re.split(r"\n(?=\S)", verbose)
    bootp = [p for p in packets if "BOOTP/DHCP, Request from " in p]
    types = []
    odd = 0
    for p in bootp:
        found = re.findall(r"DHCP-Message \(53\), length 1: (\w+)", p)
        if len(found) != 1 or found[0] not in ("Discover", "Request"):
            odd += 1
        types += found
    problems = []
    if frames == 0 or ok != frames:
        problems.append(f"{ok} of {frames} UDP frames read 'udp sum ok'")
    if complaints:
        problems.append(f"complaints: {sorted(set(complaints))}")
    if not bootp or odd:
        problems.append(f"{odd} of {len(bootp)} BOOTP requests without exactly one "
                        "Discover or Request type")
    wants = ["Magic Cookie 0x63825363", "Flags [Broadcast]"]
    if lease is not None:
        wants += [f"Requested-IP (50), length 4: {'.'.join(map(str, lease))}",
                  "Server-ID (54), length 4: 10.0.2.2"]
    for want in wants:
        if want not in verbose:
            problems.append(f"never saw {want!r}")
    if problems:
        return s.fail(name, "; ".join(problems))
    return s.ok(name, f"{frames} UDP frames, all 'udp sum ok'; {len(bootp)} DHCP messages, "
                      f"{types.count('Discover')} Discover and {types.count('Request')} Request")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--wrap-kernel", required=True,
                    help="the kernel whose tick counter starts just short of 2^32")
    ap.add_argument("--initrd", required=True)
    ap.add_argument("--build", required=True)
    ap.add_argument("--wait", type=float, default=20.0)
    args = ap.parse_args()

    s = Suite()
    s3 = HeldSuite()
    pcaps = {}
    third = threading.Thread(target=lambda: pcaps.update(three=boot_three(args, s3)))
    third.start()
    pcap1 = boot_one(args, s)
    pcap2 = boot_two(args, s)
    third.join()

    print("\nboot 3, the wrap kernel -- run alongside, no server answering:")
    print("\n".join(s3.lines))
    s.failures += s3.failures

    print("\ntcpdump, re-decoding every frame the guest sent:")
    if pcap1:
        tcpdump_gate(s, pcap1, GUEST_IP)
    if pcap2:
        tcpdump_gate(s, pcap2, ip(10, 0, 2, 42))
    if pcaps.get("three"):
        tcpdump_gate(s, pcaps["three"], None)

    print("\n--- DHCP client ---")
    if s.failures:
        print(f"{len(s.failures)} case(s) FAILED: {', '.join(s.failures)}")
        return 1
    print("every case passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
