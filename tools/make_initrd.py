#!/usr/bin/env python3
"""Pack a directory of files into the kernel's initrd image.

Image layout:

    [superblock][file_count x file_header][file data ...]

The struct formats below mirror include/fs/initrd.h exactly. Both sides must
change together, so the sizes are asserted here rather than left to drift: a
mismatch fails the build instead of producing an image the kernel misparses.

Offsets in a file header are measured from the start of the image, which is
why the driver needs nothing but the base address to resolve them.

Usage: make_initrd.py <source-directory> <output-image>
"""

import os
import struct
import sys

# Must match INITRD_MAGIC and INITRD_NAME_MAX in include/fs/initrd.h.
MAGIC = 0x494E5244  # 'INRD'
NAME_MAX = 64

SUPERBLOCK_FMT = "<II"                    # magic, file_count
FILE_HEADER_FMT = "<I{}sII".format(NAME_MAX)  # magic, name, offset, length

SUPERBLOCK_SIZE = struct.calcsize(SUPERBLOCK_FMT)
FILE_HEADER_SIZE = struct.calcsize(FILE_HEADER_FMT)

assert SUPERBLOCK_SIZE == 8, "superblock must be 8 bytes, got %d" % SUPERBLOCK_SIZE
assert FILE_HEADER_SIZE == 76, "file header must be 76 bytes, got %d" % FILE_HEADER_SIZE


def build(source_dir, output_path):
    if not os.path.isdir(source_dir):
        sys.exit("make_initrd: '%s' is not a directory" % source_dir)

    # Sorted so the image is reproducible: an unsorted listdir would reorder
    # entries between builds and change the output for no reason.
    names = sorted(
        entry
        for entry in os.listdir(source_dir)
        if os.path.isfile(os.path.join(source_dir, entry))
    )

    files = []

    for name in names:
        encoded = name.encode("utf-8")

        # One byte is reserved for the terminator the kernel relies on.
        if len(encoded) > NAME_MAX - 1:
            sys.exit(
                "make_initrd: '%s' is %d bytes, longer than the %d the format allows"
                % (name, len(encoded), NAME_MAX - 1)
            )

        with open(os.path.join(source_dir, name), "rb") as handle:
            files.append((encoded, handle.read()))

    data_start = SUPERBLOCK_SIZE + FILE_HEADER_SIZE * len(files)

    headers = bytearray()
    payload = bytearray()
    offset = data_start

    for encoded, contents in files:
        # struct.pack zero-pads the name field, so it is always terminated.
        headers += struct.pack(FILE_HEADER_FMT, MAGIC, encoded, offset, len(contents))
        payload += contents
        offset += len(contents)

    image = struct.pack(SUPERBLOCK_FMT, MAGIC, len(files)) + bytes(headers) + bytes(payload)

    output_dir = os.path.dirname(output_path)

    if output_dir:
        os.makedirs(output_dir, exist_ok=True)

    with open(output_path, "wb") as handle:
        handle.write(image)

    print(
        "make_initrd: %d file(s), %d bytes -> %s" % (len(files), len(image), output_path)
    )

    for encoded, contents in files:
        print("  %-24s %6d B" % (encoded.decode("utf-8"), len(contents)))


def main(argv):
    if len(argv) != 3:
        sys.exit("usage: %s <source-directory> <output-image>" % argv[0])

    build(argv[1], argv[2])


if __name__ == "__main__":
    main(sys.argv)
