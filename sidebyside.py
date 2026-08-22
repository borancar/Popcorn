#!/usr/bin/env python3
"""
Play the emulator and the port side by side, frame by frame, on the same input.

`verify.py` settles one routine at a time on one call. That is the right tool
for "is this transcription right", and the wrong one for "does the game come
out the same", because a difference of a single pixel in frame three is a ball
on the other side of the paddle by frame ninety. This plays a real game on both
at once and compares everything after every frame.

The inputs are **driven**, not read. The bot decides where the paddle should go
from the emulator's memory - the reference - and the same number is handed to
both sides. Letting each read its own mouse would make a divergence in the
picture indistinguishable from a divergence in what the player did. The BIOS
tick count the PRNG is seeded from is handed across the same way.

Both start from the same state: the emulator plays the menu, and when it
reaches the play loop at 1ac2:1873 the whole machine is captured and given to
the port, which resumes from it. Neither side replays the menu twice.

The sync point is **1ac2:1c3f**, the `jmp` that closes the frame - the one
instruction both paths through it converge on. Not 0x1a62, its top: the serve
wait reaches that too, at 0x1a58, every time the action button is held, and a
bot holds it permanently.

    venv/bin/python sidebyside.py                 # 200 frames, stop on the first difference
    venv/bin/python sidebyside.py --frames 500
    venv/bin/python sidebyside.py --keep-going    # count them instead of stopping
    venv/bin/python sidebyside.py --window        # watch the emulator while it runs
"""
import argparse
import collections
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.join(HERE, "reconstruct", "popcorn")

CODE = 0x1AC20
PLAY_LOOP = 0x1873                      # one level
PLAY_SESSION = 0x02F5                   # a whole game, with --from-session
FRAME_END = 0x1C3F                      # `jmp 0x1a62`, the frame's close
#  - and NOT 0x1a62, its top: the serve wait jumps there too, at 0x1a58,
#    whenever the action button is held, so the top is hit more than once
#    a frame and the two sides end up compared at different points.
IMAGE_LEN = 0x208B0
CGA_SIZE = 0x4000
CGA_W, CGA_H = 320, 200
CGA_PLANE = 0x2000
CGA_STRIDE = 80
PADDLE_X = 0x2E54
LEVEL_NUMBER = 0x13CC                   # 0-0x31, for naming snapshots

# A snapshot is everything needed to start a level again: the unmasked image
# (the stack included, or the `ret` from play_loop goes nowhere), the screen,
# the registers with SP/SS/CS/IP, and the tick the PRNG is seeded from. One is
# written every time the emulator enters play_loop, which is once a level and
# again after a lost life - so a divergence twenty minutes in can be reached
# in seconds instead of replayed.
SNAP_MAGIC = b"PSNP"
SNAP_REGS = 14                          # ax bx cx dx si di bp es ds fl sp ss cs ip

# Excluded from the comparison for the same reasons verify.py excludes them:
# the stack below SP is not a function of anything, and the three key-state
# bytes are maintained by an interrupt handler on one side and by the platform
# layer on the other.
STACK_LO, STACK_HI = 0x1AA20, 0x1AC20
KEYS_LO, KEYS_HI = 0x2D4C, 0x2D4F

# The sound player's state at cs:[0xf4]-[0xf7] used to be masked here, because
# it diverged on the first frame and hid everything after it. It was a real
# bug - sound_tick read its tune pointers as image offsets when they are
# offsets into the code segment - and nothing is masked for it any more.

# The variables worth naming when something diverges, so the report says
# "the ball's y" rather than "image 0x2ea2". Kept deliberately short: this is
# for reading, and the byte offsets are in the output anyway.
NAMED = [
    (0x13C9, 1, "lives"),
    (0x13CC, 1, "level number"),
    (0x13CD, 8, "score"),
    (0x1487, 2, "frame delay"),
    (0x2D40, 1, "paddle repeat"),
    (0x2D4B, 1, "paddle divider"),
    (0x2E54, 1, "paddle x"),
    (0x2E73, 1, "balls left"),
    (0x2F10, 0xB0, "the level's cells"),
    (0x3138, 2, "entity free head"),
    (0x3144, 2, "entity list head"),
    (0x3146, 0x1C0, "entity pool"),
    (0x33D2, 2, "PRNG state"),
    (0x3F08, 1, "players"),
]
for i in range(4):
    b = 0x2EA1 + i * 0x1E
    NAMED += [(b + 0x00, 1, f"ball {i} x"), (b + 0x01, 1, f"ball {i} y"),
              (b + 0x14, 2, f"ball {i} direction"),
              (b + 0x16, 2, f"ball {i} slope"),
              (b + 0x18, 2, f"ball {i} anchor"),
              (b + 0x1A, 2, f"ball {i} accumulators"),
              (b + 0x1C, 1, f"ball {i} state")]


def name_of(off):
    for base, length, what in NAMED:
        if base <= off < base + length:
            return f"{what} (+{off - base})" if length > 1 else what
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=200,
                    help="0 runs until a comparison fails or you "
                         "stop it")
    ap.add_argument("--snapshots", metavar="DIR",
                    help="write a resumable snapshot at the start of every "
                         "level into DIR")
    ap.add_argument("--resume", metavar="FILE",
                    help="start from a snapshot instead of walking the menu")
    ap.add_argument("--from-session", action="store_true",
                    help="capture at play_session rather than play_loop, so "
                         "the comparison follows level transitions and lost "
                         "lives instead of ending with the level")
    ap.add_argument("--trace-from", type=int, default=-1, metavar="N",
                    help="from frame N, print one entity node and the PRNG on "
                         "both sides every frame")
    ap.add_argument("--trace-node", type=lambda v: int(v, 0), default=0x3154,
                    help="which node --trace-from follows")
    ap.add_argument("--inject", type=int, default=-1, metavar="N",
                    help="flip one byte of the port's image at frame N, to "
                         "prove the comparison can fail")
    ap.add_argument("--watch", action="store_true",
                    help="show the port's screen in a window (needs a display)")
    ap.add_argument("--snap", metavar="FILE",
                    help="write the port's screen to FILE as a PNG every "
                         "--snap-every frames, for watching from elsewhere")
    ap.add_argument("--snap-every", type=int, default=250)
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--no-sound", action="store_true",
                    help="clear cs:[0x84] on both sides before comparing, so "
                         "the note timer at cs:[0xf5] cannot diverge")
    ap.add_argument("--dump", metavar="DIR",
                    help="write emulator/port/diff PNGs for each "
                         "differing frame")
    ap.add_argument("--keep-going", action="store_true",
                    help="report every differing frame instead of the first")
    ap.add_argument("--enter-seconds", type=float, default=120.0,
                    help="how long to give the menu before giving up")
    args = ap.parse_args()

    sys.path.insert(0, HERE)
    import unicorn
    from unicorn.x86_const import (
        UC_X86_REG_SP, UC_X86_REG_SS,
        UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
        UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_ES,
        UC_X86_REG_DS, UC_X86_REG_EFLAGS, UC_X86_REG_CS, UC_X86_REG_IP)
    from emulation import VgaDos, KEYMAP, GAME_CODE
    from trace_dos import UNPACKED
    from autoplay import Bot, ROUTE_PLAY, parse_route

    m = VgaDos(UNPACKED, max_insns=1 << 62, cmdline=args.cmdline)
    base = m.load_seg * 16
    code = base + GAME_CODE
    bot = Bot(m, keyboard=False)

    def snapshot_image():
        return bytes(m.uc.mem_read(base, IMAGE_LEN))

    def snapshot_vram():
        return bytes(m.uc.mem_read(0xB8000, CGA_SIZE))

    def bios_ticks():
        lo, hi = struct.unpack("<HH", m.uc.mem_read(0x46C, 4))
        return (lo + hi) & 0xFFFF

    REGS_10 = (UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
               UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_ES,
               UC_X86_REG_DS, UC_X86_REG_EFLAGS)
    REGS_ALL = REGS_10 + (UC_X86_REG_SP, UC_X86_REG_SS,
                          UC_X86_REG_CS, UC_X86_REG_IP)

    def regs_now(which=REGS_10):
        return [m.uc.reg_read(x) & 0xFFFF for x in which]

    def raw_image():
        return bytes(m.uc.mem_read(base, IMAGE_LEN))   # stack and all

    def write_snapshot(path, level, frame):
        with open(path, "wb") as f:
            f.write(SNAP_MAGIC + struct.pack("<II", level, frame))
            f.write(struct.pack(f"<{SNAP_REGS}H", *regs_now(REGS_ALL)))
            f.write(struct.pack("<I", bios_ticks()))
            img = raw_image()
            f.write(struct.pack("<I", len(img)) + img)
            v = snapshot_vram()
            f.write(struct.pack("<I", len(v)) + v)

    def read_snapshot(path):
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
        return level, frame, regs, ticks, img, d[o:o + vlen]

    pending = {}
    for off, key, _ in parse_route(ROUTE_PLAY):
        pending.setdefault(off, collections.deque()).append(key)

    start_at = PLAY_SESSION if args.from_session else PLAY_LOOP
    captured = {}
    frame_hit = {}
    reentries = [0]
    resuming = [False]
    draws = []
    frames_done = [0]
    hits = collections.Counter()

    def on_code(uc, address, size, user):
        off = address - code
        q = pending.get(off)
        if q:
            sc, asc = KEYMAP[q.popleft()]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)
        if off == start_at and captured:
            reentries[0] += 1
        if args.snapshots and off == PLAY_LOOP and captured:
            lv = m.uc.mem_read(base + LEVEL_NUMBER, 1)[0]
            path = os.path.join(args.snapshots,
                                f"level{lv:02d}_f{frames_done[0]:06d}.snap")
            write_snapshot(path, lv, frames_done[0])
            print(f"  snapshot: level {lv} at frame {frames_done[0]} "
                  f"-> {os.path.basename(path)}", flush=True)
        if captured and off in (0x0097, 0x1AD8, 0x1AF5, 0x1B04, 0x1B4D, 0x1C3F):
            hits[off] += 1
        if captured and off == 0x40C0:          # game_random: who asked?
            sp = m._reg(UC_X86_REG_SP)
            ss = m._reg(UC_X86_REG_SS)
            ret, = struct.unpack("<H", m.uc.mem_read(ss * 16 + sp, 2))
            draws.append((ret, m._reg(UC_X86_REG_DX) & 0xff))
        if off == start_at and not captured:
            captured["regs"] = regs_now()
            captured["img"] = snapshot_image()
            captured["vram"] = snapshot_vram()
            captured["ticks"] = bios_ticks()
            uc.emu_stop()
        elif captured and off == FRAME_END:
            # emu_stop() leaves IP *at* this instruction, so the next
            # emu_start runs it again and the hook fires a second time with
            # no work done in between. Counting those as frames compares the
            # port's frame N+1 against the emulator's frame N.
            if resuming[0]:
                resuming[0] = False
                return
            frame_hit["img"] = snapshot_image()
            frame_hit["vram"] = snapshot_vram()
            resuming[0] = True
            uc.emu_stop()

    m.uc.hook_add(unicorn.UC_HOOK_CODE, on_code, None, code, code + 0x10000)

    def run_a_bit():
        """Let the guest run until a hook stops it, or a chunk goes by."""
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.blocked_on_input = False
        m.uc.emu_start(addr, 0, count=20000)
        m.service_keyboard()

    if args.resume:
        lv, fr, regs, ticks, img, vram = read_snapshot(args.resume)
        m.uc.mem_write(base, img)
        m.uc.mem_write(0xB8000, vram)
        for reg, val in zip(REGS_ALL, regs):
            m.uc.reg_write(reg, val)
        m.uc.mem_write(0x46C, struct.pack("<I", ticks))
        captured["regs"] = list(regs[:10])
        captured["img"] = img
        captured["vram"] = vram
        captured["ticks"] = ticks
        start_at = PLAY_LOOP            # a snapshot is always a play_loop entry
        frames_done[0] = fr
        print(f"resumed {os.path.basename(args.resume)}: level {lv}, "
              f"originally frame {fr}")

    print("walking the menu...") if not args.resume else None
    while not captured and m._elapsed() < args.enter_seconds:
        run_a_bit()
        if m.finished:
            break
    if not captured:
        print(f"never reached the play loop in {args.enter_seconds:.0f}s")
        return 1
    print(f"in a game after {m._elapsed():.0f}s; handing the state over")

    if args.no_sound:
        # F9's flag, in the code segment. Written to the emulator's memory and
        # to the copy the port gets, so both start from the same silence.
        off = GAME_CODE + 0x84
        m.uc.mem_write(base + off, b"\x00")
        img = bytearray(captured["img"])
        img[off] = 0
        captured["img"] = bytes(img)
        print("   sound off on both sides: emulator cs:[0x84]=%d, "
              "port copy=%d" % (m.uc.mem_read(base + off, 1)[0],
                               captured["img"][off]))

    state = os.path.join(os.environ.get("TMPDIR", "/tmp"),
                         "popcorn_lockstep.pvs")
    with open(state, "wb") as f:
        f.write(b"PVS2" + struct.pack("<I", start_at))
        f.write(struct.pack("<10H", *captured["regs"]))
        f.write(struct.pack("<I", captured["ticks"]))
        f.write(struct.pack("<I", len(captured["img"])) + captured["img"])
        f.write(struct.pack("<I", CGA_SIZE) + captured["vram"])

    port = subprocess.Popen([PORT, "--lockstep", state],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def read_exact(n):
        buf = b""
        while len(buf) < n:
            chunk = port.stdout.read(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def port_frame():
        head = read_exact(4)
        if head != b"PFRM":
            return None
        struct.unpack("<I", read_exact(4))
        n, = struct.unpack("<I", read_exact(4))
        img = read_exact(n)
        v, = struct.unpack("<I", read_exact(4))
        vram = read_exact(v)
        k, = struct.unpack("<I", read_exact(4))
        got = list(struct.unpack(f"<{k}H", read_exact(k * 2))) if k else []
        return img, vram, got

    def port_go(mouse_x, buttons, ticks, stop_it=0):
        try:
            port.stdin.write(struct.pack("<HHIB3x", mouse_x & 0xFFFF,
                                         buttons & 0xFFFF,
                                         ticks & 0xFFFFFFFF, stop_it))
            port.stdin.flush()
        except BrokenPipeError:
            pass

    view = {}

    def watch_open():
        import numpy as np
        if not args.watch:              # snapshots need no display at all
            os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
        import pygame
        pygame.init()
        view["np"] = np
        view["pygame"] = pygame
        if args.watch:
            pygame.display.set_caption("Popcorn - the port, in lockstep")
            view["screen"] = pygame.display.set_mode(
                (CGA_W * args.scale, CGA_H * args.scale))
        view["surf"] = pygame.Surface((CGA_W, CGA_H))
        # CGA's interlace, precomputed: which vram byte feeds each cell.
        view["idx"] = np.array(
            [[(CGA_PLANE if y & 1 else 0) + (y >> 1) * CGA_STRIDE + x
              for x in range(CGA_STRIDE)] for y in range(CGA_H)])
        view["pal"] = np.array([(0, 0, 0), (0x55, 0xff, 0xff),
                                (0xff, 0x55, 0x55), (0xff, 0xff, 0xff)],
                               dtype=np.uint8)

    def watch_draw(vram, n):
        np, pygame = view["np"], view["pygame"]
        b = np.frombuffer(vram, dtype=np.uint8)[view["idx"]]
        px = np.stack([(b >> 6) & 3, (b >> 4) & 3, (b >> 2) & 3, b & 3],
                      axis=-1).reshape(CGA_H, CGA_W)
        rgb = view["pal"][px]
        pygame.surfarray.blit_array(view["surf"], rgb.transpose(1, 0, 2))
        if args.snap and n % args.snap_every == 0:
            big = pygame.transform.scale(
                view["surf"], (CGA_W * args.scale, CGA_H * args.scale))
            pygame.image.save(big, args.snap)
        if not args.watch:
            return 1
        pygame.transform.scale(view["surf"], view["screen"].get_size(),
                               view["screen"])
        pygame.display.flip()
        for e in pygame.event.get():
            if e.type == pygame.QUIT:
                return 0
        return 1

    def save_screens(ev, pv, n):
        import pygame
        pygame.init()
        os.makedirs(args.dump, exist_ok=True)
        pal = [(0, 0, 0), (0x55, 0xff, 0xff), (0xff, 0x55, 0x55),
               (0xff, 0xff, 0xff)]

        def surf(v, other=None):
            s = pygame.Surface((320, 200))
            for y in range(200):
                row = (0x2000 if y & 1 else 0) + (y >> 1) * 80
                for xb in range(80):
                    byte = v[row + xb]
                    diff = other is not None and other[row + xb] != byte
                    for k in range(4):
                        c = (byte >> (6 - k * 2)) & 3
                        s.set_at((xb * 4 + k, y),
                                 (0xff, 0, 0xff) if diff else pal[c])
            return s

        pygame.image.save(surf(ev), f"{args.dump}/f{n:04d}_emulator.png")
        pygame.image.save(surf(pv), f"{args.dump}/f{n:04d}_port.png")
        pygame.image.save(surf(pv, ev), f"{args.dump}/f{n:04d}_diff.png")
        print(f"    wrote {args.dump}/f{n:04d}_*.png")

    def first_mouse():
        p = getattr(m, "mouse_pos", None)
        return p[0] if isinstance(p, tuple) else getattr(m, "mouse_x", 0)

    def mask(img):
        b = bytearray(img)
        b[STACK_LO:STACK_HI] = bytes(STACK_HI - STACK_LO)
        b[KEYS_LO:KEYS_HI] = bytes(KEYS_HI - KEYS_LO)
        return bytes(b)

    # The port gets its first input before it runs a single instruction, for
    # the same reason: its serve wait reads the action button too.
    bot.step()
    port_go(first_mouse(), getattr(m, "mouse_btn", 0), bios_ticks())

    if args.watch or args.snap:
        watch_open()

    differing, compared = 0, 0
    n = -1
    while args.frames == 0 or n + 1 < args.frames:
        n += 1
        pf = port_frame()
        if pf is None:
            print(f"the port stopped at frame {n}")
            break
        pimg, pvram, pdraws = pf

        frame_hit.clear()
        del draws[:]
        guard = 0
        while not frame_hit and guard < 4000 and not m.finished:
            run_a_bit()
            guard += 1
        if not frame_hit:
            print(f"the emulator did not reach a frame top at {n}")
            break
        compared = n + 1

        # The whole-buffer compare is a C memcmp; the per-byte walk only
        # happens when there is something to report. Without this the Python
        # loops cost more than running both emulators.
        if args.trace_from >= 0 and n >= args.trace_from:
            nd = args.trace_node
            ee, pp = frame_hit["img"], pimg
            print("  f%-5d node %s | %s   prng %02x%02x | %02x%02x" % (
                n,
                " ".join(f"{ee[nd + k]:02x}" for k in range(14)),
                " ".join(f"{pp[nd + k]:02x}" for k in range(14)),
                ee[0x33D3], ee[0x33D2], pp[0x33D3], pp[0x33D2]), flush=True)

        if n == args.inject:                # prove the check can fail
            pimg = bytearray(pimg)
            pimg[PADDLE_X] ^= 0x01
            pimg = bytes(pimg)
            print(f"  injected a one-bit change at frame {n}")

        a, b = mask(frame_hit["img"]), mask(pimg)
        ev = frame_hit["vram"]
        if a == b and ev == pvram:
            img_bad = vram_bad = []
        else:
            img_bad = [i for i in range(IMAGE_LEN) if a[i] != b[i]]
            vram_bad = [i for i in range(CGA_SIZE) if ev[i] != pvram[i]]

        if img_bad or vram_bad:
            differing += 1
            lv = frame_hit["img"][LEVEL_NUMBER]
            print(f"\nframe {n}, level {lv}: {len(img_bad)} image bytes, "
                  f"{len(vram_bad)} screen bytes differ")
            seen = set()
            for off in img_bad:
                what = name_of(off)
                key = what or f"{off:#07x}"
                if key in seen:
                    continue
                seen.add(key)
                print(f"    {off:#07x} {what or '(unnamed)':<24s} "
                      f"emulator {a[off]:#04x}  port {b[off]:#04x}")
                if len(seen) >= 20:
                    print(f"    ... and {len(img_bad) - 20} more image bytes")
                    break
            if draws or pdraws:
                print("    random() this frame - emulator: " +
                      (", ".join(f"{a:#06x}/{dl}" for a, dl in draws)
                       or "none"))
                print("                          port: " +
                      (", ".join(str(dl) for dl in pdraws) or "none"))
            if vram_bad:
                rows = collections.Counter()
                xs = []
                for off in vram_bad:
                    o = off - 0x2000 if off >= 0x2000 else off
                    y = (o // 80) * 2 + (1 if off >= 0x2000 else 0)
                    rows[y] += 1
                    xs.append((o % 80) * 4)
                ys = sorted(rows)
                print(f"    screen: {len(vram_bad)} bytes over "
                      f"{len(ys)} scan lines, y {ys[0]}-{ys[-1]}, "
                      f"x {min(xs)}-{max(xs) + 3}")
                worst = rows.most_common(3)
                print("    busiest lines: " +
                      ", ".join(f"y{y} ({c})" for y, c in worst))
                if args.dump:
                    save_screens(frame_hit["vram"], pvram, n)
            if not args.keep_going:
                print("\nstopping at the first difference "
                      "(--keep-going to carry on)")
                port_go(0, 0, 0, stop_it=1)
                port.wait(timeout=5)
                return 1

        if (args.watch or args.snap) and not watch_draw(pvram, n):
            print(f"\nwindow closed at frame {n}")
            break
        frames_done[0] = n
        if n and n % 250 == 0:
            print(f"  {n} frames, still identical "
                  f"(level {frame_hit['img'][LEVEL_NUMBER]})", flush=True)

        # The bot reads the emulator - the reference - and both are told the
        # same thing.
        bot.step()
        port_go(first_mouse(), getattr(m, "mouse_btn", 0), bios_ticks())

    port_go(0, 0, 0, stop_it=1)
    try:
        port.wait(timeout=5)
    except Exception:
        port.kill()
    print(f"the emulator re-entered {start_at:#06x} {reentries[0]} times")
    print("   " + ", ".join(f"{k:#06x}x{v}" for k, v in sorted(hits.items())))
    if differing:
        print(f"\n{compared} frames compared, {differing} differed")
        return 1
    print(f"\n{compared} frames compared, identical throughout")
    return 0


if __name__ == "__main__":
    sys.exit(main())
