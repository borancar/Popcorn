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
import re
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
CODE_LEN = 0x5E90               # the code segment; past it is data

EXTRA_ENTRIES = {
    0x03E3: "int09_handler (installed by the DOS set-vector at startup)",
    # The two input routines, selected into [0x2d45]/[0x2d47] by F3/F4 and
    # called through that pointer from the play loop.
    0x1654: "input_mouse (INT 33h; paddle = clamp(mouse x / 2))",
    0x16D2: "input_keyboard (steps the paddle one pixel per repeat tick)",
    # The third one. demo_start stores it in [0x2d45] the way F1 stores
    # 0x1654 or 0x16d2, and the play loop calls whatever is there - so
    # nothing that follows control flow reaches it either.
    0x1785: "input_demo (chases the ball, and any key ends the demo)",
    # Cells 16-21 of the brick table all come here - the special
    # bricks that belong to a larger animation.
    0x2CCD: "brick: cells 16-21, the animated ones",
    # The entity 0x2ccd installs: the animation that keeps running.
    0x3ABF: "entity handler for an animated brick",
    # Entity handlers. The play loop at 0x1873 walks a linked list from the
    # head link at 0x3144 and calls each node's `+0x00` - so none of these is
    # reachable by following control flow, and all of them are the game.
    # Collected by walking that list while the game played; the node pool is
    # at 0x3146, stride 0x0e, and `+0x0c` is the next link with 0xffff as the
    # terminator.
    0x3273: "entity handler (seen every frame in play)",
    0x3386: "entity handler",
    0x3561: "entity handler",
    0x3717: "entity handler",
    0x390D: "entity handler",
    0x39FA: "entity handler",
    0x3AEE: "entity handler",
    0x3B2A: "entity handler",
    # Brick behaviour, from the table at 0x3044 indexed by cell value. Read out
    # of the image rather than followed, because the call is `call word ptr
    # [bx]` with bx computed from the cell.
    0x28CB: "brick: ordinary (cell 1)",
    0x2985: "brick: cell 2",
    0x2A3F: "brick: cell 3",
    0x3221: "brick: cells 4 and 12 - does nothing",
    0x2A73: "brick: cell 5",
    0x2AB4: "brick: cell 6",
    0x2AF5: "brick: cell 7",
    0x2B36: "brick: cell 8",
    0x2B9D: "brick: cell 9",
    0x2C59: "brick: cell 10",
    0x2D68: "brick: cell 11",
    # How a falling bonus moves, from the table at 0x3447 indexed by [bx+2].
    # The rest of the entity handlers, found by collecting every
    # `mov word ptr [si], imm` / `mov word ptr [bx], imm` that installs one -
    # an entity's kind is its handler, and handlers install each other.
    0x365E: "entity handler (from brick 3)",
    0x366F: "entity handler (from brick 8)",
    0x3696: "entity handler (from brick 9)",
    0x36A1: "entity handler (from brick 9)",
    0x36F6: "entity handler (from brick 9)",
    0x37E0: "entity handler (from brick 10)",
    # What a bonus actually does, from the table at 0x33bc indexed by the
    # capsule's kind. 0x318b (an extra life) and 0x3231 are already reached.
    0x2DAA: "bonus effect 0",
    0x2DEF: "bonus effect 1",
    0x2E03: "bonus effect 3",
    0x2E16: "bonus effect 4",
    0x3119: "bonus effect 5",
    0x315B: "bonus effect 6",
    0x2DA0: "bonus effect 8",
    0x31E8: "bonus effect 9",
    0x3231: "bonus effect 2 - does nothing",
    0x3200: "bonus effect 10 - the game slows down",
    0x3C66: "bonus movement 0",
    0x3D3C: "bonus movement 1",
    0x3CF3: "bonus movement 2",
    0x3CAF: "bonus movement 3",
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
    ap.add_argument("--tables", action="store_true",
                    help="check every dispatch table's targets are in the map")
    ap.add_argument("--listing", action="store_true",
                    help="dump every reachable routine, in address order")
    ap.add_argument("--xref", type=lambda s: int(s, 0))
    ap.add_argument("--min-gap", type=int, default=8)
    a = ap.parse_args()

    img = load_image(a.exe)
    data = open(a.exe, "rb").read()
    entry_ip, entry_cs = struct.unpack_from("<HH", data, 0x14)
    assert entry_cs == CODE_SEG, f"entry segment {entry_cs:#06x} is not the code segment"

    cm = CodeMap(img)
    cm.build([entry_ip] + list(EXTRA_ENTRIES))

    if a.tables:
        # The map is seeded by hand from tables the code dispatches through,
        # and a hand-written list can be short. It was: the bonus effect table
        # has twelve entries and only ten had been read, so the routine that
        # slows the game down was never mapped, never transcribed, and the
        # port silently did nothing when a player collected it. Walk each
        # table out of the image and say so if a target is not in the map.
        bad = 0
        # The counts are deliberately generous. A hand-written one was wrong
        # once already: the brick table was audited as fourteen entries and
        # has twenty-two, so cells 16 to 21 - which all dispatch to 0x2ccd -
        # were never mapped, never transcribed, and did nothing in the port.
        # A zero entry does not end a table either; 13, 14 and 15 are zero and
        # 16 is not.
        # Lengths established from the data, not guessed. The brick table
        # really is twenty-two: cells 16 to 21 all dispatch to 0x2ccd, which
        # is deliberate and was missed when this was audited as fourteen.
        # A zero entry does not end a table - 13, 14 and 15 are zero and 16 is
        # not - and past the end there is only data, so entries are also
        # required to point inside the code segment.
        for name, base, count in (
                ("brick behaviour", 0x3044, 22),
                ("bonus effect", 0x33BC, 12),
                ("bonus movement", 0x3447, 4)):
            for i in range(count):
                v = struct.unpack_from("<H", img, base + i * 2)[0]
                if v == 0 or not (0 < v < CODE_LEN):
                    continue
                if v in cm.insns:
                    continue
                print(f"  {name}[{i}] -> {v:#06x} is NOT in the map")
                bad += 1
        # Tables are not the only indirection. [0x2d45] is a single word the
        # play loop calls through - `call word ptr [0x2d45]` - and the menu
        # stores one of three routine addresses in it. Two were in the seed
        # list and the demo's was not, so it was never disassembled and the
        # demo had no way to move its paddle. Find every word the code calls
        # through, then every immediate stored into it.
        called_through = set()
        for off, ins in sorted(cm.insns.items()):
            text = f"{ins.mnemonic} {ins.op_str}"
            m = re.match(r"call\s+word ptr \[(0x[0-9a-f]+)\]$", text)
            if m:
                called_through.add(int(m.group(1), 16))
        for off, ins in sorted(cm.insns.items()):
            text = f"{ins.mnemonic} {ins.op_str}"
            m = re.match(r"mov\s+word ptr \[(0x[0-9a-f]+)\], (0x[0-9a-f]+)$",
                         text)
            if not m:
                continue
            var, val = int(m.group(1), 16), int(m.group(2), 16)
            if var not in called_through or val in cm.insns:
                continue
            print(f"  [{var:#06x}] <- {val:#06x} at {off:#06x} "
                  f"is NOT in the map")
            bad += 1

        print("every dispatch target is mapped" if not bad
              else f"{bad} dispatch targets are missing")
        return

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

    if a.listing:
        # The whole reachable code segment, routine by routine, in address
        # order. Written to a file rather than the terminal because the point
        # is to read it as a document while transcribing.
        for e in sorted(cm.funcs):
            body = sorted(cm.funcs[e])
            note = EXTRA_ENTRIES.get(e, "")
            callers = ", ".join(f"{c:04x}" for c in sorted(cm.callers[e])) or "-"
            print(f"\n;;; ---- {e:04x}  (image {CODE_BASE + e:#07x})  "
                  f"called by {callers}  {note}")
            for off in body:
                ins = cm.insns[off]
                print(f"{off:04x}  {ins.bytes.hex():<16s} "
                      f"{ins.mnemonic} {ins.op_str}")
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
