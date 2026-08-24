#!/usr/bin/env python3
"""
Compare the C port's screen against the emulator's, byte for byte.

Looking right is not the same as being right: a blit that is two bytes out
still draws something, and at 320x200 in four colours the eye forgives a lot.
This runs both to the same point, dumps the 0xb8000 aperture from each, and
diffs them - so "the menu is correct" is a number rather than an impression.

The emulator is the reference. It runs the original code; the port does not.

Usage:
    python compare_screen.py                  # the menu, after the intro
    python compare_screen.py --seconds 40 --save debug/diff.png
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.join(HERE, "reconstruct", "popcorn-dev")


def emulator_vram(seconds, cmdline):
    os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
    import pygame
    pygame.init()
    from emulation import VgaDos
    from trace_dos import UNPACKED
    from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP

    m = VgaDos(UNPACKED, max_insns=1 << 62, cmdline=cmdline)
    addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
    while m._elapsed() < seconds:
        m.blocked_on_input = False
        m.uc.emu_start(addr, 0, count=20000)
        if m.finished:
            break
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.service_keyboard()
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
    return bytes(m.uc.mem_read(0xB8000, 0x4000))


def port_vram(ms, out):
    env = dict(os.environ, SDL_VIDEODRIVER="offscreen")
    r = subprocess.run([PORT, "--scale", "1", "--run-ms", str(ms),
                        "--dump-vram", out],
                       env=env, capture_output=True, text=True,
                       timeout=ms / 1000 + 60)
    if not os.path.exists(out):
        raise SystemExit(f"the port wrote no dump: {r.stderr.strip()}")
    return open(out, "rb").read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=32.0,
                    help="how long to let each run before comparing")
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--save", default="",
                    help="write a three-panel PNG: emulator, port, difference")
    a = ap.parse_args()

    if not os.path.exists(PORT):
        raise SystemExit("reconstruct/popcorn-dev is not built")

    tmp = os.path.join(HERE, "debug", "port_vram.bin")
    os.makedirs(os.path.dirname(tmp), exist_ok=True)
    want = emulator_vram(a.seconds, a.cmdline)
    got = port_vram(int(a.seconds * 1000), tmp)

    # Only the 200 visible scan lines matter; the 192 bytes of padding at the
    # end of each half are never displayed and never written by either side.
    rows = [(y, (0x2000 if y & 1 else 0) + (y >> 1) * 80) for y in range(200)]
    bad_rows, bad_bytes = [], 0
    for y, base in rows:
        d = sum(1 for x in range(80) if want[base + x] != got[base + x])
        if d:
            bad_rows.append((y, d))
            bad_bytes += d
    total = 200 * 80
    print(f"visible screen: {total - bad_bytes} of {total} bytes identical "
          f"({100.0 * (total - bad_bytes) / total:.2f}%)")
    if bad_rows:
        print(f"  {len(bad_rows)} of 200 scan lines differ; "
              f"worst: {sorted(bad_rows, key=lambda r: -r[1])[:6]}")
    else:
        print("  the port's screen is the emulator's, exactly")

    if a.save:
        import pygame
        pygame.init()
        pal = [(0, 0, 0), (85, 255, 255), (255, 85, 85), (255, 255, 255)]

        def surf(buf, mark=None):
            s = pygame.Surface((320, 200))
            for y, base in rows:
                for x in range(80):
                    b = buf[base + x]
                    for k in range(4):
                        s.set_at((x * 4 + k, y), pal[(b >> (6 - 2 * k)) & 3])
            return s

        out = pygame.Surface((320, 200 * 3 + 8))
        out.fill((40, 40, 40))
        out.blit(surf(want), (0, 0))
        out.blit(surf(got), (0, 204))
        diff = pygame.Surface((320, 200))
        for y, base in rows:
            for x in range(80):
                c = (255, 0, 255) if want[base + x] != got[base + x] else (0, 0, 0)
                for k in range(4):
                    diff.set_at((x * 4 + k, y), c)
        out.blit(diff, (0, 408))
        pygame.image.save(pygame.transform.scale_by(out, 2), a.save)
        print(f"  wrote {a.save}: emulator, port, difference")
    return 1 if bad_bytes else 0


if __name__ == "__main__":
    sys.exit(main())
