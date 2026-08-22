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
import re
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.join(HERE, "reconstruct", "popcorn")

# Every routine the C side implements, with what it is and how far into a
# session it is first reached. Keep in step with dispatch() in verify.c.
def _routines():
    """Every routine `reconstruct/verify.c` can dispatch, named from game.c.

    Kept out of a hand-written list on purpose. There used to be one here, it
    fell behind the dispatch table by fifty-seven routines, and the runs that
    said "nothing failing" had quietly not checked any of them - the same trap
    as analyze.py's short seed list, one layer up. Both sides are read from
    the source, so they cannot drift.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    cases = set(int(m, 16) for m in re.findall(
        r"case 0x([0-9a-f]{4}):",
        open(os.path.join(here, "reconstruct", "verify.c")).read()))
    src = open(os.path.join(here, "reconstruct", "game.c")).read()
    names = {}
    # Two routines can share one header: "1ac2:5099 / 1ac2:50bc  save_screen /
    # restore_screen". Take those pairwise first.
    for a, b, na, nb in re.findall(
            r"^\s*(?:/\*|\*)\s*1ac2:([0-9a-f]{4}) / 1ac2:([0-9a-f]{4})"
            r"\s+(\w+) / (\w+)", src, re.M):
        names.setdefault(int(a, 16), na)
        names.setdefault(int(b, 16), nb)
    # Only a header counts - `1ac2:xxxx name` at the start of a comment line.
    # Matching anywhere picks up prose: "1ac2:1c4f drives it, 1ac2:1e23 steps
    # it" named two routines "drives" and "steps".
    for off, name in re.findall(
            r"^\s*(?:/\*|\*)\s*1ac2:([0-9a-f]{4})\s+(\w+)", src, re.M):
        names.setdefault(int(off, 16), name)
    # A loose pass for the headers that do not fit either shape, so a routine
    # gets a name rather than routine_xxxx. Strict wins where both match.
    for off, name in re.findall(r"1ac2:([0-9a-f]{4})\s+(\w+)", src):
        names.setdefault(int(off, 16), name)
    return {off: names.get(off, f"routine_{off:04x}") for off in sorted(cases)}


ROUTINES = _routines()

REGS = "ax bx cx dx si di bp es ds fl".split()

# Routines whose answer is a register rather than a change to memory. Without
# this they pass whatever they compute: 0x40c0 only bumps a counter by a
# constant, so comparing memory alone says nothing about the number it
# returned. The value is what the caller reads - AH for the two random
# routines, since both are used as `random(dl)` with the result in AH.
RETURNS = {
    0x40C0: "ah",
    0x5448: "ax",
    0x548A: "ax",
    # Routines that answer in the **carry flag**. play_loop reads `jae` after
    # each of these, so the flag *is* the decision - and comparing memory
    # alone passes a routine that returns the opposite one. "ncf" is for the C
    # returning the sense play_loop tests: `if (!ball_redraw(...))` takes the
    # loss, and the original takes it on carry set, so C == !CF.
    0x2827: "ncf",                      # ball_redraw: clear means keep going
    0x2E1E: "ncf",                       # ball_on_paddle
}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=90.0)
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--only", default="",
                    help="comma-separated routine offsets to check")
    ap.add_argument("--max-checked", type=int, default=60,
                    help="stop sampling a routine after this many "
                         "checks even if none of them did work")
    ap.add_argument("--max-per-routine", type=int, default=10,
                    help="stop checking a routine once this many of its calls "
                         "have actually changed something - counting calls "
                         "made instead lets an early-return path fill the "
                         "quota with agreements that prove nothing")
    ap.add_argument("--menu", action="store_true",
                    help="stay in the menu instead of playing - the "
                         "attract demo and the menu animations are "
                         "not reachable from a route that starts a game")
    ap.add_argument("--keyboard", action="store_true",
                    help="play through the keyboard input routine rather than "
                         "the mouse, which is the only way 1ac2:16d2 runs")
    ap.add_argument("--keys", default="",
                    help="extra @offset:key triggers, comma separated, on top "
                         "of the route - the way to reach a screen that is one "
                         "key press from a snapshot")
    ap.add_argument("--resume", metavar="FILE",
                    help="start from a sidebyside.py snapshot instead of "
                         "walking the menu, so routines that only run deep "
                         "in a game can be sampled")
    ap.add_argument("--json", metavar="FILE",
                    help="write one record per routine, so several runs over "
                         "different routes can be unioned")
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
    route = [] if args.menu else (ROUTE_PLAY_KEYS if args.keyboard
                                  else ROUTE_PLAY)
    for off, key, _ in parse_route(route):
        pending.setdefault(off, collections.deque()).append(key)
    extra_keys = parse_route([k for k in args.keys.split(",") if k.strip()])
    started = [False]

    checked = collections.Counter()
    # How many of the agreements were on a call that actually did something.
    # A routine whose common path is an early return can agree twenty-five
    # times without its interesting path ever running - the same trap as
    # "never called", one level down, and worth reporting separately.
    did_work = collections.Counter()
    mismatched = collections.Counter()
    interrupted = collections.Counter()   # samples dropped: an IRQ landed inside
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
    # The three key-state bytes the INT 09h handler maintains - left, right and
    # action - are asynchronous input, not a function of any routine. The
    # original takes interrupts while a sampled call is running and the C takes
    # none, so a difference here measures when a key arrived, not whether the
    # transcription is right: draw_paddle_shifted, which never mentions the
    # bytes, differed on one call in eleven because a key went down inside it.
    # The same argument as the stack. Blanked at comparison time only - the
    # routine still gets the real bytes, because laser_fire reads 0x2d4c to
    # decide whether to fire. What this gives up is the check on 0x195a,
    # inside play_loop, the one place game code clears them.
    KEYS_LO, KEYS_HI = 0x2D4C, 0x2D4F

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
                # A keyboard interrupt delivered while the original was inside
                # the routine writes the key-state bytes at 0x2d4c-0x2d4e from
                # outside it. The C takes no interrupts, so those bytes would
                # differ for a reason that is not the transcription - the same
                # argument as excluding the stack. Drop the sample rather than
                # exclude the bytes, so the routines that legitimately write
                # them are still checked on the calls where nothing interrupted.
                if m.guest_dispatch[9] != inside[0][3]:
                    interrupted[inside[0][0]] += 1
                    inside[0] = None
                    return
                want_img, want_vram = snapshot()
                ax = uc.reg_read(UC_X86_REG_AX) & 0xFFFF
                # The flags at the `ret`, because for several routines the
                # answer is the carry rather than anything in memory.
                cf = uc.reg_read(UC_X86_REG_EFLAGS) & 1
                compare(inside[0][0], inside[0][2], want_img, want_vram,
                        ax, cf)
                inside[0] = None
            return

        # Cap on calls that *did something*, not on calls made. A routine
        # whose common path is an early return would otherwise fill its quota
        # with agreements that prove nothing, and stop being sampled long
        # before its real path ever ran.
        # A routine whose common path is an early return never reaches the
        # did_work cap, and each sample costs a subprocess. Stop sampling it
        # once enough calls have agreed that more would only cost time - the
        # report already says the sample was all early returns.
        if (off in wanted and did_work[off] < args.max_per_routine
                and checked[off] < args.max_checked):
            sp = uc.reg_read(UC_X86_REG_SP)
            ss = uc.reg_read(UC_X86_REG_SS)
            ret = struct.unpack("<H", uc.mem_read(ss * 16 + sp, 2))[0]
            inside[0] = (off, uc.reg_read(UC_X86_REG_CS) * 16 + ret,
                         (regs_now(), snapshot(), bios_ticks()),
                         m.guest_dispatch[9])

    def compare(off, before, want_img, want_vram, want_ax, want_cf=0):
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
        got_res = got[len(img) + 0x4000:]
        got = got[:len(img) + 0x4000]
        got_img = bytearray(got[:len(img)])
        got_img[STACK_LO:STACK_HI] = bytes(STACK_HI - STACK_LO)
        got_img[KEYS_LO:KEYS_HI] = bytes(KEYS_HI - KEYS_LO)
        want_img = bytearray(want_img)
        want_img[KEYS_LO:KEYS_HI] = bytes(KEYS_HI - KEYS_LO)
        want_img = bytes(want_img)
        got_img, got_vram = bytes(got_img), got[len(img):]
        checked[off] += 1
        def diffs(a, b, label, width):
            out, n = [], 0
            for k in range(len(a)):
                if a[k] == b[k]:
                    continue
                n += 1
                if n <= int(os.environ.get("PVLIM", "4")):
                    out.append(f"{label} {k:#0{width}x}: "
                               f"orig {a[k]:#04x} C {b[k]:#04x}")
            if n > int(os.environ.get("PVLIM", "4")):
                out.append(f"(+{n - int(os.environ.get("PVLIM", "4"))} more {label})")
            return out

        bad = diffs(want_vram, got_vram, "vram", 6) + \
              diffs(want_img, got_img, "image", 7)

        which = RETURNS.get(off)
        if which and len(got_res) >= 3 and got_res[0]:
            got_val = got_res[1] | (got_res[2] << 8)
            # The original leaves its answer in a register half; the C
            # returns it as a value. Only the original's needs extracting -
            # shifting both turned every non-zero remainder into zero and made
            # this look like a mismatch when it was the comparison that was
            # wrong.
            want_val = ((want_ax >> 8) if which == "ah" else
                        want_cf if which == "cf" else
                        (want_cf ^ 1) if which == "ncf" else want_ax)
            if want_val != got_val:
                bad.append(f"returned {which}: orig {want_val:#06x} "
                           f"C {got_val:#06x}")
            else:
                did_work[off] += 1      # a matching answer counts as work
        if bad:
            if os.environ.get("PVSAVE"):
                open(os.environ["PVSAVE"], "wb").write(img)
                open(os.environ["PVSAVE"] + ".after", "wb").write(want_img)
                open(os.environ["PVSAVE"] + ".vin", "wb").write(vram)
                open(os.environ["PVSAVE"] + ".vwant", "wb").write(want_vram)
                open(os.environ["PVSAVE"] + ".vgot", "wb").write(got_vram)
            bad.append("regs " + " ".join(
                f"{n}={v:04x}" for n, v in zip(REGS, regs)))
        if want_img != img or want_vram != vram:
            did_work[off] += 1
        if bad:
            mismatched[off] += 1
            first_bad.setdefault(off, "; ".join(bad))
        elif args.verbose:
            print(f"  {ROUTINES[off]} ({off:#06x}) agrees "
                  f"[{checked[off]}]")

    m.uc.hook_add(unicorn.UC_HOOK_CODE, on_code, None, code, code + 0x10000)

    if args.resume:
        # sidebyside.py's snapshot, restored whole: image, video memory, all
        # fourteen registers and the BIOS tick the PRNG seeds from. The
        # alternative is playing to the level the routine appears on, which for
        # the animated bricks is ten minutes of emulation for three samples.
        d = open(args.resume, "rb").read()
        if d[:4] != b"PSNP":
            raise SystemExit(f"{args.resume}: not a snapshot")
        lv, fr = struct.unpack_from("<II", d, 4)
        o = 12
        regs = struct.unpack_from("<14H", d, o); o += 28
        ticks, = struct.unpack_from("<I", d, o); o += 4
        ilen, = struct.unpack_from("<I", d, o); o += 4
        img = d[o:o + ilen]; o += ilen
        vlen, = struct.unpack_from("<I", d, o); o += 4
        m.uc.mem_write(base, img)
        m.uc.mem_write(0xB8000, d[o:o + vlen])
        for reg, val in zip((UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX,
                             UC_X86_REG_DX, UC_X86_REG_SI, UC_X86_REG_DI,
                             UC_X86_REG_BP, UC_X86_REG_ES, UC_X86_REG_DS,
                             UC_X86_REG_EFLAGS, UC_X86_REG_SP, UC_X86_REG_SS,
                             UC_X86_REG_CS, UC_X86_REG_IP), regs):
            m.uc.reg_write(reg, val)
        m.uc.mem_write(0x46C, struct.pack("<I", ticks))
        pending.clear()
        started[0] = True
        print(f"resumed {os.path.basename(args.resume)}: "
              f"level {lv}, frame {fr}")

    # After the resume, so a snapshot's own route is cleared but these are not:
    # the point of --keys is to press something *from* the snapshot.
    for off, key, _ in extra_keys:
        pending.setdefault(off, collections.deque()).append(key)

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
            if interrupted[off]:
                note += (f", {interrupted[off]} dropped for a keyboard "
                         f"interrupt inside the call)")
            print(f"  ok   {name} ({off:#06x}): {note}")
    if never:
        print(f"  NOT REACHED, so unproven: {', '.join(never)}")
    if args.json:
        import json
        with open(args.json, "w") as f:
            json.dump({"route": (args.resume or
                                 ("menu" if args.menu else
                                  "keyboard" if args.keyboard else "play")),
                       "seconds": m._elapsed(),
                       "routines": {f"{off:#06x}": {
                           "name": ROUTINES[off],
                           "checked": checked[off],
                           "did_work": did_work[off],
                           "mismatched": mismatched[off],
                           "why": first_bad.get(off, "")}
                           for off in sorted(wanted)}}, f, indent=1)
    return 1 if fails or never else 0


if __name__ == "__main__":
    sys.exit(main())
