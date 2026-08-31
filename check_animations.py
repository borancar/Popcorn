#!/usr/bin/env python3
"""Do the animated bricks get hit, and does the level keep running once they are?

Nine of the fifty levels carry a 2x3 block of animated bricks - one 32x24
picture cut into six brick cells.  A cell is static until it is hit; the hit
turns it into cells 24..29 and hands it to entity_anim_brick, which redraws it
every time anim_step steps the level's cursor.  So "the block is playing" is a
fact about the **cells**, not about the screen: all six in 24..29, and the
level still running.

This watches globals and nothing else - sidebyside is where video memory gets
compared, and it answers a different question.  It runs the emulator alone
with the bot, one frame at a time, and stops at the first of:

    all six tiles hit and held for 100 frames    - the block played, PASS
    level.bricks reaches 0                       - the level was cleared
    the game is over                             - the bot lost its lives
    1ac2:4210                                    - it fell into the bonus

The end-of-level capsule is rigged out: odds[8] takes odds[7]'s value, so `+`
has zero width and cannot skip a level out from under the test.
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import snapshot as snap

FRAME_END, BONUS_BODY = 0x1C3F, 0x4210
LIVES, LEVEL_NUMBER = 0x13C9, 0x13CC
BRICKS, CELLS, GAME_OVER = 0x2F10, 0x2F18, 0x2E78
ODDS = 0x33B1
HOLD = 100


def run(panel, cap, verbose, seed=0):
    import unicorn
    from unicorn.x86_const import (
        UC_X86_REG_SP, UC_X86_REG_SS, UC_X86_REG_AX, UC_X86_REG_BX,
        UC_X86_REG_CX, UC_X86_REG_DX, UC_X86_REG_SI, UC_X86_REG_DI,
        UC_X86_REG_BP, UC_X86_REG_ES, UC_X86_REG_DS, UC_X86_REG_EFLAGS,
        UC_X86_REG_CS, UC_X86_REG_IP)
    from emulation import VgaDos, GAME_CODE, UNPACKED
    from autoplay import Bot

    REGS = (UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
            UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_ES,
            UC_X86_REG_DS, UC_X86_REG_EFLAGS, UC_X86_REG_SP, UC_X86_REG_SS,
            UC_X86_REG_CS, UC_X86_REG_IP)

    m = VgaDos(UNPACKED, max_insns=1 << 62)
    base = m.load_seg * 16
    code = base + GAME_CODE
    bot = Bot(m, keyboard=False, seed=seed)

    lv, fr, regs, ticks, img, vram, extra, low = snap.read(
        os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     f'snapshots/L{panel:02d}.snap'))
    img = bytearray(img)
    img[ODDS + 8] = img[ODDS + 7]        # `+` gets zero width
    bot.setstate(extra)
    if low:
        m.uc.mem_write(0, low)
    m.uc.mem_write(base, bytes(img))
    m.uc.mem_write(0xB8000, vram)
    for r, v in zip(REGS, regs):
        m.uc.reg_write(r, v)
    m.uc.mem_write(0x46C, struct.pack("<I", ticks))

    stop, resuming, saw_bonus = {'f': False}, [None], {'v': False}

    def on_code(uc, addr, size, user):
        off = addr - code
        if off == BONUS_BODY:
            saw_bonus['v'] = True
        if off == FRAME_END:
            if resuming[0] == off:
                resuming[0] = None
                return
            resuming[0] = off
            stop['f'] = True
            uc.emu_stop()

    m.uc.hook_add(unicorn.UC_HOOK_CODE, on_code, None, code, code + 0x10000)

    def peek(off, n=1):
        return bytes(m.uc.mem_read(base + off, n))

    # The snapshot sits on the level *before* the one wanted, with the brick
    # count already cleared - that is how sweep_levels.sh makes play_session
    # load the next one normally.  So nothing is judged until the wanted level
    # is actually up: level_number where it should be, and bricks on the field.
    want = panel - 1
    frames = held = 0
    all_hit_at = None
    best = 0
    armed = False
    while frames < cap:
        stop['f'] = False
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.blocked_on_input = False
        m.uc.emu_start(addr, 0, count=20000)
        m.service_keyboard()
        if not stop['f']:
            continue
        frames += 1
        bot.step()

        cells = peek(CELLS, 168)
        hit = sum(1 for c in cells if 24 <= c <= 29)
        bricks, lives, over = peek(BRICKS)[0], peek(LIVES)[0], peek(GAME_OVER)[0]
        level = peek(LEVEL_NUMBER)[0]

        if not armed:
            if level == want and bricks > 0:
                armed = True
                if verbose:
                    print(f"      level {want} up at frame {frames}, "
                          f"{bricks} bricks", flush=True)
            continue
        best = max(best, hit)

        if hit == 6:
            if all_hit_at is None:
                all_hit_at = frames
                if verbose:
                    print(f"      all six hit at frame {frames}", flush=True)
            held = frames - all_hit_at
        else:
            all_hit_at, held = None, 0

        if saw_bonus['v']:
            return ('bonus', frames, best, held, bricks, lives)
        if over:
            return ('game over', frames, best, held, bricks, lives)
        if bricks == 0:
            return ('level clear', frames, best, held, bricks, lives)
        if held >= HOLD:
            return ('played', frames, best, held, bricks, lives)
    return ('cap', frames, best, held, bricks, lives)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('panels', nargs='*', type=int,
                    default=[8, 10, 21, 24, 31, 35, 40, 43, 46])
    ap.add_argument('--cap', type=int, default=60000)
    ap.add_argument('--seeds', default='0',
                    help='bot seeds to try, in order, until one proves it')
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()
    seeds = [int(x) for x in a.seeds.split(',')]
    print(f"{'panel':>5} {'seed':>4} {'outcome':<12} {'frames':>7} {'tiles':>6} "
          f"{'held':>5} {'bricks':>7} {'lives':>6}")
    bad = 0
    for p in a.panels:
        for seed in seeds:
            why, fr, best, held, bricks, lives = run(p, a.cap, a.verbose, seed)
            # The stop conditions that prove it: all six animating and held for
            # HOLD frames, or the level finished with all six having animated.
            # A clear is a legitimate end - the block played, the game moved on.
            ok = best == 6 and why in ('played', 'level clear')
            print(f"{p:>5} {seed:>4} {why:<12} {fr:>7} {best:>4}/6 {held:>5} "
                  f"{bricks:>7} {lives:>6}"
                  f"{'' if ok else '   <-- not proven'}", flush=True)
            if ok:
                break
        bad += not ok
    print(f"\n{len(a.panels) - bad}/{len(a.panels)} proven: all six animating, "
          f"held {HOLD} frames or the level cleared with them running")
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
