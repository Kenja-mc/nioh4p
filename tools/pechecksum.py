#!/usr/bin/env python3
"""Recompute the PE optional-header checksum (stdlib only).

The checksum field sits 64 bytes into the optional header, i.e. at
e_lfanew + 4 (PE signature) + 20 (COFF file header) + 64. The algorithm is the
one from the PE specification (and CheckSumMappedFile):

  * sum all 16-bit little-endian words of the file,
  * with the four checksum bytes treated as zero,
  * folding carries back into the low 16 bits after every add,
  * fold once more, add the file length, keep 32 bits.

Odd-length files pad the last word with one zero byte. Writes the result back
to the file and prints both values.

Usage: tools/pechecksum.py <file.dll|file.exe>
"""

import struct
import sys

OPTIONAL_CHECKSUM_OFFSET = 64  # both PE32 and PE32+ optional headers


def pe_checksum_offset(data):
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError("not a PE file: missing MZ header")
    (e_lfanew,) = struct.unpack_from("<I", data, 0x3C)
    if e_lfanew + 24 + OPTIONAL_CHECKSUM_OFFSET + 4 > len(data):
        raise ValueError("truncated PE headers")
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise ValueError("not a PE file: missing PE signature")
    (optional_size,) = struct.unpack_from("<H", data, e_lfanew + 20)
    (magic,) = struct.unpack_from("<H", data, e_lfanew + 24)
    if magic not in (0x10B, 0x20B):  # PE32 / PE32+
        raise ValueError("unknown optional header magic 0x%04x" % magic)
    if optional_size < OPTIONAL_CHECKSUM_OFFSET + 4:
        raise ValueError("optional header too small for a checksum")
    return e_lfanew + 24 + OPTIONAL_CHECKSUM_OFFSET


def compute_checksum(data, checksum_offset):
    total = 0
    n = len(data)
    for off in range(0, n, 2):
        if checksum_offset <= off < checksum_offset + 4:
            continue  # four zero bytes, so no words to add
        lo = data[off]
        hi = data[off + 1] if off + 1 < n else 0
        total += lo | (hi << 8)
        total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (total + n) & 0xFFFFFFFF


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: %s <pe-file>\n" % argv[0])
        return 2
    path = argv[1]
    with open(path, "rb") as f:
        data = f.read()
    try:
        offset = pe_checksum_offset(data)
    except ValueError as exc:
        sys.stderr.write("pechecksum: %s: %s\n" % (path, exc))
        return 1
    old = struct.unpack_from("<I", data, offset)[0]
    new = compute_checksum(data, offset)
    if new != old:
        with open(path, "r+b") as f:
            f.seek(offset)
            f.write(struct.pack("<I", new))
    print("pechecksum: %s 0x%08x (was 0x%08x)" % (path, new, old))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
