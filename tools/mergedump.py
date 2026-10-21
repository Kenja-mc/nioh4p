#!/usr/bin/env python3
"""Merge nioh3p image snapshots into one image with maximum decrypted coverage.

`nioh.exe` is protected such that `.text` is encrypted on disk and decrypted page
by page while the game executes. A single snapshot therefore holds only the code
the run had already touched; the proxy can take periodic snapshots
(`dump_every_s`). This tool walks the snapshots plus the on-disk exe and, for
every 4 KB page, keeps whichever version is *not* the encrypted one, preferring
the latest snapshot. It then reports coverage per section so a run's usefulness
is visible instead of assumed.

Usage:
    tools/mergedump.py --out merged.bin dump1.bin dump2.bin ...
    tools/mergedump.py --report dump1.bin dump2.bin ...     # coverage only

Pages that never differed from the on-disk bytes stay encrypted in the output
(they are byte-identical to the file, not zeroed), so a merged image is still a
valid module image to load at 0x140000000.
"""

import argparse
import hashlib
import os
import struct
import sys

BASE = 0x140000000
PAGE = 0x1000
GAME = os.environ.get("NIOH_DIR",
                      os.path.expanduser("~/.local/share/Steam/steamapps/common/Nioh"))


def sections(d):
    e = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, e + 6)[0]
    optsz = struct.unpack_from("<H", d, e + 20)[0]
    so = e + 24 + optsz
    out = []
    for i in range(nsec):
        s = d[so + i * 40:so + (i + 1) * 40]
        nm = s[:8].rstrip(b"\0").decode()
        vs, va, rs, ra = struct.unpack_from("<IIII", s, 8)
        out.append({"name": nm, "va": va, "vsize": vs, "raw": ra, "rsize": rs})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dumps", nargs="+")
    ap.add_argument("--exe", default=os.path.join(GAME, "nioh.exe"))
    ap.add_argument("--out")
    ap.add_argument("--report", action="store_true")
    a = ap.parse_args()

    disk = open(a.exe, "rb").read()
    secs = sections(disk)
    size = max(s["va"] + s["vsize"] for s in secs)
    snaps = [open(p, "rb").read() for p in a.dumps]
    print(f"on-disk exe {len(disk):,} B, image {size:,} B, snapshots: "
          f"{', '.join(os.path.basename(p) for p in a.dumps)}")

    merged = bytearray(disk[:1]) * 0  # empty
    merged = bytearray(size)
    coverage = {s["name"]: [0, 0] for s in secs}   # [decrypted_pages, total]
    for s in secs:
        total = (s["vsize"] + PAGE - 1) // PAGE
        coverage[s["name"]][1] = total
        dstart = s["va"]
        if s["raw"] + s["rsize"] > len(disk):
            continue
        disk_sec = disk[s["raw"]:s["raw"] + s["vsize"]]
        merged[dstart:dstart + len(disk_sec)] = disk_sec
        for pg in range(total):
            o = pg * PAGE
            dpg = disk_sec[o:o + PAGE]
            chosen = None
            for snap in snaps:
                cand = snap[dstart + o:dstart + o + PAGE]
                if len(cand) == len(dpg) and cand != dpg:
                    chosen = cand
            if chosen is not None:
                merged[dstart + o:dstart + o + len(chosen)] = chosen
                coverage[s["name"]][0] += 1

    print(f"{'section':10s} {'decrypted':>10s} / {'pages':>6s}   {'bytes':>12s}")
    for s in secs:
        dec, tot = coverage[s["name"]]
        print(f"{s['name']:10s} {dec:10d} / {tot:6d}   {dec * PAGE:12,d}")

    if a.out:
        with open(a.out, "wb") as f:
            f.write(merged)
        h = hashlib.md5(merged).hexdigest()
        print(f"wrote {a.out} ({len(merged):,} B, md5 {h})")


if __name__ == "__main__":
    sys.exit(main())
