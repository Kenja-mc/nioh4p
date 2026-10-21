"""Recursive-ish disassembly of a PE64 .text using .pdata function boundaries (capstone).

Builds an instruction index so we can answer: which function touches IAT slot X,
which function references VA Y, what is the call graph around a site.
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
sys.path.insert(0, '/home/<user>/.local/lib/pylibs')
from pe import PE  # noqa: E402
import capstone  # noqa: E402


class Dis:
    def __init__(self, pe):
        self.pe = pe
        self.d = pe.d
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True
        self.funcs = self._pdata()
        self.insns = {}

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
        md = self.md
        d = self.d
        for (b, e) in self.funcs:
            o = self.pe.r2o(b)
            if o is None:
                continue
            blob = d[o:o + (e - b)]
            for ins in md.disasm(blob, self.pe.image_base + b):
                self.insns[ins.address] = ins
        return self.insns

    def func_of(self, va):
        """VA of the function containing `va` (PE .pdata entries are RVAs)."""
        for (b, e) in self.funcs:
            if b <= va - self.pe.image_base < e:
                return self.pe.image_base + b
        return None

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
        o = self.pe.r2o(start_va - self.pe.image_base)
        blob = self.d[o:o + length]
        return list(self.md.disasm(blob, start_va))

    def calls_in(self, func_va):
        """Outgoing edges from a function: (target_va, ins, kind)."""
        r = self.func_of(func_va)
        if r is None:
            return []
        outs = []
        for va, ins in self.insns.items():
            if not (r <= va - self.pe.image_base < r + 0x100000):
                continue
            if ins.mnemonic in ('call', 'jmp'):
                for op in ins.operands:
                    if op.type == capstone.x86.X86_OP_IMM:
                        outs.append((op.imm, ins, ins.mnemonic))
                    elif op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                        outs.append((va + ins.size + op.mem.disp, ins, ins.mnemonic + '/mem'))
        return sorted(outs, key=lambda x: x[1].address)


def load(exe='/home/<user>/.local/share/Steam/steamapps/common/Nioh/nioh.exe'):
    pe = PE(exe)
    dis = Dis(pe)
    return pe, dis


if __name__ == '__main__':
    pe, dis = load()
    dis.disasm_all()
    print('pdata functions:', len(dis.funcs), 'instructions:', len(dis.insns))
    idx = pe.iat_index()
    rev = {v: k for k, v in idx.items()}
    targets = [k for k in idx if k.startswith('EOS_')]
    print('=== EOS references ===')
    for name in targets:
        iat = pe.image_base + idx[name]
        r = dis.calls_to_iat(iat)
        if r:
            print(f'{name:38s} {len(r):3d} refs: ' + ' '.join(hex(i.address) for _, i in r[:8]))
