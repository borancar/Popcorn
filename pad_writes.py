#!/usr/bin/env python3
"""What does the game write into the parts of the image the port calls padding?

Every `_pad_*` in `game.h`'s `global_t` is a claim - *these bytes are not
variables* - and every `_r` inside an entity arm is the same claim about ten
bytes that six handlers read six different ways. Both were arrived at by
reading code, which can only say what the routines that were read do.

This says what the running program does. It puts a Unicorn write hook on each
padding range, plays, and reports which of them were ever written to, by whom.
A clean region is a claim that has been tested; a written one is a variable
nobody has named yet.

The ranges are not typed in. They come from `game.h` itself - a generated C
program prints `offsetof` and `sizeof` for every `_pad_*` field - so the tool
cannot drift from the header it is checking.

    uv run pad_writes.py --route play --seconds 120
    uv run pad_writes.py --resume snapshots/L08.snap --bot --seconds 70
    uv run pad_writes.py --entities --route play --seconds 120

`--entities` watches the node pool instead, keyed by the node's **handler at
the moment of the write**, so each of the six arms can be checked separately.
One thing to read carefully there: a handler that hands its node to another
kind fills the new arm's fields *before* rewriting the handler word, so those
writes are attributed to the outgoing handler. `entity_capsule` writing
`morph.bonus` at 1ac2:3309 is that, not a stray write into `ent_fall_t::_r`.
"""
import argparse
import collections
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(HERE, "reconstruct", "src", "game.h")

# The menu routes coverage.py already uses, and the same play route
# autoplay.py walks: F3 picks the mouse, F1 opens the name box, and the box
# has to be typed into rather than waited out.
ROUTES = {
    "play": ["0206:f3", "0206:f1", "13d2:b", "13d2:o", "13d2:t",
             "13d2:return", "13d2:return"],
    "demo": ["0206:f2"],
    "keys": ["0206:f5"],
    "scores": ["0206:f6"],
    "palette": ["0206:f8", "0206:f8", "0206:f8", "0206:f8"],
    "menu": [],
}

POOL, STRIDE, POOL_END = 0x3138, 14, 0x3384


def pad_ranges():
    """Every `_pad_*` in global_t, as (name, offset, size), out of game.h."""
    names = re.findall(r"uint8_t\s+(_pad_\w+)\s*\[", open(HEADER).read())
    src = ['#include <stdio.h>', '#include <stddef.h>', '#include "game.h"',
           "int main(void){"]
    for n in names:
        src.append(r'  printf("%s %zu %zu\n", "{0}", offsetof(global_t, {0}), '
                   r'sizeof(((global_t*)0)->{0}));'.format(n))
    src += ["  return 0; }"]
    with tempfile.TemporaryDirectory() as d:
        c, exe = os.path.join(d, "pads.c"), os.path.join(d, "pads")
        open(c, "w").write("\n".join(src) + "\n")
        subprocess.run(["gcc", "-I", os.path.dirname(HEADER), "-o", exe, c],
                       check=True)
        out = subprocess.run([exe], capture_output=True, text=True,
                             check=True).stdout
    return [(n, int(o), int(s)) for n, o, s in
            (line.split() for line in out.splitlines())]


def handler_names():
    """The ENTITY_*_FN constants, address to name."""
    return {int(a, 16): n for n, a in
            re.findall(r"#define (ENTITY_\w+_FN)\s+(0x[0-9a-f]+)",
                       open(HEADER).read())}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--route", default="play", choices=sorted(ROUTES))
    ap.add_argument("--seconds", type=float, default=90.0,
                    help="emulated seconds to run for")
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--resume", metavar="FILE",
                    help="start from a snapshot.py snapshot rather than "
                         "walking the menu")
    ap.add_argument("--bot", action="store_true",
                    help="drive the paddle, which --route play does anyway")
    ap.add_argument("--entities", action="store_true",
                    help="watch the entity pool instead of the padding, keyed "
                         "by the node's handler at the moment of the write")
    args = ap.parse_args()

    os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
    import pygame
    pygame.init()
    import unicorn
    from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP
    from emulation import VgaDos, KEYMAP, GAME_CODE, UNPACKED
    from autoplay import Bot

    m = VgaDos(UNPACKED, max_insns=1 << 62, cmdline=args.cmdline)
    base = m.load_seg * 16 + GAME_CODE          # the code segment, physically
    ds = m.load_seg * 16                        # DS is 0, so this is image 0

    def writer_ip(uc):
        return uc.reg_read(UC_X86_REG_CS) * 16 + uc.reg_read(UC_X86_REG_IP) - base

    pads = pad_ranges()
    hits = {n: collections.defaultdict(set) for n, _, _ in pads}
    ent = collections.defaultdict(set)          # (handler, offset) -> writers

    def pad_cb(name):
        def cb(uc, access, address, size, value, user):
            for i in range(size):
                hits[name][address - ds + i].add(writer_ip(uc))
            return True
        return cb

    def ent_cb(uc, access, address, size, value, user):
        for i in range(size):
            rel = address - ds - POOL + i
            node, off = POOL + rel // STRIDE * STRIDE, rel % STRIDE
            handler = int.from_bytes(uc.mem_read(ds + node, 2), "little")
            ent[(node == POOL, handler, off)].add(writer_ip(uc))
        return True

    if args.entities:
        m.uc.hook_add(unicorn.UC_HOOK_MEM_WRITE, ent_cb, None,
                      ds + POOL, ds + POOL_END - 1)
    else:
        for n, off, size in pads:
            m.uc.hook_add(unicorn.UC_HOOK_MEM_WRITE, pad_cb(n), None,
                          ds + off, ds + off + size - 1)

    pending = collections.OrderedDict()
    for item in ROUTES[args.route]:
        off, _, name = item.partition(":")
        key = next((k for k in (getattr(pygame, f"K_{x}", None)
                                for x in (name.lower(), name.upper()))
                    if k is not None), None)
        if key is None or key not in KEYMAP:
            raise SystemExit(f"no scan code for {name!r}")
        pending.setdefault(int(off, 16), collections.deque()).append(key)
    started = [not pending]

    def on_code(uc, address, size_, user):
        q = pending.get(address - base)
        if q:
            sc, asc = KEYMAP[q.popleft()]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)
            if not any(pending.values()):
                started[0] = True

    m.uc.hook_add(unicorn.UC_HOOK_CODE, on_code, None, base, base + 0x6000)

    if args.resume:
        import snapshot
        snapshot.restore(m, args.resume)
        pending.clear()
        started[0] = True

    bot = Bot(m) if (args.route == "play" or args.bot) else None
    addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
    deadline = m._elapsed() + args.seconds
    while m._elapsed() < deadline:
        m.blocked_on_input = False
        try:
            m.uc.emu_start(addr, 0, count=20000)
        except Exception as e:
            print(f"  [cpu] {e}", file=sys.stderr)
            break
        if m.finished:
            print(f"  [dos] program exited: {m.finished}", file=sys.stderr)
            break
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.service_keyboard()
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        if bot is not None and started[0]:
            bot.step()

    where = args.resume or f"route {args.route}"
    print(f"{where}, {m._elapsed():.0f}s emulated")

    if args.entities:
        names = handler_names()
        print("\n  the head node at 0x3138, whose payload is _pad_head:")
        offs = sorted({o for (head, _, o) in ent if head})
        print("    written: " + " ".join(f"{o:#04x}" for o in offs))
        print("\n  the pool, by the handler the node carried:")
        for h in sorted({h for (head, h, _) in ent if not head}):
            offs = sorted(o for (head, hh, o) in ent if not head and hh == h)
            payload = [o for o in offs if 2 <= o <= 11]
            print(f"    {names.get(h, f'{h:#06x}'):<24} "
                  + (" ".join(f"{o:#04x}" for o in payload) or "(payload untouched)"))
        return

    for n, off, size in pads:
        h = hits[n]
        if not h:
            print(f"  {n:10s} {off:#06x} + {size:<5d} clean")
            continue
        ks = sorted(h)
        writers = sorted({w for v in h.values() for w in v})
        print(f"  {n:10s} {off:#06x} + {size:<5d} WRITTEN "
              f"{ks[0]:#06x}..{ks[-1]:#06x} ({len(ks)} bytes) by "
              + " ".join(f"1ac2:{w:04x}" for w in writers))


if __name__ == "__main__":
    main()
