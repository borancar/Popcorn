#!/usr/bin/env python3
"""Capture and restore the whole machine, so a check can *start* at a screen.

`sidebyside.py` writes one of these at the start of every level, which is what
made a divergence twenty minutes into a game findable at all. But a level start
is not the only state worth checking: `verify_all.py`'s union reaches 92 of the
147 dispatched routines, and most of the 51 it misses are not hard to run -
they are simply not on the way to anywhere a bot goes. The menu's border
animation, the attract demo's walker, the between-level tally, the high-score
sort. Each needs the game to be *at* that screen, and playing to it takes
longer than checking it.

So this drives the emulator to a stopping condition and writes a snapshot
there. The format is the one `sidebyside.py --resume` and `verify.py --resume`
already read: the unmasked image, the screen, all fourteen registers and the
BIOS tick the PRNG is seeded from.

    venv/bin/python snapshot.py hsc.snap --keys @0206:f6 --at 0x4d37
    venv/bin/python snapshot.py demo.snap --keys @0206:f2 --seconds 25
    venv/bin/python snapshot.py border.snap --seconds 20

`--at OFFSET` stops the first time execution reaches that code offset, which is
reproducible; `--seconds` stops after that much *emulated* time, which is not
quite, and is the fallback for a screen that has no single instruction that
means "here". Both are segment-relative, the convention everything else here
uses.
"""
import argparse
import json
import os
import struct
import sys

SNAP_MAGIC = b"PSNP"
SNAP_REGS = 14                  # ax bx cx dx si di bp es ds fl sp ss cs ip
IMAGE_LEN = 0x208B0
CGA_SIZE = 0x4000
CODE = 0x1AC20
LEVEL_NUMBER = 0x13CC


def _regs(uc):
    from unicorn.x86_const import (
        UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
        UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_ES,
        UC_X86_REG_DS, UC_X86_REG_EFLAGS, UC_X86_REG_SP, UC_X86_REG_SS,
        UC_X86_REG_CS, UC_X86_REG_IP)
    return (UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
            UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_ES,
            UC_X86_REG_DS, UC_X86_REG_EFLAGS, UC_X86_REG_SP, UC_X86_REG_SS,
            UC_X86_REG_CS, UC_X86_REG_IP)


def bios_ticks(m):
    """What the PRNG at 0x40c0 starts from.

    Not the tick count: the routine reads the counter's two words at 0040:006c
    and 0040:006e and **adds** them, keeping sixteen bits.
    """
    lo, hi = struct.unpack("<HH", m.uc.mem_read(0x46C, 4))
    return (lo + hi) & 0xFFFF


def raw_ticks(m):
    """The BIOS counter itself, not the sixteen-bit fold the PRNG makes of it.
    Storing the fold restores a different counter, and everything seeded from
    it then walks a different sequence."""
    return struct.unpack("<I", m.uc.mem_read(0x46C, 4))[0]


def write(m, path, level=None, frame=0, extra=None):
    base = m.load_seg * 16
    if level is None:
        level = m.uc.mem_read(base + LEVEL_NUMBER, 1)[0]
    img = bytes(m.uc.mem_read(base, IMAGE_LEN))
    vram = bytes(m.uc.mem_read(0xB8000, CGA_SIZE))
    with open(path, "wb") as f:
        f.write(SNAP_MAGIC + struct.pack("<II", level, frame))
        f.write(struct.pack(f"<{SNAP_REGS}H",
                            *[m.uc.reg_read(r) & 0xFFFF for r in _regs(m.uc)]))
        f.write(struct.pack("<I", raw_ticks(m)))
        f.write(struct.pack("<I", len(img)) + img)
        f.write(struct.pack("<I", len(vram)) + vram)
        # Optional and trailing, so a snapshot written before this existed
        # still reads: whatever the driver needs to carry across a resume,
        # which for sidebyside.py is the bot's wander generator.
        if extra is not None:
            blob = json.dumps(extra).encode()
            f.write(struct.pack("<I", len(blob)) + blob)
    return path


def read(path):
    """-> (level, frame, regs, ticks, image, vram, extra)"""
    d = open(path, "rb").read()
    if d[:4] != SNAP_MAGIC:
        raise SystemExit(f"{path}: not a snapshot")
    level, frame = struct.unpack_from("<II", d, 4)
    o = 12
    regs = struct.unpack_from(f"<{SNAP_REGS}H", d, o); o += SNAP_REGS * 2
    ticks, = struct.unpack_from("<I", d, o); o += 4
    ilen, = struct.unpack_from("<I", d, o); o += 4
    img = d[o:o + ilen]; o += ilen
    vlen, = struct.unpack_from("<I", d, o); o += 4
    vram = d[o:o + vlen]; o += vlen
    extra = None
    if o + 4 <= len(d):
        elen, = struct.unpack_from("<I", d, o); o += 4
        if elen and o + elen <= len(d):
            extra = json.loads(d[o:o + elen])
    return level, frame, regs, ticks, img, vram, extra


def restore(m, path):
    """Put one back, registers and all. -> (level, frame, extra)"""
    level, frame, regs, ticks, img, vram, extra = read(path)
    m.uc.mem_write(m.load_seg * 16, img)
    m.uc.mem_write(0xB8000, vram)
    for r, v in zip(_regs(m.uc), regs):
        m.uc.reg_write(r, v)
    m.uc.mem_write(0x46C, struct.pack("<I", ticks))
    return level, frame, extra


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="where to write the snapshot")
    ap.add_argument("--keys", default="",
                    help="comma-separated @offset:key triggers, autoplay.py's "
                         "route syntax, so a screen can be driven to by key "
                         "rather than waited for: @0206:f6")
    ap.add_argument("--at", default="",
                    help="stop the first time execution reaches this "
                         "segment-relative code offset")
    ap.add_argument("--seconds", type=float, default=30.0,
                    help="stop after this much emulated time; also the cap "
                         "on --at")
    ap.add_argument("--bot", action="store_true",
                    help="run autoplay's paddle bot, for a state that needs "
                         "the game to be survived rather than only reached")
    ap.add_argument("--resume", metavar="FILE",
                    help="start from an existing snapshot instead of from "
                         "boot, so a state can be reached in stages")
    ap.add_argument("--poke", action="append", default=[],
                    metavar="ADDR=VALUE",
                    help="write a byte into the image before capturing. This "
                         "fast-forwards the game rather than playing it: "
                         "--poke 0x2f10=1 leaves one brick, so the next hit "
                         "clears the level and everything a level transition "
                         "runs becomes reachable in seconds. The state is the "
                         "game's own, only sooner")
    ap.add_argument("--cmdline", default="")
    args = ap.parse_args()

    os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
    import pygame
    pygame.init()
    import unicorn
    from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP
    from emulation import VgaDos, KEYMAP, GAME_CODE
    from trace_dos import UNPACKED
    from autoplay import Bot, parse_route

    m = VgaDos(UNPACKED, max_insns=1 << 62, cmdline=args.cmdline)
    code = m.load_seg * 16 + GAME_CODE
    bot = Bot(m)
    if args.resume:
        lv, fr, _ = restore(m, args.resume)
        print(f"from {os.path.basename(args.resume)}: level {lv}, frame {fr}")

    import collections
    pending = collections.defaultdict(collections.deque)
    for off, key, _ in parse_route(
            [k for k in args.keys.split(",") if k.strip()]):
        pending[off].append(key)
    want = int(args.at, 0) if args.at else None
    stop = [False]

    def on_code(uc, address, size, user):
        off = address - code
        q = pending.get(off)
        if q:
            sc, asc = KEYMAP[q.popleft()]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)
        if want is not None and off == want and not pending:
            stop[0] = True
            uc.emu_stop()

    m.uc.hook_add(unicorn.UC_HOOK_CODE, on_code, None, code, code + 0x10000)

    addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
    while not stop[0] and m._elapsed() < args.seconds:
        m.blocked_on_input = False
        m.uc.emu_start(addr, 0, count=20000)
        if m.finished:
            break
        m.service_keyboard()
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        if args.bot and not pending:
            bot.step()

    if want is not None and not stop[0]:
        raise SystemExit(f"never reached {want:#06x} in "
                         f"{args.seconds:.0f}s of emulated time")
    for poke in args.poke:
        where, _, what = poke.partition("=")
        addr, val = int(where, 0), int(what, 0)
        m.uc.mem_write(m.load_seg * 16 + addr, bytes([val & 0xFF]))
        print(f"  poked {addr:#07x} = {val:#04x}")
    write(m, args.path)
    lv = m.uc.mem_read(m.load_seg * 16 + LEVEL_NUMBER, 1)[0]
    where = f"at {want:#06x}" if want is not None else \
            f"after {m._elapsed():.0f}s"
    print(f"{args.path}: level {lv}, {where}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
