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

    uv run snapshot.py hsc.snap --keys @0206:f6 --at 0x4d37
    uv run snapshot.py demo.snap --keys @0206:f2 --seconds 25
    uv run snapshot.py border.snap --seconds 20

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
        else:
            f.write(struct.pack("<I", 0))
        # Low memory: the interrupt vector table and the BIOS data area,
        # neither of which is inside the load image. The game installs its own
        # INT 09h for play and takes it out again for the menus, so a resume
        # that boots fresh and overwrites only the image has the BIOS handler
        # where the running game had the game's.
        low = bytes(m.uc.mem_read(0, 0x500))
        f.write(b"LOWM" + struct.pack("<I", len(low)) + low)
    return path


def read(path):
    """-> (level, frame, regs, ticks, image, vram, extra, low)"""
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
    extra = low = None
    if o + 4 <= len(d):
        elen, = struct.unpack_from("<I", d, o); o += 4
        if elen and o + elen <= len(d):
            extra = json.loads(d[o:o + elen])
            o += elen
    if d[o:o + 4] == b"LOWM":
        o += 4
        llen, = struct.unpack_from("<I", d, o); o += 4
        low = d[o:o + llen]
    return level, frame, regs, ticks, img, vram, extra, low


def restore(m, path):
    """Put one back, registers and all. -> (level, frame, extra)"""
    level, frame, regs, ticks, img, vram, extra, low = read(path)
    # The mode, which the game sets once with INT 10h AX=0005 at startup and a
    # resume never runs. Without it the machine is still in the 80x25 text mode
    # DOS hands it, and anything that renders gets a black window - the
    # emulator draws from m.width, m.height and m.text_mode, not from the bytes
    # at 0xb8000.
    #
    # Before the screen is written, because setting mode 5 *clears* video
    # memory; after it, the snapshot's own screen would be the thing lost.
    m.mode = 0x05
    m.width, m.height = 320, 200
    m.text_mode = False
    m.cga_mode_ctrl = 0x0E              # what the BIOS leaves for mode 05h
    m.cga_colour = 0x30
    m.uc.mem_write(m.load_seg * 16, img)
    m.uc.mem_write(0xB8000, vram)
    for r, v in zip(_regs(m.uc), regs):
        m.uc.reg_write(r, v)
    m.uc.mem_write(0x46C, struct.pack("<I", ticks))
    if low:
        m.uc.mem_write(0, low)
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
    ap.add_argument("--copy", action="append", default=[],
                    metavar="DST=SRC:LEN",
                    help="copy a block of the image over itself before "
                         "writing. The player table at 0x344f is 0x11b bytes "
                         "a player, so --copy 0x356a=0x344f:0x11b gives a "
                         "second player a record that is the shape the game "
                         "made rather than one invented here")
    ap.add_argument("--poke-str", action="append", default=[],
                    metavar="ADDR=TEXT",
                    help="write text, for the fields that hold ASCII - a "
                         "player's name at +0, its score as digits at +0x10")
    ap.add_argument("--poke", action="append", default=[],
                    metavar="ADDR=VALUE",
                    help="write a byte into the image as the snapshot is "
                         "**written**, which is after the run, not before it. "
                         "So a poke seeds whatever resumes from the file; it "
                         "does not steer the capture, and combining it with "
                         "--at means stopping somewhere the poke then takes "
                         "effect from. It fast-forwards the game rather than "
                         "faking it: --poke 0x2f10=1 leaves one brick, so the "
                         "next hit clears the level and everything a level "
                         "transition runs becomes reachable in seconds. The "
                         "state is the game's own, only sooner")
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--watch", action="store_true",
                    help="show the emulator in a window while it runs. "
                         "Headless is the default because most uses here are "
                         "unattended, but a run nobody can see is a run "
                         "nobody can check - and the bot's behaviour is one "
                         "of the things worth checking by eye")
    ap.add_argument("--scale", type=int, default=3)
    args = ap.parse_args()

    if not args.watch:
        os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
    import pygame
    pygame.init()
    import unicorn
    from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP
    from emulation import VgaDos, KEYMAP, GAME_CODE, make_surface
    from emulation import UNPACKED
    from autoplay import Bot, parse_route

    m = VgaDos(UNPACKED, max_insns=1 << 62, cmdline=args.cmdline)
    screen = None
    if args.watch:
        screen = pygame.display.set_mode((320 * args.scale, 200 * args.scale))
        pygame.display.set_caption("Popcorn - the emulator, with the bot")
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
        if screen is not None:
            for _ev in pygame.event.get():
                if _ev.type == pygame.QUIT:
                    stop[0] = True
            surf = make_surface(m).convert(screen)
            pygame.transform.scale(surf, screen.get_size(), screen)
            pygame.display.flip()

    if want is not None and not stop[0]:
        raise SystemExit(f"never reached {want:#06x} in "
                         f"{args.seconds:.0f}s of emulated time")
    for poke in args.poke:
        where, _, what = poke.partition("=")
        addr, val = int(where, 0), int(what, 0)
        m.uc.mem_write(m.load_seg * 16 + addr, bytes([val & 0xFF]))
        print(f"  poked {addr:#07x} = {val:#04x}")
    for spec in args.copy:
        dst, _, rest = spec.partition("=")
        src, _, ln = rest.partition(":")
        d, o, n = int(dst, 0), int(src, 0), int(ln, 0)
        buf = bytes(m.uc.mem_read(m.load_seg * 16 + o, n))
        m.uc.mem_write(m.load_seg * 16 + d, buf)
        print(f"  copied {n} bytes {o:#07x} -> {d:#07x}")
    for spec in args.poke_str:
        where, _, text = spec.partition("=")
        addr = int(where, 0)
        m.uc.mem_write(m.load_seg * 16 + addr, text.encode("latin-1"))
        print(f"  poked {addr:#07x} = {text!r}")
    write(m, args.path)
    lv = m.uc.mem_read(m.load_seg * 16 + LEVEL_NUMBER, 1)[0]
    where = f"at {want:#06x}" if want is not None else \
            f"after {m._elapsed():.0f}s"
    print(f"{args.path}: level {lv}, {where}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
