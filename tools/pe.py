"""Minimal PE64 reader (no external deps): sections, RVA<->file offset, imports, exports."""
import struct


class PE:
    def __init__(self, path, memory_image=False):
        """memory_image=True: `path` is a runtime (decrypted) module image, so
        RVAs map 1:1 to file offsets instead of section raw pointers."""
        self.path = path
        self.memory_image = memory_image
        self.d = open(path, 'rb').read()
        d = self.d
        # Length first: struct.unpack_from on a short file reports the buffer
        # size, which says nothing about the input. A truncated dump whose first
        # two bytes happen to be "MZ" reaches here from analyze.load().
        if len(d) < 0x40:
            raise ValueError(f'{path}: not a PE ({len(d)} B, shorter than the '
                             'DOS header)')
        self.e = struct.unpack_from('<I', d, 0x3c)[0]
        e = self.e
        if e + 24 > len(d):
            raise ValueError(f'{path}: not a PE (e_lfanew {e:#x} is past the end '
                             f'of a {len(d)} B file)')
        if d[e:e + 4] != b'PE\0\0':
            raise ValueError(f'{path}: not a PE (no PE signature at {e:#x})')
        self.nsec = struct.unpack_from('<H', d, e + 6)[0]
        self.optsize = struct.unpack_from('<H', d, e + 20)[0]
        self.magic = struct.unpack_from('<H', d, e + 24)[0]
        # image_base is read as a qword at optional-header +0x18, which only
        # holds for PE32+; a PE32 image would silently give a garbage base and
        # an empty export table, so refuse anything else up front.
        if self.magic != 0x20B:
            raise ValueError(f'{path}: unsupported optional header magic '
                             f'{self.magic:#06x} (this reader handles PE32+ 0x20b)')
        self.image_base = struct.unpack_from('<Q', d, e + 24 + 24)[0]
        self.secs = []
        so = e + 24 + self.optsize
        if so + self.nsec * 40 > len(d):
            raise ValueError(f'{path}: section table does not fit the file '
                             f'({self.nsec} sections)')
        for i in range(self.nsec):
            s = d[so + i * 40: so + (i + 1) * 40]
            name = s[:8].rstrip(b'\0').decode('latin1')
            vs, va, rs, ra = struct.unpack_from('<IIII', s, 8)
            chars = struct.unpack_from('<I', s, 36)[0]
            self.secs.append((name, va, vs, ra, rs, chars))

    # ---- addressing ----
    def r2o(self, rva):
        if self.memory_image:
            return rva if 0 <= rva < len(self.d) else None
        for _, va, vs, ra, rs, _c in self.secs:
            if va <= rva < va + max(vs, rs):
                off = ra + (rva - va)
                return off if off < len(self.d) else None
        return None

    def o2r(self, off):
        for _, va, vs, ra, rs, _c in self.secs:
            if ra <= off < ra + rs:
                return va + (off - ra)
        return None

    def section_of(self, rva):
        for n, va, vs, ra, rs, c in self.secs:
            if va <= rva < va + max(vs, rs):
                return n
        return None

    def dd(self, index):
        """(rva, size) of data directory `index`."""
        # The directory array lives in the optional header, whose declared size
        # is what bounds it: reading past it picks up unrelated header bytes.
        if 112 + index * 8 + 8 > self.optsize:
            raise ValueError(f'{self.path}: no data directory {index} '
                             f'(optional header is {self.optsize:#x} B)')
        base = self.e + 24 + 112 + index * 8
        if base + 8 > len(self.d):
            raise ValueError(f'{self.path}: data directory {index} is past end of file')
        return struct.unpack_from('<II', self.d, base)

    def _u(self, rva, fmt):
        """unpack `fmt` at `rva`, refusing unmapped/truncated reads."""
        o = self.r2o(rva)
        n = struct.calcsize(fmt)
        if o is None or o + n > len(self.d):
            raise ValueError(f'{self.path}: cannot read {fmt} ({n} B) at RVA {rva:#x}')
        return struct.unpack_from(fmt, self.d, o)

    def cstr(self, rva):
        o = self.r2o(rva)
        if o is None:
            raise ValueError(f'{self.path}: string RVA {rva:#x} is not in any section')
        end = self.d.find(b'\0', o)
        return self.d[o:end if end >= 0 else len(self.d)].decode('latin1')

    # ---- tables ----
    def imports(self):
        """{dll: [(name_or_ordinal, iat_rva, hint_name_rva)]}"""
        out = {}
        rva, _ = self.dd(1)
        if not rva:
            return out
        o = self.r2o(rva)
        if o is None:
            raise ValueError(f'{self.path}: import directory RVA {rva:#x} is not mapped')
        while o + 20 <= len(self.d):
            ent = self.d[o:o + 20]
            if ent == b'\0' * 20:
                break
            oft, ts, fc, nrva, ft = struct.unpack('<IIIII', ent)
            dll = self.cstr(nrva)
            syms = []
            t = oft or ft
            iat = ft
            k = 0
            while True:
                v = self._u(t + 8 * k, '<Q')[0]
                if v == 0:
                    break
                if v >> 63:
                    syms.append(('#%d' % (v & 0xffff), iat + 8 * k, None))
                else:
                    syms.append((self.cstr(v + 2), iat + 8 * k, v))
                k += 1
            out[dll] = syms
            o += 20
        return out

    def iat_index(self):
        """{symbol_name: iat_rva} across all imported DLLs."""
        idx = {}
        for dll, syms in self.imports().items():
            for name, iat, _ in syms:
                idx[name] = iat
        return idx

    def exports(self):
        """{name: rva} ordered by the export address table.

        Ordinal-only exports (an address with no name pointer) are keyed
        '#<ordinal>'. A forwarder is mapped to '->DLL.Function' instead of an
        address: its Function RVA points at a string inside the export
        directory, not at code (Wine's kernel32.dll forwards 101 of its 1361
        exports that way, e.g. NTDLL.RtlAcquireSRWLockExclusive)."""
        out = {}
        rva, size = self.dd(0)
        if not rva:
            return out
        base, nfun, nname, aof, anf, aaf = self._u(rva + 16, '<IIIIII')
        named = {}
        for i in range(nname):
            nr = self._u(anf + 4 * i, '<I')[0]
            orr = self._u(aaf + 2 * i, '<H')[0]
            named[orr] = self.cstr(nr)
        for i in range(nfun):
            fr = self._u(aof + 4 * i, '<I')[0]
            name = named[i] if i in named else '#%d' % (base + i)
            out[name] = '->' + self.cstr(fr) if rva <= fr < rva + size else fr
        return out

    def text(self):
        for n, va, vs, ra, rs, _c in self.secs:
            if n == '.text':
                return va, ra, rs
        raise KeyError('.text')
