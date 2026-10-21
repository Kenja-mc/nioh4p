"""Map MSVC RTTI type descriptors -> vtables -> methods in a (dumped) module image.

Nioh's .text is encrypted on disk, but RTTI and the vtables live in .rdata/.data,
which are not. Combined with the runtime image dump this gives a class map of the
online/menu code without needing symbols.

IMAGE_RTTI_COMPLETE_OBJECT_LOCATOR (x64, .rdata):
    +0x00 signature (1)
    +0x04 offset
    +0x08 cdOffset
    +0x0c pTypeDescriptor   (RVA)
    +0x10 pClassDescriptor  (RVA)
    +0x14 pSelf             (RVA, points back at the locator)
A vtable's slot -1 (i.e. the qword right before the vtable) holds the locator RVA.
"""
import re
import struct
import sys
import collections

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from pe import PE  # noqa: E402

RSLASH = re.compile(rb'\.\?A[VE][^\x00]{0,200}@@')


def locators(pe):
    """[(locator_rva, type_name)] for every complete object locator."""
    out = []
    for sec in ('.rdata', '.data'):
        try:
            base = next(va for n, va, vs, ra, rs, c in pe.secs if n == sec)
            size = next(vs for n, va, vs, ra, rs, c in pe.secs if n == sec)
        except StopIteration:
            continue
        for off in range(0, size - 20, 4):
            rva = base + off
            o = pe.r2o(rva)
            if o is None:
                continue
            sig, _ofs, _cd, ptype, pclass, pself = struct.unpack_from('<IIIIII', pe.d, o)
            if sig != 1 or pself != rva or ptype == 0:
                continue
            to = pe.r2o(ptype)
            if to is None:
                continue
            m = RSLASH.match(pe.d, to + 0x10)
            if not m:
                continue
            out.append((rva, m.group(0)[4:-2].decode('latin1')))
    return out


def vtables(pe, loc_rvas):
    """{locator_rva: [vtable_rva, ...]} - vtable[-1] == locator rva."""
    want = set(loc_rvas)
    found = collections.defaultdict(list)
    for sec in ('.rdata', '.data'):
        try:
            base = next(va for n, va, vs, ra, rs, c in pe.secs if n == sec)
            size = next(vs for n, va, vs, ra, rs, c in pe.secs if n == sec)
        except StopIteration:
            continue
        for off in range(0, size - 8, 8):
            rva = base + off
            o = pe.r2o(rva)
            if o is None:
                continue
            v = struct.unpack_from('<Q', pe.d, o)[0]
            r = v - pe.image_base
            if r in want:
                found[r].append(rva + 8)
    return found


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else \
        '/tmp/nioh3p-smoke/game/nioh3p_image_settled.bin'
    pe = PE(path, memory_image=True)
    locs = locators(pe)
    by_rva = dict(locs)
    vt = vtables(pe, by_rva.keys())
    print(f'type descriptors with locators: {len(locs)}, with vtable: {len(vt)}')
    pat = sys.argv[2] if len(sys.argv) > 2 else None
    for rva, name in sorted(locs, key=lambda x: x[1]):
        if pat and not re.search(pat, name, re.I):
            continue
        tabs = vt.get(rva, [])
        print(f'\n{name}')
        for t in tabs:
            print(f'   vtable {t:#x}')
            for i in range(12):
                o = pe.r2o(t + i * 8)
                if o is None:
                    break
                fn = struct.unpack_from('<Q', pe.d, o)[0]
                if fn == 0:
                    break
                print(f'      [{i}] {fn:#x}')


if __name__ == '__main__':
    main()
