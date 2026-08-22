#!/usr/bin/env python3
"""
Check the C reconstruction against the code it replaces.

A routine that compiles proves nothing, and one that looks right on screen
proves very little more - a blitter can be wrong in ways that still draw
something plausible. So each transcribed routine is checked the only way that
settles it: the emulator is stopped at the routine's entry, the machine is
captured, the **original** body is allowed to run to its return, and the
machine is captured again. The C routine is then run on the first capture and
its result diffed against the second.

The comparison is between the C and the original **on the same call inside one
run**, so it needs no determinism to mean anything: the host clock and the
game's RNG cannot make it flaky.

A routine that never ran proves nothing either, and that is the trap this is
built to avoid - "0 mismatches" over a state that never reaches the routine
reads exactly like a pass. So the exit status distinguishes "checked and
agreed" from "never called", and the summary says which.

Usage:
    python verify.py                            # every routine C implements
    python verify.py --only 0x22de,0x2281
    python verify.py --seconds 120 --verbose
"""
import argparse
import collections
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.join(HERE, "reconstruct", "popcorn")

# Every routine the C side implements, with what it is and how far into a
# session it is first reached. Keep in step with dispatch() in verify.c.
ROUTINES = {
    0x27D7: "ball_step",
    0x22DE: "paddle_row_offsets",
    0x2281: "blit_xor",
    0x221A: "draw_paddle",
    0x0C64: "draw_char",
    0x1712: "input_keyboard",
    0x044B: "level_colours",
    0x10C5: "draw_run",
    0x10D1: "draw_text",
    0x14A7: "draw_cursor",
    0x1E50: "walker_draw",
    0x1FC1: "field_backdrop",
    0x2034: "draw_brick_row",
    0x2109: "scroll_up_band",
    0x2148: "scroll_down_band",
    0x2187: "draw_paddle_shifted",
    0x22A9: "draw_paddle_raw",
    0x318B: "extra_life",
    0x20B9: "draw_sprite_20x6",
    0x2316: "ball_paddle",
    0x254D: "ball_bricks",
    0x247F: "ball_after",
    0x2827: "ball_redraw",
    0x2881: "ball_draw",
    0x3B64: "xor_sprite_16x7",
    0x413D: "score_add",
    0x2755: "probe_cell",
    0x2DAA: "bonus_points",
    0x2DEF: "bonus_catch",
    0x2E03: "bonus_laser",
    0x2E16: "bonus_multiball",
    0x3119: "bonus_net",
    0x315B: "bonus_reverse",
    0x31E8: "bonus_speed",
    0x3231: "bonus_nothing",
    0x41B1: "fill_column",
    0x2E1E: "ball_on_paddle",
    0x2EE3: "laser_fire",
    0x3273: "entity_capsule",
    0x3386: "entity_paddle_fx",
    0x3561: "entity_popup",
    0x3717: "entity_multiball",
    0x306B: "shot_xor",
    0x30DD: "pixel_xor",
    0x3146: "flash_bar",
    0x3232: "entity_alloc",
    0x3257: "entity_unlink",
    0x365E: "entity_soften",
    0x366F: "entity_repeat",
    0x3696: "entity_plain",
    0x36A1: "entity_ball_arrive",
    0x36F6: "entity_cells_timer",
    0x36FB: "cells_restore",
    0x3668: "cell_set_three",
    0x37E0: "entity_ball_hold",
    0x390D: "entity_hatch",
    0x39A1: "bonus_release",
    0x39FA: "entity_bonus",
    0x3AEE: "entity_sparkle",
    0x3B2A: "entity_crumble",
    0x3DF1: "bonus_update",
    0x3F20: "bonus_hits_ball",
    0x3F4F: "sprite_shift_draw",
    0x406A: "xor_sprite_20x16",
    0x40C0: "game_random",
    0x40F2: "xor_sprite_16xn",
    0x5099: "save_screen",
    0x50BC: "restore_screen",
}

REGS = "ax bx cx dx si di bp es ds fl".split()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=90.0)
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--only", default="",
                    help="comma-separated routine offsets to check")
    ap.add_argument("--max-per-routine", type=int, default=10,
                    help="stop checking a routine once this many of its calls "
                         "have actually changed something - counting calls "
                         "made instead lets an early-return path fill the "
                         "quota with agreements that prove nothing")
    ap.add_argument("--keyboard", action="store_true",
                    help="play through the keyboard input routine rather than "
                         "the mouse, which is the only way 1ac2:16d2 runs")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(PORT):
        raise SystemExit(f"{PORT} is not built; run `make -C reconstruct`")

    wanted = ROUTINES
    if args.only:
        keep = {int(x, 0) for x in args.only.split(",") if x.strip()}
        wanted = {k: v for k, v in ROUTINES.items() if k in keep}
        missing = keep - set(ROUTINES)
        if missing:
            raise SystemExit("no C routine for " +
                             ", ".join(hex(x) for x in sorted(missing)))

    os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
    import pygame
    pygame.init()
    import unicorn
    from unicorn.x86_const import (
        UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
        UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_ES,
        UC_X86_REG_DS, UC_X86_REG_EFLAGS, UC_X86_REG_CS, UC_X86_REG_IP,
        UC_X86_REG_SP, UC_X86_REG_SS)
    from emulation import VgaDos, KEYMAP, GAME_CODE
    from trace_dos import UNPACKED
    from autoplay import Bot, ROUTE_PLAY, ROUTE_PLAY_KEYS, parse_route

    m = VgaDos(UNPACKED, max_insns=1 << 62, cmdline=args.cmdline)
    base = m.load_seg * 16
    code = base + GAME_CODE
    bot = Bot(m, keyboard=args.keyboard)

    pending = {}
    route = ROUTE_PLAY_KEYS if args.keyboard else ROUTE_PLAY
    for off, key, _ in parse_route(route):
        pending.setdefault(off, collections.deque()).append(key)
    started = [False]

    checked = collections.Counter()
    # How many of the agreements were on a call that actually did something.
    # A routine whose common path is an early return can agree twenty-five
    # times without its interesting path ever running - the same trap as
    # "never called", one level down, and worth reporting separately.
    did_work = collections.Counter()
    mismatched = collections.Counter()
    first_bad = {}
    # Set while the original body is running, so the entry hook does not
    # re-enter for a nested call to the same routine.
    inside = [None]
    pre = {}

    # The stack is scratch, not state. It sits at SS=0x1aa2 with SP starting
    # at 0x200, so it occupies the 512 bytes immediately below the code
    # segment, and the original's `push cx / push di ... pop` leaves different
    # bytes there than a C function that takes its arguments as arguments.
    # Comparing it reports every routine that pushes anything as a mismatch,
    # which is noise - the first run flagged draw_char on exactly this.
    STACK_LO, STACK_HI = 0x1AA20, 0x1AC20

    def bios_ticks():
        """What the PRNG at 0x40c0 starts from.

        It is not the tick count: the routine reads the counter's two words at
        0040:006c and 0040:006e and **adds** them, then keeps working in 16
        bits. Handing over the 32-bit count instead leaves the low word right
        and the seed wrong.
        """
        lo, hi = struct.unpack("<HH", m.uc.mem_read(0x46C, 4))
        return (lo + hi) & 0xFFFF

    def snapshot():
        img = bytearray(m.uc.mem_read(base, 0x208B0))
        img[STACK_LO:STACK_HI] = bytes(STACK_HI - STACK_LO)
        vram = bytes(m.uc.mem_read(0xB8000, 0x4000))
        return bytes(img), vram

    def regs_now():
        r = [m.uc.reg_read(x) & 0xFFFF for x in (
            UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
            UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_ES,
            UC_X86_REG_DS, UC_X86_REG_EFLAGS)]
        return r

    def on_code(uc, address, size, user):
        off = address - code
        q = pending.get(off)
        if q:
            sc, asc = KEYMAP[q.popleft()]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)
            if not any(pending.values()):
                started[0] = True

        if inside[0] is not None:
            # Waiting for this call to return: the return address is on the
            # stack where the CALL left it, and reaching it means the body is
            # done. Compare, then release.
            if address == inside[0][1]:
                want_img, want_vram = snapshot()
                compare(inside[0][0], inside[0][2], want_img, want_vram)
                inside[0] = None
            return

        # Cap on calls that *did something*, not on calls made. A routine
        # whose common path is an early return would otherwise fill its quota
        # with agreements that prove nothing, and stop being sampled long
        # before its real path ever ran.
        if off in wanted and did_work[off] < args.max_per_routine:
            sp = uc.reg_read(UC_X86_REG_SP)
            ss = uc.reg_read(UC_X86_REG_SS)
            ret = struct.unpack("<H", uc.mem_read(ss * 16 + sp, 2))[0]
            inside[0] = (off, uc.reg_read(UC_X86_REG_CS) * 16 + ret,
                         (regs_now(), snapshot(), bios_ticks()))

    def compare(off, before, want_img, want_vram):
        regs, (img, vram), ticks = before
        with tempfile.TemporaryDirectory() as d:
            si = os.path.join(d, "in.pvs")
            so = os.path.join(d, "out.bin")
            with open(si, "wb") as f:
                f.write(b"PVS2" + struct.pack("<I", off))
                f.write(struct.pack("<10H", *regs))
                f.write(struct.pack("<I", ticks))
                f.write(struct.pack("<I", len(img)) + img)
                f.write(struct.pack("<I", len(vram)) + vram)
            r = subprocess.run([PORT, "--verify", si, so],
                               capture_output=True, text=True)
            if r.returncode == 2:
                return                      # no C routine; not a failure
            if r.returncode != 0:
                mismatched[off] += 1
                first_bad.setdefault(off, f"exit {r.returncode}: "
                                          f"{r.stderr.strip()}")
                return
            got = open(so, "rb").read()
        got_img = bytearray(got[:len(img)])
        got_img[STACK_LO:STACK_HI] = bytes(STACK_HI - STACK_LO)
        got_img, got_vram = bytes(got_img), got[len(img):]
        checked[off] += 1
        bad = []
        if got_vram != want_vram:
            i = next(k for k in range(len(want_vram))
                     if want_vram[k] != got_vram[k])
            bad.append(f"vram at {i:#06x}: original {want_vram[i]:#04x}, "
                       f"C {got_vram[i]:#04x}")
        if got_img != want_img:
            i = next(k for k in range(len(want_img))
                     if want_img[k] != got_img[k])
            bad.append(f"image at {i:#07x}: original {want_img[i]:#04x}, "
                       f"C {got_img[i]:#04x}")
        if want_img != img or want_vram != vram:
            did_work[off] += 1
        if bad:
            mismatched[off] += 1
            first_bad.setdefault(off, "; ".join(bad))
        elif args.verbose:
            print(f"  {ROUTINES[off]} ({off:#06x}) agrees "
                  f"[{checked[off]}]")

    m.uc.hook_add(unicorn.UC_HOOK_CODE, on_code, None, code, code + 0x10000)

    addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
    while m._elapsed() < args.seconds:
        m.blocked_on_input = False
        m.uc.emu_start(addr, 0, count=20000)
        if m.finished:
            break
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.service_keyboard()
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        if started[0]:
            bot.step()

    print(f"\n=== {m._elapsed():.0f}s of play ===")
    never = []
    fails = 0
    for off in sorted(wanted):
        name = ROUTINES[off]
        n, bad = checked[off], mismatched[off]
        if n == 0 and bad == 0:
            never.append(f"{name} ({off:#06x})")
            continue
        if bad:
            fails += 1
            print(f"  FAIL {name} ({off:#06x}): {bad} of {n + bad} calls "
                  f"differ - {first_bad[off]}")
        else:
            w = did_work[off]
            note = (f"{n} calls, identical" if w == n else
                    f"{n} calls, identical - but only {w} changed anything")
            if w == 0:
                note += " (every one was an early return: unproven)"
            print(f"  ok   {name} ({off:#06x}): {note}")
    if never:
        print(f"  NOT REACHED, so unproven: {', '.join(never)}")
    return 1 if fails or never else 0


if __name__ == "__main__":
    sys.exit(main())
