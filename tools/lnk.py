#!/usr/bin/env python3
"""Koei "K300" archive reader (.lnk files of Nioh: Complete Edition).

Layout, established from the files themselves (all 60 archives agree):

    0x00  char[4]  "K300"
    0x04  u32      reserved (0)
    0x08  u64      entry count
    0x10  u64      total size = end of the last entry = file size (entries are
                   padded to 0x800, so this is larger than the sum of sizes)
    0x18  u64      0x800, the table block granularity (64 entries per block)
    0x20  entry[count], 32 bytes each:
            u64 offset   absolute file offset, 0x800-aligned, monotonic
            u64 size     stored bytes
            u64 size2    equal to size in every archive here
            u64 flags    0 in every entry here (no compression marker)

    first data byte = round_up(0x20 + count*32, 0x800) = entry[0].offset

There is no name table: the mapping name -> index lives outside the archive
(the game reads names from its own tables), so this tool addresses entries by
index and by offset. Run `list` to see counts, `dump <archive> <index>` to
extract one entry.

Usage:
    tools/lnk.py list                 # every archive in the game, summary + type mix
    tools/lnk.py info  <archive>      # header and first/last entries
    tools/lnk.py dump  <archive> <i> [out]   # extract entry i
    tools/lnk.py scan  <archive>      # signatures of every entry
"""

import os
import struct
import sys
from collections import Counter

GAME = os.environ.get("NIOH_DIR",
                      os.path.expanduser("~/.local/share/Steam/steamapps/common/Nioh"))
ARCHIVE_DIR = os.path.join(GAME, "archive")

HDR = 0x20
ENT = 0x20


class Entry:
    __slots__ = ("index", "offset", "size", "size2", "flags")

    def __init__(self, index, offset, size, size2, flags):
        self.index, self.offset, self.size = index, offset, size
        self.size2, self.flags = size2, flags


class Archive:
    def __init__(self, path):
        self.path = path
        self.size = os.path.getsize(path)
        with open(path, "rb") as f:
            head = f.read(HDR)
            magic, res, self.count, self.total, self.block = struct.unpack("<4sIQQQ", head)
            if magic != b"K300":
                raise ValueError(f"{path}: not a K300 archive (magic {magic!r})")
            self.reserved = res
            raw = f.read(self.count * ENT)
        self.entries = []
        for i in range(self.count):
            off, size, size2, flags = struct.unpack_from("<QQQQ", raw, i * ENT)
            self.entries.append(Entry(i, off, size, size2, flags))

    @property
    def data_start(self):
        return self.entries[0].offset if self.entries else (HDR + 0x7FF) & ~0x7FF

    def check(self):
        """Everything a well-formed archive must satisfy; returns [problems]."""
        bad = []
        if self.total != self.size:
            bad.append(f"total {self.total:#x} != file size {self.size:#x}")
        table_end = (HDR + self.count * ENT + 0x7FF) & ~0x7FF
        if self.entries and self.entries[0].offset != table_end:
            bad.append(f"first offset {self.entries[0].offset:#x} != table end {table_end:#x}")
        prev = 0
        for e in self.entries:
            if e.offset % 0x800:
                bad.append(f"entry {e.index}: offset {e.offset:#x} not 0x800-aligned")
            if e.offset < prev:
                bad.append(f"entry {e.index}: offsets not monotonic")
            if e.offset + e.size > self.size:
                bad.append(f"entry {e.index}: {e.offset:#x}+{e.size:#x} past EOF")
            if e.size != e.size2:
                bad.append(f"entry {e.index}: size {e.size:#x} != size2 {e.size2:#x}")
            if e.flags:
                bad.append(f"entry {e.index}: flags {e.flags:#x}")
            prev = e.offset
        if self.entries:
            end = self.entries[-1].offset + self.entries[-1].size
            if (end + 0x7FF) & ~0x7FF != self.total:
                bad.append(f"last entry end {end:#x} does not pad up to total {self.total:#x}")
        return bad

    def read(self, i):
        e = self.entries[i]
        with open(self.path, "rb") as f:
            f.seek(e.offset)
            return f.read(e.size)


def sig16(blob):
    """First 16 bytes, with the printable ASCII run kept for eyeballing."""
    txt = "".join(chr(b) if 32 <= b < 127 else "." for b in blob[:16])
    return blob[:8].hex(" "), txt


def entropy(blob):
    if not blob:
        return 0.0
    c = Counter(blob)
    import math
    n = len(blob)
    return -sum((v / n) * math.log2(v / n) for v in c.values())


def archives():
    return sorted(f for f in os.listdir(ARCHIVE_DIR) if f.endswith(".lnk"))


def cmd_list():
    tot_e = tot_b = 0
    for name in archives():
        a = Archive(os.path.join(ARCHIVE_DIR, name))
        bad = a.check()
        tot_e += a.count
        tot_b += a.size
        print(f"{name}  entries={a.count:5d}  size={a.size:13,d}  data@{a.data_start:#x}  "
              f"{'OK' if not bad else 'BAD: ' + '; '.join(bad[:2])}")
    print(f"total: {len(archives())} archives, {tot_e} entries, {tot_b:,d} bytes")


def cmd_info(path):
    a = Archive(path)
    print(f"{path}: count={a.count} total={a.total:#x} block={a.block:#x} file={a.size:#x}")
    print("check:", a.check() or "clean")
    for e in a.entries[:4] + (["..."] if a.count > 8 else []) + a.entries[-4:]:
        if e == "...":
            print("  ...")
            continue
        blob = a.read(e.index)
        h, t = sig16(blob)
        print(f"  [{e.index:4d}] off={e.offset:#010x} size={e.size:9,d} flags={e.flags} "
              f"entropy={entropy(blob[:4096]):.2f}  {h}  {t!r}")


def cmd_scan(path):
    a = Archive(path)
    sigs = Counter()
    ents = []
    for e in a.entries:
        blob = a.read(e.index)
        sigs[blob[:4].hex()] += 1
        ents.append(entropy(blob[:4096]))
    print(f"{path}: {a.count} entries, entropy min/mean/max = "
          f"{min(ents):.2f}/{sum(ents)/len(ents):.2f}/{max(ents):.2f}")
    for s, n in sigs.most_common(12):
        print(f"  first4={s}  x{n}")


def cmd_dump(path, i, out=None):
    a = Archive(path)
    blob = a.read(int(i))
    out = out or f"entry_{int(i):04d}.bin"
    with open(out, "wb") as f:
        f.write(blob)
    print(f"wrote {out} ({len(blob)} bytes) from {path} entry {i}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    if cmd == "list":
        cmd_list()
    elif cmd == "info":
        cmd_info(sys.argv[2])
    elif cmd == "scan":
        cmd_scan(sys.argv[2])
    elif cmd == "dump":
        cmd_dump(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
