#!/usr/bin/env python3
"""
Recursive-descent map of Popcorn's code segment.

The image is one data area (0x00000-0x1ac20) followed by one code segment
(`0x1ac2:0000`, image 0x1ac20-0x208b0 - 23,696 bytes).  Linear disassembly of it
desynchronises: the code is hand-written assembly with jump tables and
byte-sized data sitting between routines.  Following control flow instead finds
exactly the bytes that execute, and everything it does not reach is either data
or reached indirectly - which is itself worth knowing.

Reports, in order: the routines found, what each calls, the interrupts and ports
it touches, and the byte ranges control flow never reached.

Usage:
    python analyze.py                    # the whole map
    python analyze.py --func 0x1ad33     # one routine, disassembled
    python analyze.py --gaps             # only the unreached ranges
    python analyze.py --xref 0x2d4f      # who touches a data address
"""
import argparse
import os
import struct
import sys
from collections import defaultdict

from capstone import Cs, CS_ARCH_X86, CS_MODE_16
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

from tools_dis import load_image, UNPACKED

CODE_SEG = 0x1AC2
CODE_BASE = CODE_SEG * 16

# Entry points control flow cannot reach from `main`: interrupt handlers the
# program installs, and routines only ever called through a pointer variable.
# Each is justified where it is used; an unjustified one hides a gap.
EXTRA_ENTRIES = {
    0x03E3: "int09_handler (installed by the DOS set-vector at startup)",
}

# Data locations worth naming as they show up in an operand.
DATA_NAMES = {}


class CodeMap:
    def __init__(self, img):
        self.img = img
        self.md = Cs(CS_ARCH_X86, CS_MODE_16)
        self.md.detail = True
        self.insns = {}            # offset in segment -> instruction
        self.funcs = {}            # entry offset -> set of block starts
        self.calls = defaultdict(set)     # caller entry -> callee offsets
        self.callers = defaultdict(set)   # callee -> caller entries
        self.ints = defaultdict(set)      # entry -> interrupt numbers
        self.ports = defaultdict(set)     # entry -> port numbers seen as imm
        self.mem = defaultdict(set)       # data offset -> entries touching it
        self.indirect = defaultdict(set)  # entry -> sites of indirect control
        self.seg_refs = set()

    def code_at(self, off, n=16):
        a = CODE_BASE + off
        return self.img[a:a + n]

    def decode(self, off):
        ins = self.insns.get(off)
        if ins is None:
            g = self.md.disasm(self.code_at(off, 16), off)
            ins = next(g, None)
            if ins is not None:
                self.insns[off] = ins
        return ins

    def walk(self, entry):
        """Trace one routine; returns the set of instruction offsets in it."""
        seen, work = set(), [entry]
        while work:
            off = work.pop()
            while True:
                if off in seen or not (0 <= off < len(self.img) - CODE_BASE):
                    break
                ins = self.decode(off)
                if ins is None:
                    break
                seen.add(off)
                m, ops = ins.mnemonic, ins.operands

                if m.startswith("int") and m != "into":
                    if ops and ops[0].type == X86_OP_IMM:
                        self.ints[entry].add(ops[0].imm)
                if m in ("in", "out"):
                    for o in ops:
                        if o.type == X86_OP_IMM:
                            self.ports[entry].add(o.imm)
                    if any(o.type == X86_OP_REG for o in ops):
                        self.ports[entry].add(-1)     # via DX; resolved by hand
                for o in ops:
                    if o.type == X86_OP_MEM and o.mem.base == 0 and o.mem.index == 0:
                        self.mem[o.mem.disp & 0xFFFF].add(entry)

                if m == "call":
                    if ops and ops[0].type == X86_OP_IMM:
                        t = ops[0].imm
                        self.calls[entry].add(t)
                        self.callers[t].add(entry)
                        work.append(t) if False else None
                    else:
                        self.indirect[entry].add(off)
                    off = off + ins.size
                    continue
                if m in ("jmp", "ljmp"):
                    if ops and ops[0].type == X86_OP_IMM:
                        off = ops[0].imm
                        continue
                    self.indirect[entry].add(off)
                    break
                if m.startswith("j") or m.startswith("loop"):
                    if ops and ops[0].type == X86_OP_IMM:
                        work.append(ops[0].imm)
                    off = off + ins.size
                    continue
                if m in ("ret", "retf", "iret", "iretd", "hlt"):
                    break
                off = off + ins.size
        return seen

    def build(self, entries):
        pending = list(entries)
        while pending:
            e = pending.pop()
            if e in self.funcs:
                continue
            self.funcs[e] = self.walk(e)
            for t in self.calls[e]:
                if t not in self.funcs:
                    pending.append(t)

    def covered(self):
        out = set()
        for e, body in self.funcs.items():
            for off in body:
                ins = self.insns.get(off)
                if ins:
                    out.update(range(off, off + ins.size))
        return out

    def gaps(self, limit=None):
        cov = self.covered()
        end = len(self.img) - CODE_BASE
        runs, start = [], None
        for i in range(end):
            if i in cov:
                if start is not None:
                    runs.append((start, i - start))
                    start = None
            elif start is None:
                start = i
        if start is not None:
            runs.append((start, end - start))
        return runs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=UNPACKED)
    ap.add_argument("--func", type=lambda s: int(s, 0))
    ap.add_argument("--gaps", action="store_true")
    ap.add_argument("--xref", type=lambda s: int(s, 0))
    ap.add_argument("--min-gap", type=int, default=8)
    a = ap.parse_args()

    img = load_image(a.exe)
    data = open(a.exe, "rb").read()
    entry_ip, entry_cs = struct.unpack_from("<HH", data, 0x14)
    assert entry_cs == CODE_SEG, f"entry segment {entry_cs:#06x} is not the code segment"

    cm = CodeMap(img)
    cm.build([entry_ip] + list(EXTRA_ENTRIES))

    if a.func is not None:
        body = cm.funcs.get(a.func) or cm.walk(a.func)
        for off in sorted(body):
            ins = cm.insns[off]
            print(f"{CODE_SEG:04x}:{off:04x}  {CODE_BASE + off:06x}  "
                  f"{ins.bytes.hex():<18s} {ins.mnemonic} {ins.op_str}")
        return

    if a.xref is not None:
        who = cm.mem.get(a.xref, set())
        print(f"data {a.xref:#06x} touched by {len(who)} routines: "
              + ", ".join(f"{x:#06x}" for x in sorted(who)))
        return

    if a.gaps:
        runs = [r for r in cm.gaps() if r[1] >= a.min_gap]
        total = sum(n for _, n in runs)
        print(f"{len(runs)} unreached runs >= {a.min_gap} bytes, {total} bytes")
        for off, n in runs:
            blob = img[CODE_BASE + off:CODE_BASE + off + min(n, 24)]
            print(f"  {CODE_SEG:04x}:{off:04x}  {CODE_BASE + off:06x}  "
                  f"{n:5d} bytes  {blob.hex(' ')}")
        return

    cov = cm.covered()
    size = len(img) - CODE_BASE
    print(f"code segment {CODE_SEG:04x} = image {CODE_BASE:#07x}-"
          f"{CODE_BASE + size:#07x} ({size} bytes)")
    print(f"{len(cm.funcs)} routines reached, {len(cov)} of {size} bytes "
          f"({100.0 * len(cov) / size:.1f}%)\n")
    for e in sorted(cm.funcs):
        body = cm.funcs[e]
        nbytes = sum(cm.insns[o].size for o in body if o in cm.insns)
        bits = []
        if cm.ints[e]:
            bits.append("int " + ",".join(f"{i:02x}h" for i in sorted(cm.ints[e])))
        if cm.ports[e]:
            p = sorted(cm.ports[e])
            bits.append("port " + ",".join("dx" if x < 0 else f"{x:#x}" for x in p))
        if cm.indirect[e]:
            bits.append(f"{len(cm.indirect[e])} indirect")
        note = EXTRA_ENTRIES.get(e, "")
        print(f"  {e:04x}  {nbytes:5d} b  "
              f"calls {len(cm.calls[e]):2d}  called by {len(cm.callers[e]):2d}"
              f"  {'  '.join(bits)}  {note}")


if __name__ == "__main__":
    main()
