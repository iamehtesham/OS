#!/usr/bin/env python3
"""Check net_checksum() -- the real C, from src/user/lib/net.c -- against RFC 1071.

The kernel is 32-bit and freestanding, but net_checksum is plain C over bytes,
so it is compiled here natively as a shared library and called through ctypes.
(A -m32 hosted build is not possible on this machine: there is no gcc-multilib.)
The two runtime functions net.c's other helpers call are stubbed; the checksum
calls neither.

Checks, each against a Python reference written from the RFC, not from net.c:
  1. RFC 1071's own worked example: 00 01 f2 03 f4 f5 f6 f7 sums to ddf2,
     so the checksum is 220d.
  2. Ten thousand random buffers, 0..1500 bytes, odd and even lengths.
  3. The receiver's check: data with its checksum appended sums to zero.
  4. Byte-order independence: swapping the bytes of every word swaps the
     bytes of the checksum. This is the property that makes one's-complement
     arithmetic the right choice for a protocol spoken by machines of both
     endiannesses, so it is tested rather than asserted.
  5. Edge values: empty, all zeroes, all 0xFF, a single byte.
  6. Top-bit errors, both ways. Flipping bit 15 in two words in the SAME
     direction must change the checksum -- that is what the end-around carry
     buys over two's complement, where the change is 65536 = 0. Flipping them
     in OPPOSITE directions must NOT change it, under either arithmetic: the
     limit is asserted too, so the documentation cannot claim more than the
     arithmetic gives.

usage: tools/check_checksum.py [--root DIR] [--count N]
Exit status 0 only if every check passes.
"""

import argparse
import ctypes
import os
import random
import struct
import subprocess
import sys
import tempfile

STUBS = r"""
#include <stddef.h>
#include <stdint.h>
void *u_memcpy(void *d, const void *s, size_t n)
{ unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
char *u_append_dec(char *out, const char *limit, uint32_t v) { (void)limit; (void)v; return out; }
"""


def reference(data):
    """RFC 1071, straight from the definition."""
    if len(data) % 2:
        data += b"\x00"
    total = sum(struct.unpack(f">{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return ~total & 0xFFFF


def build(root, where):
    stubs = os.path.join(where, "stubs.c")
    lib = os.path.join(where, "libnetcheck.so")
    with open(stubs, "w") as f:
        f.write(STUBS)
    subprocess.run(["gcc", "-O2", "-shared", "-fPIC", "-ffreestanding", f"-I{root}/include",
                    "-o", lib, f"{root}/src/user/lib/net.c", stubs], check=True)
    so = ctypes.CDLL(lib)
    so.net_checksum.argtypes = [ctypes.c_char_p, ctypes.c_uint32]
    so.net_checksum.restype = ctypes.c_uint16
    return so


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--count", type=int, default=10000)
    args = ap.parse_args()

    failures = []

    def check(name, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'} {name}{(' -- ' + detail) if detail and not ok else ''}")
        if not ok:
            failures.append(name)

    with tempfile.TemporaryDirectory() as where:
        so = build(os.path.abspath(args.root), where)

        def c(data):
            return so.net_checksum(data, len(data))

        example = bytes.fromhex("0001f203f4f5f6f7")
        check("RFC 1071 worked example gives 0x220d", c(example) == 0x220D, f"got {c(example):#06x}")

        rng = random.Random(1071)
        mismatches = 0
        for _ in range(args.count):
            data = bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 1500)))
            if c(data) != reference(data):
                mismatches += 1
        check(f"{args.count} random buffers match the reference", mismatches == 0,
              f"{mismatches} mismatches")

        bad_verify = 0
        for _ in range(2000):
            n = rng.randint(0, 750) * 2  # the checksum must sit on a word boundary
            data = bytes(rng.getrandbits(8) for _ in range(n))
            whole = data + struct.pack(">H", c(data))
            if c(whole) != 0:
                bad_verify += 1
        check("data plus its own checksum sums to 0 (the receiver's check)", bad_verify == 0,
              f"{bad_verify} failures")

        not_symmetric = 0
        for _ in range(2000):
            n = rng.randint(1, 750) * 2
            data = bytes(rng.getrandbits(8) for _ in range(n))
            swapped = b"".join(data[i + 1:i + 2] + data[i:i + 1] for i in range(0, n, 2))
            a, b = c(data), c(swapped)
            if b != (((a & 0xFF) << 8) | (a >> 8)):
                not_symmetric += 1
        check("byte-swapping every word byte-swaps the checksum", not_symmetric == 0,
              f"{not_symmetric} failures")

        def flip_top(data, word):
            b = bytearray(data)
            b[2 * word] ^= 0x80
            return bytes(b)

        same_missed = opposite_caught = trials = 0
        for _ in range(2000):
            n = rng.randint(2, 750) * 2
            data = bytes(rng.getrandbits(8) for _ in range(n))
            words = n // 2
            zeros = [w for w in range(words) if not data[2 * w] & 0x80]
            ones = [w for w in range(words) if data[2 * w] & 0x80]
            base = c(data)
            if len(zeros) >= 2:                       # both 0 -> 1: same direction
                i, j = rng.sample(zeros, 2)
                trials += 1
                if c(flip_top(flip_top(data, i), j)) == base:
                    same_missed += 1
            if len(ones) >= 2:                        # both 1 -> 0: same direction
                i, j = rng.sample(ones, 2)
                trials += 1
                if c(flip_top(flip_top(data, i), j)) == base:
                    same_missed += 1
            if zeros and ones:                        # one each way: they cancel
                i, j = rng.choice(zeros), rng.choice(ones)
                if c(flip_top(flip_top(data, i), j)) != base:
                    opposite_caught += 1
        check(f"two same-direction top-bit errors are caught ({trials} trials)", same_missed == 0,
              f"{same_missed} went unseen")
        check("two opposite-direction top-bit errors cancel (the documented limit)",
              opposite_caught == 0, f"{opposite_caught} were caught, contradicting the arithmetic")

        for name, data in [("empty", b""), ("one byte", b"\xab"), ("all zeroes", bytes(64)),
                           ("all 0xFF", b"\xff" * 64), ("odd, all 0xFF", b"\xff" * 63)]:
            check(f"edge: {name}", c(data) == reference(data),
                  f"got {c(data):#06x}, want {reference(data):#06x}")

    print(f"\n{'all checks passed' if not failures else f'{len(failures)} check(s) FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
