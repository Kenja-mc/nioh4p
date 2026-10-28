"""Recursive-ish disassembly of a PE64 .text using .pdata function boundaries (capstone).

Builds an instruction index so we can answer: which function touches IAT slot X,
which function references VA Y, what is the call graph around a site.

The input is a *runtime image*: the module mapped at its load address with .text
decrypted - one of the proxy's `dump_every_s` snapshots, or a merge of them made
by tools/mergedump.py.  It defaults to the merged dump ($NIOH_DUMP); pass a path
to analyse another one.  nioh.exe on disk is a valid PE64 whose headers, .pdata,
.rdata and .data are readable, but whose .text is encrypted at rest, so
disassembling it decodes noise and finds no reference to any import.  Such an
image is refused and a partially decrypted one is labelled, so an empty
`EOS references` section can never be read as a true negative.
"""
import bisect
import os
import struct
import sys

# the repo's own tools/ first: `from pe import PE` must never pick up another
# `pe` module from a directory appended below
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# capstone lives outside the venv in this checkout; appended, never prepended, so
# it cannot shadow the repo's tools/pe.py
sys.path.append(os.path.expanduser('~/.local/lib/pylibs'))
from pe import PE  # noqa: E402
import capstone  # noqa: E402

GAME = os.environ.get('NIOH_DIR') or os.path.expanduser(
    '~/.local/share/Steam/steamapps/common/Nioh')
EXE = os.path.join(GAME, 'nioh.exe')
DUMP = os.environ.get('NIOH_DUMP') or os.path.expanduser(
    '~/.cache/nioh3p/dumps/nioh3p_image_merged.bin')
PAGE = 0x1000
SWEEP_STOPS = 10


class Dis:
    def __init__(self, pe):
        self.pe = pe
        self.d = pe.d
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True
        self.funcs = self._pdata()
        self.insns = {}
        self.short = []                     # .pdata functions whose sweep ended early
        self.begins = [b for (b, _e) in self.funcs]

    def _pdata(self):
        rva, size = self.pe.dd(3)
        out = []
        if rva:
            o = self.pe.r2o(rva)
            for i in range(size // 12):
                b, e, u = struct.unpack_from('<III', self.d, o + i * 12)
                if b:
                    out.append((b, e))
        return sorted(out)

    def disasm_all(self):
        """Decode every .pdata function; record the sweeps that stop early.

        capstone ends a linear sweep at the first byte it cannot decode, so a
        function with embedded data - jump tables, alignment padding - loses
        everything after that byte.  The stops go into self.short (reported by
        the caller) instead of leaving the sweep looking complete.
        """
        md = self.md
        d = self.d
        base = self.pe.image_base
        for (b, e) in self.funcs:
            o = self.pe.r2o(b)
            if o is None:
                continue
            blob = d[o:o + (e - b)]
            last = b
            for ins in md.disasm(blob, base + b):
                self.insns[ins.address] = ins
                last = ins.address + ins.size - base        # RVA past this insn
            if last < e:
                k = o + (last - b)
                self.short.append((b, e, last, d[k] if k < len(d) else 0))
        return self.insns

    def func_range(self, va):
        """(begin_rva, end_rva) of the .pdata function containing `va`, or None.

        The 72,232 .pdata entries of the shipped build are sorted by begin and do
        not overlap, so bisecting the begins is exact.
        """
        rva = va - self.pe.image_base
        i = bisect.bisect_right(self.begins, rva) - 1
        if i >= 0 and rva < self.funcs[i][1]:
            return self.funcs[i]
        return None

    def func_of(self, va):
        """VA of the function containing `va` (PE .pdata entries are RVAs)."""
        r = self.func_range(va)
        return self.pe.image_base + r[0] if r else None

    def refs_to_va(self, target_va):
        """Instructions whose rip-relative memory operand or immediate equals target_va."""
        out = []
        for va, ins in self.insns.items():
            hit = False
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    if va + ins.size + op.mem.disp == target_va:
                        hit = True
                elif op.type in (capstone.x86.X86_OP_IMM,) and op.imm == target_va:
                    hit = True
            if hit:
                out.append(ins)
        return sorted(out, key=lambda i: i.address)

    def refs_to_range(self, lo, hi):
        out = []
        for va, ins in self.insns.items():
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    t = va + ins.size + op.mem.disp
                    if lo <= t < hi:
                        out.append((t, ins))
        return sorted(out, key=lambda x: x[1].address)

    def calls_to_iat(self, iat_va):
        """call [rip+disp] / jmp [rip+disp] / mov reg,[rip+disp] targeting iat_va."""
        return self.refs_to_range(iat_va, iat_va + 1)

    def disasm_range(self, start_va, length):
        """Disassemble `length` bytes at `start_va` (a hint, not a hard window)."""
        o = self.pe.r2o(start_va - self.pe.image_base)
        if o is None:      # used to slice None and raise a bare TypeError
            raise ValueError(f'{self.pe.path}: VA {start_va:#x} is not in any '
                             'section of this image')
        return list(self.md.disasm(self.d[o:o + length], start_va))

    def calls_in(self, func_va):
        """Outgoing edges from a function: (target_va, ins, kind).

        Kind is 'call', 'jmp' or 'call/mem' for indirect call sites.  The window is
        the function's own .pdata [begin, end): the old
        `r <= va - image_base < r + 0x100000` compared the *VA* func_of() returns
        against an RVA, so every call returned [] - and the 1 MB window it meant to
        use would have merged the edges of ~19,952 neighbouring functions in.
        """
        r = self.func_range(func_va)
        if r is None:
            return []
        b, e = r
        outs = []
        for va, ins in self.insns.items():
            if not (b <= va - self.pe.image_base < e):
                continue
            if ins.mnemonic in ('call', 'jmp'):
                for op in ins.operands:
                    if op.type == capstone.x86.X86_OP_IMM:
                        outs.append((op.imm, ins, ins.mnemonic))
                    elif op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                        outs.append((va + ins.size + op.mem.disp, ins, ins.mnemonic + '/mem'))
        return sorted(outs, key=lambda x: x[1].address)


def die(msg, usage=False):
    sys.stderr.write(f'analyze.py: {msg}\n')
    if usage:
        sys.stderr.write('usage: tools/analyze.py [IMAGE]\n'
                         f'   IMAGE  runtime image to analyse (default: {DUMP})\n')
    sys.exit(2)


def named(image, exc):
    """pe.py's messages already start with the path; don't print it twice."""
    msg = str(exc)
    return msg if msg.startswith(f'{image}:') else f'{image}: {msg}'


def on_disk_file(path):
    """True when `path` holds the shipped module file itself (byte-identical).

    Identity rather than just the path: the on-disk file is opened with
    memory_image=True like a dump, which reads the headers and the section table
    where the code should be, and the .text-vs-disk comparison below would then
    find every page "decrypted".  Copies are caught too, hence the bytes.
    """
    try:
        a, b = open(path, 'rb'), open(EXE, 'rb')
    except OSError:
        return False
    with a, b:
        if os.fstat(a.fileno()).st_size != os.fstat(b.fileno()).st_size:
            return False
        return a.read() == b.read()


def text_coverage(pe):
    """(#.text pages of `pe` that differ from the on-disk bytes, total), or None.

    The comparison tools/mergedump.py uses to pick the pages a run had decrypted;
    here it is the guard that no image is analysed as if all its code were
    readable.  None when the on-disk module cannot be read (no game installed).
    """
    try:
        disk = PE(EXE)
        iva, _ira, irs = pe.text()
        _dva, dra, drs = disk.text()
    except (OSError, ValueError, KeyError):
        return None
    n = min(irs, drs, len(pe.d) - iva, len(disk.d) - dra)
    if n <= 0:
        return None
    total = diff = 0
    for o in range(0, n, PAGE):
        total += 1
        if pe.d[iva + o:iva + o + PAGE] != disk.d[dra + o:dra + o + PAGE]:
            diff += 1
    return diff, total


def load(image=None):
    """PE for a runtime image (default: the merged dump, $NIOH_DUMP).

    These dumps are the module mapped at its load address, so file offset == RVA
    and they are opened with memory_image=True - the section table they carry is
    the on-disk one, whose raw pointers do not address them (tools/ktol_refs.py
    addresses them the same way). A merged dump written without its first page is
    headerless but equally flat, so its header page is spliced in from the
    on-disk nioh.exe of the same build.
    """
    image = image or DUMP
    try:
        with open(image, 'rb') as fh:
            has_header = fh.read(2) == b'MZ'
            size = fh.seek(0, 2)
    except OSError as exc:
        die(f'{image}: {exc}')
    if has_header:
        try:
            pe = PE(image, memory_image=True)
        except (ValueError, OSError) as exc:
            die(named(image, exc))
    else:
        try:
            pe = PE(EXE, memory_image=True)
        except (ValueError, OSError) as exc:
            die(f'{image} has no PE header and the build layout is unavailable '
                f'({named(EXE, exc)})')
        # A headerless dump zeroes the first page, which is where the PE header
        # lives (it is not encrypted, so a snapshot never holds it). Splicing the
        # on-disk one in makes e_lfanew, the optional header, the data
        # directories and the section table the build's own - and any of them
        # read later (pe.dd() among them) still lands in the dump.
        hdr_end = pe.e + 24 + pe.optsize + pe.nsec * 40
        with open(image, 'rb') as fh:
            body = fh.read()
        with open(EXE, 'rb') as fh:
            header = fh.read(hdr_end)
        pe.d = header + body[len(header):]
        pe.memory_image = True
        pe.path = f'{image} (headerless flat image, header from {EXE})'
    need = max(va + max(vs, rs) for _n, va, vs, _ra, rs, _c in pe.secs)
    if size < need:
        die(f'{image} is {size} B but the image of this build is {need} B: '
            'truncated or unrelated file')
    return pe


def report_sweep(dis):
    """Report the functions whose linear sweep ended before their .pdata end."""
    if not dis.short:
        return
    base = dis.pe.image_base
    lost = sum(e - stop for _b, e, stop, _x in dis.short)
    print(f'.pdata sweep: {len(dis.short)} of {len(dis.funcs)} functions end early '
          f'({lost} B of their code not decoded)')
    for b, e, stop, byte in dis.short[:SWEEP_STOPS]:
        print(f'   function {base + b:#x}-{base + e:#x} stopped at '
              f'{base + stop:#x} (byte {byte:#04x})')
    if len(dis.short) > SWEEP_STOPS:
        print(f'   ... and {len(dis.short) - SWEEP_STOPS} more')


def check_pdata(pe, dis):
    """Refuse .pdata entries that are not inside .text.

    .pdata is copied from the on-disk file verbatim, so it stays valid even in a
    partly decrypted image; entries outside .text therefore mean the input is not
    an image of the build whose layout was used.
    """
    va, _ra, rs = pe.text()
    lo, hi = va, va + rs
    bad = [f for f in dis.funcs if not (lo <= f[0] < f[1] <= hi)]
    if bad:
        die(f'{pe.path}: {len(bad)} of {len(dis.funcs)} .pdata entries are not '
            f'inside .text ({lo:#x}-{hi:#x}), e.g. {bad[0][0]:#x}-{bad[0][1]:#x}: '
            'not an image of this build')


if __name__ == '__main__':
    argv = sys.argv[1:]
    if len(argv) > 1:
        die('at most one argument is expected', usage=True)
    image = argv[0] if argv else DUMP
    if on_disk_file(image):
        die(f'{image} is the on-disk module file (byte-identical to {EXE}): its '
            '.text is encrypted at rest, so every instruction decoded from it is '
            'noise.  Analyse a runtime image instead - the proxy\'s dump_every_s '
            'snapshots, or a merge of them made by tools/mergedump.py')
    pe = load(image)
    cov = text_coverage(pe)
    if cov is not None and cov[0] == 0:
        die(f'{image}: 0 of {cov[1]} .text pages differ from {EXE}, so this image '
            'holds the encrypted-at-rest code and anything decoded from it is noise')
    try:
        dis = Dis(pe)
    except ValueError as exc:
        die(named(image, exc))
    dis.disasm_all()
    print('pdata functions:', len(dis.funcs), 'instructions:', len(dis.insns))
    if cov is None:
        print(f'.text: cannot check decryption ({EXE} not readable)')
    elif cov[0] < cov[1]:
        print(f'.text: {cov[0]}/{cov[1]} pages differ from the on-disk nioh.exe - '
              f'the other {cov[1] - cov[0]} still hold the encrypted-at-rest bytes, '
              'so instructions there are noise and a missing reference is not a '
              'negative')
    else:
        print(f'.text: {cov[0]}/{cov[1]} pages differ from the on-disk nioh.exe '
              '(fully decrypted)')
    check_pdata(pe, dis)
    report_sweep(dis)
    idx = pe.iat_index()
    targets = [k for k in idx if k.startswith('EOS_')]
    print('=== EOS references ===')
    found = False
    for name in targets:
        r = dis.calls_to_iat(pe.image_base + idx[name])
        if r:
            found = True
            print(f'{name:38s} {len(r):3d} refs: ' + ' '.join(hex(i.address) for _, i in r[:8]))
    if not found:
        if targets:
            print(f'no reference to any of the {len(targets)} EOS_* imports was '
                  'found in the decoded .text (see the lines above)')
        else:
            print(f"no EOS_* import among this image's {len(idx)} import slots")
