#!/usr/bin/env python3
"""Build the proxy's call-site name table (nioh3p.sites.txt).

The proxy hooks Steam/EOS entry points through vtables and the import table.
Knowing *which* game function made a call is what turns a hook log into an
answer, and the game has no name->address table of its own - but the RTTI class
map does. tools/rtti.py writes analysis/classmap.txt from a runtime image dump:

    SearchTask@task@session@eos@sessionp2p@ktol
      vtable 0x141585ef8  methods: 0x1405cd0c0 0x1405dbf30 0x1405cc230

This tool turns every method VA into an "<rva> <Class>::vfn<N>" line, sorted by
rva, ready for the proxy's nearest-below lookup.

Usage:
    tools/sites.py                         # write nioh3p.sites.txt in the repo root
    tools/sites.py --out PATH              # elsewhere
    tools/sites.py --extra FILE            # merge hand-written "0xRVA Name" lines (they win)
    tools/sites.py --probe 0x5edc9e 0x5ef800   # what names would a caller at these RVAs get?
"""

import argparse
import os
import re
import sys

BASE = 0x140000000
IMAGE_LIMIT = 0x3073000          # last .bind section end in this build
CLASSMAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "analysis", "classmap.txt")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "nioh3p.sites.txt")

VTABLE_RE = re.compile(r"^\s+vtable\s+0[xX]([0-9a-fA-F]+)\s+methods:(.*)$")
HEX_RE = re.compile(r"0[xX]([0-9a-fA-F]+)")


def pretty(name):
    """SearchTask@task@session@eos@sessionp2p@ktol -> ktol::sessionp2p::eos::session::task::SearchTask"""
    if name.startswith("?") or "@" not in name:
        return name
    parts = [p for p in name.split("@") if p]
    return "::".join(reversed(parts)) if parts else name


def parse_classmap(path):
    """-> list of (rva, name) in file order: methods as 'Class::vfnN', vtables as 'vtable Class'.

    Every address must land inside the image.  The file is written mechanically
    by tools/rtti.py from a runtime image, so an address outside it means the
    classmap and this build disagree - and a silently dropped name would look to
    the proxy like a call site that simply has no name.
    """
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError as exc:
        sys.exit(f"cannot read {path}: {exc}")
    out = []
    bad = []
    cls = None
    collecting = False
    slot = 0
    for n, line in enumerate(lines, 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if not line[0].isspace():
            cls = line.strip()
            collecting = False
            slot = 0
            continue
        m = VTABLE_RE.match(line)
        if m:
            collecting = True
            slot = 0
            vva = int(m.group(1), 16)
            if BASE <= vva < BASE + IMAGE_LIMIT and cls:
                out.append((vva - BASE, f"vtable {pretty(cls)}"))
            else:
                bad.append((n, vva, cls))
            for h in HEX_RE.findall(m.group(2)):
                va = int(h, 16)
                if BASE <= va < BASE + IMAGE_LIMIT and cls:
                    out.append((va - BASE, f"{pretty(cls)}::vfn{slot}"))
                else:
                    bad.append((n, va, cls))
                slot += 1
            continue
        if collecting:
            hits = HEX_RE.findall(line)
            if hits:
                for h in hits:
                    va = int(h, 16)
                    if BASE <= va < BASE + IMAGE_LIMIT and cls:
                        out.append((va - BASE, f"{pretty(cls)}::vfn{slot}"))
                    else:
                        bad.append((n, va, cls))
                    slot += 1
            else:
                collecting = False
    if bad:
        for n, va, c in bad[:20]:
            sys.stderr.write(f"{path}:{n}: {va:#x} is outside the image "
                             f"({BASE:#x}-{BASE + IMAGE_LIMIT:#x}), class "
                             f"{c or '(none)'}\n")
        if len(bad) > 20:
            sys.stderr.write(f"   ... and {len(bad) - 20} more\n")
        sys.exit(f"sites.py: refusing {path}: {len(bad)} of its addresses are not in "
                 "the image of this build, so the call-site table would be incomplete")
    return out


def parse_extra(path):
    """[(rva, name)] from a hand-written "0xRVA Name" file.

    Unparsable lines and addresses outside the image are reported: these lines
    are hand-written and they win over the classmap, so dropping one silently
    would change the table without a trace.
    """
    out = []
    skipped = []
    if not path:
        return out
    if not os.path.exists(path):
        print(f"note: extra file {path} does not exist, skipping", file=sys.stderr)
        return out
    for n, raw in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^(?:0[xX])?([0-9a-fA-F]+)\s+(.+)$", line)
        if not m:
            skipped.append((n, line, "not '<rva> <name>'"))
            continue
        rva = int(m.group(1), 16)
        if not 0 <= rva < IMAGE_LIMIT:
            # RVAs are what the proxy's nearest-below lookup sees; a VA here (the
            # classmap's own form) would become a nonsense entry
            skipped.append((n, line, f"{rva:#x} is not an RVA below "
                                     f"{IMAGE_LIMIT:#x} (a VA?)"))
            continue
        out.append((rva, m.group(2).strip()))
    if skipped:
        for n, line, why in skipped[:10]:
            sys.stderr.write(f"{path}:{n}: {why}: {line}\n")
        print(f"warning: {len(skipped)} line(s) of {path} were skipped", file=sys.stderr)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--classmap", default=CLASSMAP)
    ap.add_argument("--extra", default=os.path.join(os.path.dirname(CLASSMAP), "sites_extra.txt"))
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--probe", nargs="*", type=lambda v: int(v, 16), default=[])
    args = ap.parse_args()

    rows = parse_extra(args.extra) + parse_classmap(args.classmap)
    seen = {}
    for rva, name in rows:                      # extra entries win
        seen.setdefault(rva, name)
    table = sorted(seen.items())
    header = ["# nioh3p call-site table: <rva in nioh.exe> <name>, written by tools/sites.py",
              f"# {len(table)} entries from {os.path.basename(args.classmap)}"
              + (f" + {args.extra}" if os.path.exists(args.extra or '') else ""),
              "# the proxy loads this from the same directory as dinput8.dll"]
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(header) + "\n")
        for rva, name in table:
            fh.write(f"0x{rva:x} {name}\n")
    print(f"wrote {args.out}: {len(table)} entries, {os.path.getsize(args.out)} bytes")

    for probe in args.probe:
        best = None
        for rva, name in table:
            if rva <= probe and (best is None or rva >= best[0]):
                best = (rva, name)
        if best and probe == best[0]:
            print(f"  probe 0x{probe:x} -> {best[1]} (exact)")
        elif best and probe - best[0] < 0x8000:
            # these names are the *nearest* symbol at or below the probe: the
            # proxy attributes a call site this way, so say so instead of
            # presenting the name as the site's own
            print(f"  probe 0x{probe:x} -> {best[1]}+0x{probe - best[0]:x} "
                  "(nearest below, inexact)")
        else:
            print(f"  probe 0x{probe:x} -> (no symbol within 32 KB)")


if __name__ == "__main__":
    main()
