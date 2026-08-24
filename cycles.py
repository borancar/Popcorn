"""Measure what one play-loop frame costs the original, in 8086 cycles.

The port's frame rate had been calibrated by measuring the port, which only
says how fast its own sleeps run, and by adding up the delay loops, which
misses everything the delays do not cover. Neither is a measurement of the
game.

This is: run the original under the emulator, hook every instruction, and sum
the cycle costs from the iAPX 86/88 manual's table. Divide 8 MHz by the mean
and that is the rate the game ran at on the machine its readme names.

Two things it is honest about rather than quiet about:

  * Every mnemonic the table does not know is counted and reported, so the
    coverage of the estimate is visible instead of assumed.
  * The manual's numbers exclude instruction fetch. A real 8086 empties its
    prefetch queue on every branch and stalls, so the true frame is *longer*
    than this and the true rate *lower*. The figure here is an upper bound on
    the rate, and the report says by how much a given stall estimate moves it.

    venv/bin/python cycles.py --resume level10.snap --frames 200
"""
import argparse
import collections
import os
import sys

# --- the 8086 cycle table -------------------------------------------------
#
# Intel iAPX 86/88 User's Manual, the "Instruction Set Reference" timings.
# Where the manual gives a range (the multiplies and divides) the midpoint is
# used and the spread is reported separately, because on this workload they
# are rare enough not to matter and pretending otherwise would hide it.

def ea_cost(op):
    """Effective-address calculation, the manual's five cases."""
    m = op.mem
    base, index, disp = m.base, m.index, m.disp
    if base == 0 and index == 0:
        c = 6                                  # displacement only
    elif base != 0 and index != 0:
        # bp+di and bx+si are 7; bp+si and bx+di are 8
        from capstone.x86_const import X86_REG_BP, X86_REG_BX, X86_REG_SI, X86_REG_DI
        fast = (base == X86_REG_BP and index == X86_REG_DI) or \
               (base == X86_REG_BX and index == X86_REG_SI)
        c = (7 if fast else 8) + (2 if disp else 0)
    else:
        c = 5 + (4 if disp else 0)             # base or index, +disp
    return c

# reg,reg | reg,mem | mem,reg | reg,imm | mem,imm  (mem forms add EA)
ALU = (3, 9, 16, 4, 17)
TABLE_RMI = {
    "add": ALU, "adc": ALU, "sub": ALU, "sbb": ALU,
    "and": ALU, "or": ALU, "xor": ALU,
    "cmp": (3, 9, 9, 4, 10),                   # no write-back
    "test": (3, 9, 9, 5, 11),
    "mov": (2, 8, 9, 4, 10),
}
SIMPLE = {
    "nop": 3, "cbw": 2, "cwd": 5, "lahf": 4, "sahf": 4,
    "pushf": 10, "popf": 8, "cli": 2, "sti": 2, "cld": 2, "std": 2,
    "clc": 2, "stc": 2, "cmc": 2, "hlt": 2, "xlatb": 11, "xlat": 11,
    "daa": 4, "das": 4, "aaa": 4, "aas": 4, "aam": 83, "aad": 60,
    "into": 4, "iret": 32, "int3": 52, "wait": 3,
}
STRING_ONCE = {"movsb": 18, "movsw": 18, "cmpsb": 22, "cmpsw": 22,
               "scasb": 15, "scasw": 15, "lodsb": 12, "lodsw": 12,
               "stosb": 11, "stosw": 11}
STRING_REP = {"movsb": 17, "movsw": 17, "cmpsb": 22, "cmpsw": 22,
              "scasb": 15, "scasw": 15, "lodsb": 13, "lodsw": 13,
              "stosb": 10, "stosw": 10}
JCC = {"ja", "jae", "jb", "jbe", "jc", "je", "jg", "jge", "jl", "jle",
       "jna", "jnae", "jnb", "jnbe", "jnc", "jne", "jng", "jnge", "jnl",
       "jnle", "jno", "jnp", "jns", "jnz", "jo", "jp", "jpe", "jpo",
       "js", "jz"}
MULDIV = {                                     # (min, max) from the manual
    "mul8": (70, 77), "mul16": (118, 133),
    "imul8": (80, 98), "imul16": (128, 154),
    "div8": (80, 90), "div16": (144, 162),
    "idiv8": (101, 112), "idiv16": (165, 184),
}


class Model:
    """Costs one decoded instruction. Records what it could not cost."""

    def __init__(self, cx=None):
        self.unknown = collections.Counter()
        self.cx = cx                           # reads CX, for the `rep` count

    def cost(self, insn, taken, repeated):
        from capstone.x86_const import (X86_OP_REG, X86_OP_MEM, X86_OP_IMM,
                                        X86_PREFIX_REP, X86_PREFIX_REPNE)
        m = insn.mnemonic
        ops = insn.operands
        pfx = 2 if insn.prefix[1] else 0       # a segment override is 2 clocks

        # capstone folds a repeat prefix into the mnemonic rather than into
        # insn.prefix, so `rep movsw` arrives as one string.
        rep = False
        for r in ("rep ", "repe ", "repz ", "repne ", "repnz "):
            if m.startswith(r):
                rep, m = True, m[len(r):]
                break

        if m in SIMPLE:
            return SIMPLE[m] + pfx
        if m in STRING_ONCE:
            if not rep:
                return STRING_ONCE[m] + pfx
            # A repeated string op: 9 clocks of setup, then the per-iteration
            # cost times CX. Unicorn hooks the whole `rep` once, not once an
            # iteration, so the count has to be read out of CX - which is why
            # the model is handed the machine.
            n = self.cx() if self.cx else 1
            return 9 + STRING_REP[m] * max(n, 1) + pfx

        if m in TABLE_RMI:
            rr, rm, mr, ri, mi = TABLE_RMI[m]
            if len(ops) == 2:
                d, s = ops
                if d.type == X86_OP_REG and s.type == X86_OP_REG:
                    return rr + pfx
                if d.type == X86_OP_REG and s.type == X86_OP_MEM:
                    return rm + ea_cost(s) + pfx
                if d.type == X86_OP_MEM and s.type == X86_OP_REG:
                    return mr + ea_cost(d) + pfx
                if d.type == X86_OP_REG and s.type == X86_OP_IMM:
                    return ri + pfx
                if d.type == X86_OP_MEM and s.type == X86_OP_IMM:
                    return mi + ea_cost(d) + pfx
            self.unknown[m + "/odd"] += 1
            return rr + pfx

        if m in ("inc", "dec"):
            o = ops[0]
            if o.type == X86_OP_REG:
                return (2 if o.size == 2 else 3) + pfx
            return 15 + ea_cost(o) + pfx
        if m in ("neg", "not"):
            o = ops[0]
            return (3 if o.type == X86_OP_REG else 16 + ea_cost(o)) + pfx
        if m == "push":
            o = ops[0]
            if o.type == X86_OP_MEM:
                return 16 + ea_cost(o) + pfx
            return (10 if _is_sreg(o) else 11) + pfx
        if m == "pop":
            o = ops[0]
            if o.type == X86_OP_MEM:
                return 17 + ea_cost(o) + pfx
            return 8 + pfx
        if m == "xchg":
            from capstone.x86_const import X86_REG_AX
            if any(o.type == X86_OP_MEM for o in ops):
                mo = [o for o in ops if o.type == X86_OP_MEM][0]
                return 17 + ea_cost(mo) + pfx
            if any(o.type == X86_OP_REG and o.reg == X86_REG_AX for o in ops):
                return 3 + pfx
            return 4 + pfx
        if m == "lea":
            return 2 + ea_cost(ops[1]) + pfx
        if m in ("lds", "les"):
            return 16 + ea_cost(ops[1]) + pfx
        if m == "in":
            return (10 if ops[1].type == X86_OP_IMM else 8) + pfx
        if m == "out":
            return (10 if ops[0].type == X86_OP_IMM else 8) + pfx

        if m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"):
            d, s = ops[0], ops[1]
            by_cl = s.type == X86_OP_REG
            n = 1                              # the count, when it is CL
            if d.type == X86_OP_REG:
                return (8 + 4 * n if by_cl else 2) + pfx
            return ((20 + 4 * n if by_cl else 15) + ea_cost(d)) + pfx

        if m in ("mul", "imul", "div", "idiv"):
            o = ops[-1]
            key = m + ("16" if o.size == 2 else "8")
            lo, hi = MULDIV[key]
            c = (lo + hi) // 2
            if o.type == X86_OP_MEM:
                c += 6 + ea_cost(o)
            return c + pfx

        if m in JCC:
            return (16 if taken else 4) + pfx
        if m == "loop":
            return (17 if taken else 5) + pfx
        if m in ("loope", "loopz", "loopne", "loopnz", "jcxz"):
            return (18 if taken else 6) + pfx
        if m == "jmp":
            o = ops[0]
            if o.type == X86_OP_IMM:
                return 15 + pfx
            return (11 if o.type == X86_OP_REG else 18 + ea_cost(o)) + pfx
        if m in ("call", "lcall"):
            o = ops[0]
            far = m == "lcall" or insn.size >= 5 and o.type == X86_OP_IMM and \
                  insn.bytes[0] == 0x9a
            if o.type == X86_OP_IMM:
                return (28 if far else 19) + pfx
            if o.type == X86_OP_REG:
                return 16 + pfx
            return (37 if far else 21) + ea_cost(o) + pfx
        if m in ("ret", "retf"):
            far = m == "retf"
            has_n = len(ops) == 1
            if far:
                return (17 if has_n else 18) + pfx
            return (12 if has_n else 8) + pfx
        if m == "int":
            return 51 + pfx

        self.unknown[m] += 1
        return 10 + pfx                        # a placeholder, and counted


def _is_sreg(o):
    from capstone.x86_const import (X86_OP_REG, X86_REG_CS, X86_REG_DS,
                                    X86_REG_ES, X86_REG_SS)
    return o.type == X86_OP_REG and o.reg in (X86_REG_CS, X86_REG_DS,
                                              X86_REG_ES, X86_REG_SS)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--resume", metavar="FILE", required=True,
                    help="the state to measure from - a level, in play")
    ap.add_argument("--frames", type=int, default=200,
                    help="how many play-loop frames to average over")
    ap.add_argument("--sync", default="0x1c3f",
                    help="the code offset that closes a frame")
    ap.add_argument("--bot", action="store_true", default=True)
    ap.add_argument("--no-bot", dest="bot", action="store_false",
                    help="leave the paddle still, to see the branch the "
                         "FRAME_DELAY compensation is written for")
    ap.add_argument("--stall", type=float, default=0.0,
                    help="percent to add for prefetch-queue stalls, which the "
                         "manual's table excludes")
    args = ap.parse_args()

    os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
    import pygame; pygame.init()
    import unicorn, capstone
    from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP
    from emulation import VgaDos, GAME_CODE
    from trace_dos import UNPACKED
    from autoplay import Bot
    from snapshot import restore

    m = VgaDos(UNPACKED, max_insns=1 << 62)
    code = m.load_seg * 16 + GAME_CODE
    bot = Bot(m)
    lv, fr, _ = restore(m, args.resume)
    print(f"from {os.path.basename(args.resume)}: level {lv}, frame {fr}"
          f"{'' if args.bot else ', paddle still'}")

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
    md.detail = True
    from unicorn.x86_const import UC_X86_REG_CX
    model = Model(cx=lambda: m.uc.reg_read(UC_X86_REG_CX))
    decoded = {}

    def decode(addr, size):
        d = decoded.get(addr)
        if d is None:
            data = m.uc.mem_read(addr, max(size, 15))
            d = next(md.disasm(bytes(data), addr), None)
            decoded[addr] = d
        return d

    sync = int(args.sync, 0) + code
    frames = []                                # cycles per frame
    cur = [0]
    by_kind = collections.Counter()
    prev = [None]                              # (addr, insn)
    pending_cost = [None]                      # a `rep`, costed before it ran
    n_insn = [0]
    done = [False]

    def on_code(uc, address, size, user):
        # Cost the *previous* instruction, now that its successor says whether
        # its branch was taken. That is the only way to get taken/not-taken
        # right without re-implementing the flags.
        p = prev[0]
        if p is not None:
            paddr, pinsn = p
            if pinsn is not None:
                target = None
                for o in pinsn.operands:
                    from capstone.x86_const import X86_OP_IMM
                    if o.type == X86_OP_IMM:
                        target = o.imm + (paddr - pinsn.address)
                taken = address != paddr + pinsn.size
                if target is not None:
                    taken = address == target
                c = pending_cost[0] if pending_cost[0] is not None else \
                    model.cost(pinsn, taken, address == paddr)
                pending_cost[0] = None
                cur[0] += c
                n_insn[0] += 1
                mn = pinsn.mnemonic
                if mn == "loop" and target == paddr:
                    by_kind["the `loop $` busy-waits"] += c
                elif mn.split()[-1] in STRING_ONCE:
                    by_kind["string moves and stores"] += c
                else:
                    by_kind["everything else"] += c
        d = decode(address, size)
        prev[0] = (address, d)
        # A `rep` needs CX as it stands *now*, before the instruction runs.
        pending_cost[0] = (model.cost(d, False, False)
                           if d is not None and d.mnemonic.startswith("rep")
                           else None)

        if address == sync:
            if cur[0]:
                frames.append(cur[0])
            cur[0] = 0
            if len(frames) >= args.frames:
                done[0] = True
                uc.emu_stop()

    m.uc.hook_add(unicorn.UC_HOOK_CODE, on_code)

    addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
    while not done[0] and m._elapsed() < 600:
        m.blocked_on_input = False
        m.uc.emu_start(addr, 0, count=20000)
        if m.finished:
            break
        m.service_keyboard()
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        if args.bot:
            bot.step()

    if len(frames) < 3:
        print("too few frames reached - is the state in play?")
        return 1
    frames = frames[1:]                        # the first is a partial
    frames.sort()
    mean = sum(frames) / len(frames)
    med = frames[len(frames) // 2]
    CPU = 8e6
    print(f"\n{len(frames)} frames, {n_insn[0]} instructions costed")
    print(f"  cycles/frame   mean {mean:8.0f}   median {med:8.0f}"
          f"   min {frames[0]}   max {frames[-1]}")
    print(f"  at 8 MHz       mean {CPU/mean:8.1f} Hz  median {CPU/med:8.1f} Hz")
    if args.stall:
        s = 1 + args.stall / 100
        print(f"  +{args.stall:.0f}% prefetch stall  "
              f"{CPU/(mean*s):8.1f} Hz")
    print("\n  where the cycles go")
    tot = sum(by_kind.values())
    for k, v in by_kind.most_common():
        print(f"    {v*100.0/tot:5.1f}%  {k}")
    if model.unknown:
        miss = sum(model.unknown.values())
        print(f"\n  NOT IN THE TABLE: {miss} of {n_insn[0]} "
              f"({miss*100.0/n_insn[0]:.2f}%), charged 10 each")
        for k, v in model.unknown.most_common(12):
            print(f"    {v:8d}  {k}")
    else:
        print(f"\n  every instruction executed was in the table")
    return 0


if __name__ == "__main__":
    sys.exit(main())
