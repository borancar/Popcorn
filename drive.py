"""Drive a DOS program under the emulator by key, deterministically.

`emulation.py --keys` times its presses against the **wall clock**, which is
right for the game - it is always running, and a press has to land while some
particular thing is on screen. It is wrong for a program that sits waiting for
input, because the emulator's speed varies with what the guest is doing: the
same script reaches the editor on one run and misses it on the next. Every
attempt to drive POPGEN that way landed somewhere different.

So the cue here is the guest's own: it has drained the keyboard buffer and gone
back to asking for more. Feed the next key when the buffer has been empty for
`--settle` slices, and a script means the same thing every time however fast
the host is.

    uv run drive.py --exe popcorn/popgen.exe \
        --keys right,return,@editor,f1,right,f2,@placed --shot-dir debug/pg

Keys are comma-separated: a name from the table below, a single character, or
`^c` for shift and that character - the same scan code with the upper-case
ASCII, which is what the BIOS would hand the guest. `@name` is not a key: it
saves a screenshot under that name, so a run documents itself.

`--dump FILE` writes the guest's low 256K afterwards. That is how POPGEN's
palette was measured: press one key, dump, and diff against a run that pressed
nothing - the byte that differs is the one that key writes.
"""
import argparse
import os
import sys

NAMED = {
    "esc": (0x01, 0x1B), "return": (0x1C, 0x0D), "enter": (0x1C, 0x0D),
    "space": (0x39, 0x20), "backspace": (0x0E, 0x08), "tab": (0x0F, 0x09),
    "up": (0x48, 0x00), "down": (0x50, 0x00),
    "left": (0x4B, 0x00), "right": (0x4D, 0x00),
    "home": (0x47, 0x00), "end": (0x4F, 0x00),
    "pgup": (0x49, 0x00), "pgdn": (0x51, 0x00),
    "ins": (0x52, 0x00), "del": (0x53, 0x00),
    "plus": (0x0D, 0x2B), "minus": (0x0C, 0x2D),
    "kpplus": (0x4E, 0x2B), "kpminus": (0x4A, 0x2D),
}
for _i in range(1, 11):                       # F1 is 0x3b - 0x3a is Caps Lock
    NAMED[f"f{_i}"] = (0x3A + _i, 0x00)
CHARS = {}
for _row, _base in (("qwertyuiop", 0x10), ("asdfghjkl", 0x1E), ("zxcvbnm", 0x2C)):
    for _i, _c in enumerate(_row):
        CHARS[_c] = (_base + _i, ord(_c))
for _i in range(1, 10):
    CHARS[str(_i)] = (0x02 + _i - 1, ord(str(_i)))
CHARS["0"] = (0x0B, ord("0"))
CHARS.update({" ": (0x39, 0x20), ".": (0x34, 0x2E), ",": (0x33, 0x2C),
              "-": (0x0C, 0x2D), "=": (0x0D, 0x3D), "/": (0x35, 0x2F),
              ";": (0x27, 0x3B), "[": (0x1A, 0x5B), "]": (0x1B, 0x5D)})


def resolve(tok):
    """One token to (scancode, ascii), or None if it is not a key."""
    if tok in NAMED:
        return NAMED[tok]
    if len(tok) == 2 and tok[0] == "^" and tok[1] in CHARS:
        sc, _ = CHARS[tok[1]]
        return (sc, ord(tok[1].upper()))
    if tok in CHARS:
        return CHARS[tok]
    raise SystemExit(f"drive.py: no such key {tok!r}")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, help="the DOS executable to run")
    ap.add_argument("--keys", default="",
                    help="comma-separated keys and @screenshot tags")
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--shot-dir", default="debug",
                    help="where @tags and the final screenshot are written")
    ap.add_argument("--dump", metavar="FILE",
                    help="write the guest's low 256K when the script is done")
    ap.add_argument("--settle", type=int, default=80,
                    help="slices the keyboard buffer must stay empty before "
                         "the next key. Higher is slower and safer; a program "
                         "that redraws a whole screen between keys needs more")
    ap.add_argument("--seconds", type=float, default=240.0,
                    help="give up after this much emulated time")
    ap.add_argument("--watch", action="store_true",
                    help="show it in a window while it runs. Headless is the "
                         "default because most uses are unattended, but a run "
                         "nobody can see is a run nobody can check")
    ap.add_argument("--scale", type=int, default=3)
    args = ap.parse_args()

    if not args.watch:
        os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
    import pygame
    pygame.init()
    pygame.font.init()
    from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP
    from emulation import VgaDos, make_surface

    fp = pygame.font.match_font("dejavusansmono,liberationmono,monospace")
    font = pygame.font.Font(fp, 13) if fp else pygame.font.SysFont(None, 16)

    script = [t.strip() for t in args.keys.split(",") if t.strip()]
    for t in script:                          # fail before anything runs
        if not t.startswith("@"):
            resolve(t)
    os.makedirs(args.shot_dir, exist_ok=True)

    m = VgaDos(args.exe, max_insns=1 << 62, cmdline=args.cmdline)
    screen = None
    if args.watch:
        screen = pygame.display.set_mode((320 * args.scale, 200 * args.scale))
        pygame.display.set_caption(f"drive.py - {os.path.basename(args.exe)}")

    def shot(tag):
        surf = make_surface(m, font, (8, 16))
        pygame.image.save(surf, os.path.join(args.shot_dir, f"{tag}.png"))
        print(f"  shot {tag} ({'text' if m.text_mode else 'graphics'})")

    idle, step = 0, 0
    addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
    while step <= len(script) and m._elapsed() < args.seconds:
        m.blocked_on_input = False
        m.uc.emu_start(addr, 0, count=20_000)
        if m.finished:
            print("  guest exited")
            break
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.service_keyboard()
        if screen is not None:
            for ev in pygame.event.get():
                if ev.type == pygame.QUIT:
                    step = len(script) + 1
            surf = make_surface(m, font, (8, 16)).convert(screen)
            pygame.transform.scale(surf, screen.get_size(), screen)
            pygame.display.flip()

        # The cue: the guest has taken everything and come back for more.
        idle = idle + 1 if not m.key_buf else 0
        if idle < args.settle:
            continue
        idle = 0
        if step == len(script):
            shot("final")
            step += 1
            break
        tok = script[step]
        step += 1
        if tok.startswith("@"):
            shot(tok[1:])
        else:
            m.key_buf.append(resolve(tok))
            print(f"  key {tok}")

    if step <= len(script):
        print(f"  gave up after {args.seconds}s with {len(script)-step} "
              f"tokens left - raise --seconds, or --settle if keys are being "
              f"dropped")
    if args.dump:
        open(args.dump, "wb").write(bytes(m.uc.mem_read(0, 0x40000)))
        print(f"  wrote {args.dump}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
