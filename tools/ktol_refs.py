#!/usr/bin/env python3
"""Offline xref finder for the ktol_* lobby-data keys in the runtime image dump.

Only .text is encrypted at rest in the on-disk nioh.exe - its headers, .pdata,
.rdata and .data are readable (the import descriptors at IMPORT are byte-
identical to the dump's, measured at exe file offset 0x17608e0 == dump RVA
0x1761ee0).  In the runtime dump file offset == RVA, image base is 0x140000000,
so VA = 0x140000000 + offset.  This tool is read-only.

Methods, all verified against docs/RE_KTOL_KEYS.md:
  * key strings        : byte scan of the whole image (offset == RVA)
  * code xrefs         : RIP-relative `lea r64,[rip+disp32]` in .text
  * data xrefs         : 8-byte little-endian values equal to the string VA
  * enclosing function : the .pdata RUNTIME_FUNCTION table (72,232 entries,
                         begin/end RVAs) is intact in the dump
  * import names       : the IMAGE_IMPORT_DESCRIPTOR array at IMPORT is intact
                         (only the IAT slots hold load-time addresses), so the
                         target of a `call *disp(%rip)` site can be named
  * call graph         : direct `call rel32` targets in .text

Usage:
    tools/ktol_refs.py strings
    tools/ktol_refs.py xrefs [key ...]
    tools/ktol_refs.py ptrs
    tools/ktol_refs.py callers 0x5fb560
    tools/ktol_refs.py dis 0x1405fb680 0x40
"""

import bisect
import os
import re
import struct
import subprocess
import sys

DUMP = os.environ.get(
    "NIOH_DUMP", os.path.expanduser("~/.cache/nioh3p/dumps/nioh3p_image_merged.bin"))
BASE = 0x140000000
TEXT = (0x1000, 0x1199498)          # .text RVA range (from the on-disk PE header)
PDATA = (0x2ee1000, 0xd39e0)        # .pdata (valid in the dump)
IMPORT = (0x1761ee0, 0x1a4)         # import directory (on-disk PE header, data
                                    # directory 1): 20 descriptors + terminator


_BUF = None


def load():
    """The image dump, read once per process (50 MB, several callers per run)."""
    global _BUF
    if _BUF is None:
        with open(DUMP, "rb") as f:
            _BUF = f.read()
    return _BUF


def cstr(d, o, limit=120):
    """NUL-terminated bytes at `o`, never more than `limit` bytes.

    The old body applied `limit` only when there was no NUL at all, so a caller
    bounding a fixed-size field could get an unbounded run instead
    (cstr(d, 0x245cb, 64) returned 809 bytes).
    """
    e = d.find(b"\x00", o)
    return d[o:min(e, o + limit) if e >= 0 else o + limit]


def functions(d):
    """[(begin_rva, end_rva)] from .pdata."""
    base, size = PDATA
    out = []
    for i in range(size // 12):
        b, e, _u = struct.unpack_from("<III", d, base + i * 12)
        if 0x1000 <= b < e <= TEXT[1]:
            out.append((b, e))
    out.sort()
    return out


class FuncMap:
    def __init__(self, funcs):
        self.funcs = funcs
        self.starts = [b for b, _ in funcs]

    def of(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        return self.funcs[i] if i >= 0 else None

    def name(self, rva):
        f = self.of(rva)
        return f"{f[0]:#x}-{f[1]:#x}" if f else "no containing function"


def imports(d):
    """{IAT slot RVA -> import name} from the IMAGE_IMPORT_DESCRIPTOR array.

    The descriptors, their OriginalFirstThunk name arrays and the DLL-name strings
    are intact in the dump, so the array is walked from its real location.  The
    previous version looked for the bytes of "KERNEL32.dll" and scanned the 0x1000
    bytes before that string for a descriptor holding the string's VA in its Name
    field: KERNEL32 is descriptor 8, at 0x1761f80, further than 0x1000 back from
    the string at 0x1763ff8, so the scan found no start and the map was always
    empty even though 391 IAT slots exist.
    """
    out = {}
    rva, size = IMPORT
    for k in range(size // 20):
        o = rva + k * 20                   # offset == RVA in this image
        oft, _ts, _fc, name, ft = struct.unpack_from("<IIIII", d, o)
        if not (oft or name or ft):
            break                          # null terminator
        if b".dll" not in cstr(d, name, 64).lower():
            break                          # not a descriptor array
        t = oft or ft                     # bound imports have no OriginalFirstThunk
        i = 0
        while True:
            v = struct.unpack_from("<Q", d, t + i * 8)[0]
            if v == 0:
                break
            nm = f"#{v & 0xffff}" if v >> 63 else cstr(d, v + 2).decode("latin1")
            out[ft + i * 8] = nm
            i += 1
    return out


def key_strings(d):
    """{key name -> [RVA, ...]} for every distinct ktol_* literal in the image."""
    out = {}
    for m in re.finditer(rb"ktol_[a-z0-9_]{1,40}", d):
        s, e = m.start(), m.end()
        prev = d[s - 1] if s else 0
        if 48 <= prev < 58 or 65 <= prev < 91 or 97 <= prev < 123 or prev == 0x5F:
            continue                              # inside a longer identifier
        out.setdefault(d[s:e].decode(), []).append(s)
    return out


def lea_xrefs(d):
    """[(site RVA, target RVA, dst reg)] for rip-relative `lea r64,[rip+disp32]`."""
    lo, hi = TEXT
    buf = d[lo:hi]
    out = []
    for m in re.finditer(rb"[\x48\x4c\x49\x4d]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", buf):
        p = m.start()
        disp = struct.unpack_from("<i", buf, p + 3)[0]
        # ModRM.reg is 3 bits and REX.R (bit 2 of the prefix byte) is the 4th, so
        # without it r8-r15 print as r0-r7 (6,450 of 108,364 lea sites here).
        reg = ((m.group()[2] >> 3) & 7) | (8 if m.group()[0] & 4 else 0)
        out.append((lo + p, lo + p + 7 + disp, reg))
    return out


def callmap(d):
    """{callee VA -> [call site VA, ...]} for direct `call rel32` in .text."""
    lo, hi = TEXT
    buf = d[lo:hi]
    out = {}
    for m in re.finditer(rb"\xe8", buf):
        p = m.start()
        if p + 5 > len(buf):
            break
        tgt = lo + p + 5 + struct.unpack_from("<i", buf, p + 1)[0]
        out.setdefault(BASE + tgt, []).append(BASE + lo + p)
    return out


KEY_CACHE = None


def keys():
    global KEY_CACHE
    if KEY_CACHE is None:
        KEY_CACHE = key_strings(load())
    return KEY_CACHE


def cmd_strings(_args):
    for k, offs in sorted(keys().items()):
        print(f"{k:28s} " + " ".join(f"{BASE + o:#x}" for o in offs))


def cmd_ptrs(_args):
    d = load()
    for k, offs in sorted(keys().items()):
        for o in offs:
            p = struct.pack("<Q", BASE + o)
            hits, s = [], 0
            while True:
                i = d.find(p, s)
                if i < 0:
                    break
                hits.append(i)
                s = i + 1
            print(f"{k:28s} str {BASE + o:#x}: "
                  + (", ".join(f"{BASE + h:#x}" for h in hits) if hits else "no 8-byte pointer found"))


def cmd_xrefs(args):
    d = load()
    fmap = FuncMap(functions(d))
    want = set(args) if args else set(keys())
    leas = lea_xrefs(d)
    for k in sorted(want):
        offs = keys().get(k, [])
        print(f"== {k}")
        for o in offs:
            tgt = o
            sites = [(s, r) for s, t, r in leas if t == tgt]
            print(f"   str {BASE + tgt:#x}  code xrefs: "
                  + (str(len(sites)) if sites else "0"))
            for s, r in sites:
                print(f"      {BASE + s:#x} lea r{r}  in {fmap.name(s)}")
            if not sites:
                # a lea at the *end* of one .pdata function may feed the next
                print("      (none -- see ptrs for data references)")


def addr(a):
    """VA for an address argument: both documented forms (0x5fb560, 0x1405fb560)."""
    va = int(a, 16)
    return va if va >= BASE else BASE + va


def cmd_callers(args):
    if not args:
        sys.stderr.write("usage: tools/ktol_refs.py callers 0x<VA|RVA> [0x...]\n")
        return 2
    try:
        addrs = [addr(a) for a in args]
    except ValueError:
        sys.stderr.write("callers: addresses are hexadecimal, e.g. 0x5fb560\n")
        return 2
    d = load()
    fmap = FuncMap(functions(d))
    cm = callmap(d)
    for va in addrs:
        cs = sorted(set(cm.get(va, [])))
        print(f"{va:#x} in {fmap.name(va - BASE)}: {len(cs)} direct caller(s)")
        for c in cs:
            print(f"   call site {c:#x} in {fmap.name(c - BASE)}")


def cmd_dis(args):
    if not args:
        sys.stderr.write("usage: tools/ktol_refs.py dis 0x<VA|RVA> [0x<size>]\n")
        return 2
    try:
        va = addr(args[0])
        size = int(args[1], 16) if len(args) > 1 else 0x60
    except ValueError:
        sys.stderr.write("dis: addresses are hexadecimal, e.g. 0x1405fb680 0x40\n")
        return 2
    d = load()
    if not (0 <= va - BASE < len(d)):
        sys.stderr.write(f"dis: {va:#x} is outside the image "
                         f"({BASE:#x}-{BASE + len(d):#x})\n")
        return 2
    iat = imports(d)
    ks = {BASE + o: k for k, offs in keys().items() for o in offs}   # keyed by VA
    p = subprocess.run(
        ["nice", "-n", "10", "objdump", "-D", "-b", "binary", "-m", "i386:x86-64",
         f"--adjust-vma={BASE:#x}", DUMP,
         f"--start-address={va:#x}", f"--stop-address={va + size:#x}"],
        capture_output=True, text=True)
    if p.returncode:
        sys.stderr.write(f"dis: objdump failed on {DUMP} (exit {p.returncode}): "
                         f"{p.stderr.strip() or 'no diagnostic'}\n")
        return 2
    out = []
    for line in p.stdout.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s+(\S.*)$", line)
        if m:
            out.append(m)
    if not out:
        sys.stderr.write(f"dis: objdump decoded nothing at {va:#x}+{size:#x}: "
                         "not 'no instructions', an empty window\n")
        return 2
    for m in out:
        a = int(m.group(1), 16)
        note = ""
        mm = re.search(r"# (0x[0-9a-f]+)", m.group(3))
        if mm:
            t = int(mm.group(1), 16)
            if t in ks:
                note = "KEY " + ks[t]
            elif t - BASE in iat:
                note = "IAT " + iat[t - BASE]
        if note:
            print(f"{a:#x}  {m.group(3).strip():46s} {note}")
        else:
            print(f"{a:#x}  {m.group(3).strip()}")


CMDS = {"strings": cmd_strings, "ptrs": cmd_ptrs, "xrefs": cmd_xrefs,
        "callers": cmd_callers, "dis": cmd_dis}


def main(argv):
    if len(argv) < 2 or argv[1] not in CMDS:
        print(__doc__)
        return 2
    try:
        return CMDS[argv[1]](argv[2:]) or 0
    except BrokenPipeError:                      # `| head` etc.
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
