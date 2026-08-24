#!/usr/bin/env python3
"""
Record which bytes of Popcorn's code segment actually execute.

`analyze.py` follows control flow, which cannot reach anything called through a
pointer - and Popcorn's whole entity system is exactly that: a linked list whose
nodes carry their own handler.  Running the game and recording every instruction
is the ground truth, and it is the only way to tell a routine that is merely
unreached from one that is dead.

Coverage accumulates across runs into a file, so several routes - the menu, the
demo, a played level, the hall of fame - add up to one picture.

Usage:
    python coverage.py --route play --seconds 90
    python coverage.py --route demo --seconds 60      # adds to the same file
    python coverage.py --report                       # what is still cold
    python coverage.py --report --entries             # candidate entry points
"""
import argparse
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
COVER_FILE = os.path.join(HERE, "coverage.bin")

ROUTES = {
    # Each is a list of `offset:key` - the key is pressed the first time
    # execution reaches that offset in the game's code segment. 0x0206 is the
    # main menu's INT 16h poll and 0x13d2 the player-name input loop.
    "play": ["0206:f3", "0206:f1", "13d2:b", "13d2:o", "13d2:t",
             "13d2:return", "13d2:return"],
    "demo": ["0206:f2"],
    "keys": ["0206:f5"],
    "scores": ["0206:f6"],
    "palette": ["0206:f8", "0206:f8", "0206:f8", "0206:f8"],
    "menu": [],
}


def run(args):
    os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
    import pygame
    pygame.init()
    import unicorn
    from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP
    from emulation import VgaDos, KEYMAP, GAME_CODE
    from emulation import UNPACKED
    from analyze import CODE_BASE
    from autoplay import Bot

    m = VgaDos(args.exe, max_insns=1 << 62, cmdline=args.cmdline)
    base = m.load_seg * 16 + GAME_CODE
    size = len(open(args.exe, "rb").read())      # generous upper bound
    hit = bytearray(0x6000)

    pending = collections.OrderedDict()
    for item in ROUTES[args.route]:
        off, _, name = item.partition(":")
        key = next((k for k in (getattr(pygame, f"K_{n}", None)
                                for n in (name.lower(), name.upper()))
                    if k is not None), None)
        if key is None or key not in KEYMAP:
            raise SystemExit(f"no scan code for {name!r}")
        pending.setdefault(int(off, 16), collections.deque()).append(key)
    started = [not pending]

    def on_code(uc, address, size_, user):
        off = address - base
        if 0 <= off < len(hit):
            hit[off] = 1
        q = pending.get(off)
        if q:
            sc, asc = KEYMAP[q.popleft()]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)
            if not any(pending.values()):
                started[0] = True

    m.uc.hook_add(unicorn.UC_HOOK_CODE, on_code, None, base, base + 0x6000)

    bot = Bot(m) if args.route == "play" else None
    addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
    while m._elapsed() < args.seconds:
        m.blocked_on_input = False
        m.uc.emu_start(addr, 0, count=20000)
        if m.finished:
            break
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.service_keyboard()
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        if bot is not None and started[0]:
            bot.step()

    old = bytearray(open(COVER_FILE, "rb").read()) if os.path.exists(COVER_FILE) \
        else bytearray(len(hit))
    old = (old + bytearray(len(hit)))[:len(hit)]
    added = sum(1 for i in range(len(hit)) if hit[i] and not old[i])
    for i in range(len(hit)):
        old[i] |= hit[i]
    open(COVER_FILE, "wb").write(bytes(old))
    print(f"route {args.route}: {sum(hit)} bytes executed, {added} new; "
          f"{sum(old)} total")


def report(args):
    from tools_dis import load_image
    from analyze import CODE_BASE
    if not os.path.exists(COVER_FILE):
        raise SystemExit("no coverage.bin yet - run without --report first")
    cov = open(COVER_FILE, "rb").read()
    img = load_image(args.exe)
    size = len(img) - CODE_BASE
    hot = sum(1 for i in range(size) if i < len(cov) and cov[i])
    print(f"executed {hot} of {size} bytes ({100.0 * hot / size:.1f}%)")

    runs, start = [], None
    for i in range(size):
        on = i < len(cov) and cov[i]
        if on:
            if start is not None:
                runs.append((start, i - start))
                start = None
        elif start is None:
            start = i
    if start is not None:
        runs.append((start, size - start))
    runs = [r for r in runs if r[1] >= args.min_gap]
    print(f"{len(runs)} cold runs >= {args.min_gap} bytes, "
          f"{sum(n for _, n in runs)} bytes")
    for off, n in runs:
        blob = img[CODE_BASE + off:CODE_BASE + off + min(n, 20)]
        print(f"  {off:04x}  {CODE_BASE + off:#07x}  {n:5d}  {blob.hex(' ')}")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    from emulation import UNPACKED
    ap.add_argument("--exe", default=UNPACKED)
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--route", choices=sorted(ROUTES), default="play")
    ap.add_argument("--seconds", type=float, default=90.0)
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--min-gap", type=int, default=24)
    args = ap.parse_args()
    report(args) if args.report else run(args)


if __name__ == "__main__":
    main()
