#!/usr/bin/env python3
"""Step 5 of the UDP brief, automated: the host's own netcat, through QEMU's
user-mode network, to the echo service on 10.0.2.15:7.

The brief says `nc -u 10.0.2.15 7`, which cannot work on this machine: the host
routes 10.0.2.15 out through its LAN router, not to QEMU, because user-mode
networking is NAT; and port 7 on the host side needs privilege. What does work
is QEMU's port forward -- `hostfwd=udp:127.0.0.1:PORT-10.0.2.15:7` -- which
`make qemu` sets up on port 7007 and `make udp-host` on its own port. A datagram
sent to 127.0.0.1:PORT is handed by QEMU's user-mode stack (libslirp) to the
guest as coming from 10.0.2.2, and the guest's reply is carried back to the
sender's socket. So this is the whole path: host socket, a real UDP/IP stack
this project did not write, the card, the guest, and back.

What it proves: addressing, the port swap, the length and the data survive a
real stack in both directions. What it does NOT prove is that the guest
computes its checksum -- libslirp skips the check when the checksum is 0 and
always stamps a valid checksum on what it sends the guest, and the correct
reply checksum equals the request's. That is make udp's job, whose requests can
carry checksum 0. Here the pcap is still gated on every reply's checksum.

Steps:
  1. wait for the guest by sending tagged probes from an unconnected socket
     until one comes back (late echoes of earlier probes are ignored);
  2. `printf 'hello from the host\\n' | nc -u -p SPORT -W1 -w2 127.0.0.1 PORT`
     -- the OpenBSD netcat installed on the host, from a source port chosen here
     -- must print the same line back;
  3. read the guest's console out of VGA memory and find the brief's line for
     that datagram -- `UDP Packet | Port SPORT -> 7 | Length: 28`, with the
     port nc was told to use, which QEMU passes through unchanged -- and the
     message on the line after it;
  4. a sweep of sizes up to 1472 bytes, and every byte value, each byte-exact.

usage: tools/udp_host.py --port N [--monitor PATH] [--wait S] [--count-file PATH]
Exit status 0 only if every step passes.
"""

import argparse
import os
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import udp_echo  # noqa: E402 -- its Monitor and its preview rules

LINE = b"hello from the host\n"


def wait_for_guest(port, seconds):
    """Probes until one is echoed. Unconnected, so an ICMP port-unreachable from
    the host kernel before QEMU has bound the port is not raised as an error;
    and every probe carries its own tag, because libslirp holds a datagram for
    up to a second while it learns the guest's MAC and may deliver it late."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    deadline = time.time() + seconds
    probe = 0
    try:
        while time.time() < deadline:
            probe += 1
            tag = f"probe {probe}".encode()
            sock.sendto(tag, ("127.0.0.1", port))
            until = time.time() + 1.0
            while time.time() < until:
                sock.settimeout(max(0.01, until - time.time()))
                try:
                    data, _ = sock.recvfrom(2048)
                except (TimeoutError, socket.timeout):
                    break
                if data == tag:
                    return probe
    finally:
        sock.close()
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, required=True, help="the host side of the forward")
    ap.add_argument("--monitor", help="QEMU monitor socket, to read the guest's console")
    ap.add_argument("--wait", type=float, default=30.0)
    ap.add_argument("--count-file", help="write the number of echoes received here")
    args = ap.parse_args()

    failures = []
    received = 0

    def fail(name, why):
        print(f"  FAIL {name}: {why}")
        failures.append(name)

    probes = wait_for_guest(args.port, args.wait)
    if not probes:
        print(f"nothing came back from 127.0.0.1:{args.port} in {args.wait:.0f} s")
        return 1
    received += 1
    print(f"UDP to 127.0.0.1:{args.port}, forwarded by QEMU to 10.0.2.15:7; "
          f"the guest answered probe {probes}\n")

    monitor = None
    if args.monitor:
        monitor = udp_echo.Monitor(args.monitor, args.wait)
        monitor.command("sendkey esc")        # show the system console, where [net] logs
        time.sleep(0.5)

    # The host's own netcat. -p: from a source port chosen here, so the guest's
    # log line can be checked for it. -W1: exit once one datagram has come
    # back. -w2: give up two seconds after the last activity. Judged by what it
    # prints, because nc exits 0 whether or not anything came back.
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.bind(("127.0.0.1", 0))
    nc_port = probe.getsockname()[1]
    probe.close()
    command = (f"printf 'hello from the host\\n' | "
               f"nc -u -p {nc_port} -W1 -w2 127.0.0.1 {args.port}")
    try:
        result = subprocess.run(command, shell=True, capture_output=True, timeout=15)
        output = result.stdout
    except subprocess.TimeoutExpired:
        output = None
    if output == LINE:
        received += 1
        print(f"  ok   $ {command}")
        print(f"       {output.decode().rstrip()}")
    else:
        fail("netcat", f"printed {output!r}, want {LINE!r}")

    if monitor:
        time.sleep(1.0)
        rows = monitor.screen()
        found = udp_echo.find_logged(rows, nc_port, 8 + len(LINE), LINE)
        if found is None:
            fail("the guest logged netcat's datagram",
                 "the brief's line and the message were not on screen; it showed:\n"
                 + "\n".join(rows))
        else:
            print("  ok   the guest's console, read out of VGA memory:")
            print(f"       {rows[found]}")
            print(f"       {rows[found + 1]}")
            print(f"       {rows[found + 2]}")

    # A sweep through the same forward, from one connected socket. It is a fresh
    # socket, so a late probe echo cannot land on it.
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.connect(("127.0.0.1", args.port))
    sock.settimeout(3.0)
    cases = [(f"{n} bytes", bytes(((i * 37 + n) & 0xFF) or 1 for i in range(n)))
             for n in (0, 1, 2, 17, 18, 100, 512, 1000, 1471, 1472)]
    cases.append(("every byte value", bytes(range(256))))
    swept = 0
    for name, data in cases:
        sock.send(data)
        try:
            back = sock.recv(4096)
        except (TimeoutError, socket.timeout, ConnectionRefusedError) as e:
            fail(f"sweep: {name}", f"no echo ({e.__class__.__name__})")
            continue
        if back != data:
            fail(f"sweep: {name}", f"{len(back)} bytes came back, differing from the {len(data)} sent")
            continue
        received += 1
        swept += 1
    sock.close()
    if swept == len(cases):
        print(f"  ok   {swept} datagrams of 0 to 1472 bytes, each echoed byte for byte")

    if args.count_file:
        with open(args.count_file, "w") as f:
            f.write(f"{received}\n")

    print("\n--- 127.0.0.1:%d -> 10.0.2.15:7 ---" % args.port)
    if failures:
        print(f"{len(failures)} step(s) FAILED: {', '.join(failures)}")
        return 1
    print(f"every step passed ({received} echoes received through QEMU's user-mode network)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
