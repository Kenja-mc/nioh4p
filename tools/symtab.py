#!/usr/bin/env python3
"""Symbol names out of nioh.exe's readable sections (no image dump needed).

`.text` is encrypted at rest, but `.rdata`/`.data` are not, and they carry the
game's symbol names - the whole `ktol` online layer, the session classes, the
EOS/Steam surface and the script-visible engine API. This tool extracts them
(`stats`, `find`, `dump`) and writes a sorted list.

Addresses are *not* in this table: it is a name pool. Use `tools/rtti.py` on a
runtime image dump for vtable/method addresses (see docs/FINDINGS.md 6.3).

Usage:
    tools/symtab.py stats                    # counts + a few section totals
    tools/symtab.py find CSessionP2p Lobby    # names containing all substrings
    tools/symtab.py dump analysis/symbols.txt # write every name, sorted
"""

import os
import re
import struct
import sys
from collections import Counter

GAME = os.environ.get("NIOH_DIR",
                      os.path.expanduser("~/.local/share/Steam/steamapps/common/Nioh"))
EXE = os.path.join(GAME, "nioh.exe")

READABLE = (".rdata", "_RDATA", ".data")
# C++-ish names: at least one '::', or a mangled '?' name, no whitespace runs
TOKEN = re.compile(rb"[A-Za-z_?$@][A-Za-z0-9_$@:<>~,.\-\[\]]{4,200}")


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
        out.append((nm, va, vs, ra, rs))
    return out


class SymNames:
    def __init__(self, path=EXE):
        d = open(path, "rb").read()
        self.path = path
        self.per_section = Counter()
        self.names = []
        seen = set()
        for nm, va, vs, ra, rs in sections(d):
            if nm not in READABLE:
                continue
            for m in TOKEN.finditer(d[ra:ra + rs]):
                t = m.group().decode("ascii")
                if "::" not in t and not t.startswith("?"):
                    continue
                if t in seen:
                    continue
                seen.add(t)
                self.names.append(t)
                self.per_section[nm] += 1
        self.names.sort()

    def stats(self):
        print(f"{self.path}: {len(self.names)} unique symbol names")
        for nm, n in self.per_section.most_common():
            print(f"   {nm:8s} {n}")

    def find(self, subs):
        for n in self.names:
            if all(s in n for s in subs):
                print(n)

    def dump(self, out):
        with open(out, "w") as f:
            f.write("\n".join(self.names) + "\n")
        print(f"wrote {out} ({len(self.names)} names)")


def main():
    t = SymNames()
    args = sys.argv[1:]
    if not args or args[0] == "stats":
        t.stats()
    elif args[0] == "find":
        t.find(args[1:])
    elif args[0] == "dump":
        t.dump(args[1] if len(args) > 1 else "analysis/symbols.txt")
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
