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
        self.e = struct.unpack_from('<I', d, 0x3c)[0]
        e = self.e
        assert d[e:e + 4] == b'PE\0\0', 'not a PE'
        self.nsec = struct.unpack_from('<H', d, e + 6)[0]
        self.optsize = struct.unpack_from('<H', d, e + 20)[0]
        self.magic = struct.unpack_from('<H', d, e + 24)[0]
        self.image_base = struct.unpack_from('<Q', d, e + 24 + 24)[0]
        self.secs = []
        so = e + 24 + self.optsize
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
        base = self.e + 24 + 112
        return struct.unpack_from('<II', self.d, base + index * 8)

    def cstr(self, rva):
        o = self.r2o(rva)
        return self.d[o:self.d.index(b'\0', o)].decode('latin1')

    # ---- tables ----
    def imports(self):
        """{dll: [(name_or_ordinal, iat_rva, hint_name_rva)]}"""
        out = {}
        rva, _ = self.dd(1)
        o = self.r2o(rva)
        while True:
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
                v = struct.unpack_from('<Q', self.d, self.r2o(t + 8 * k))[0]
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
        out = {}
        rva, size = self.dd(0)
        if not rva:
            return out
        o = self.r2o(rva)
        base = struct.unpack_from('<I', self.d, o + 16)[0]
        nfun = struct.unpack_from('<I', self.d, o + 20)[0]
        nname = struct.unpack_from('<I', self.d, o + 24)[0]
        aof = struct.unpack_from('<I', self.d, o + 28)[0]
        anf = struct.unpack_from('<I', self.d, o + 32)[0]
        aaf = struct.unpack_from('<I', self.d, o + 36)[0]
        for i in range(nname):
            nr = struct.unpack_from('<I', self.d, self.r2o(anf + 4 * i))[0]
            orr = struct.unpack_from('<H', self.d, self.r2o(aaf + 2 * i))[0]
            fr = struct.unpack_from('<I', self.d, self.r2o(aof + 4 * orr))[0]
            out[self.cstr(nr)] = fr
        return out

    def text(self):
        for n, va, vs, ra, rs, _c in self.secs:
            if n == '.text':
                return va, ra, rs
        raise KeyError('.text')
