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

    uv run sidebyside.py                 # 200 frames, stop on the first difference
    uv run sidebyside.py --frames 500
    uv run sidebyside.py --keep-going    # count them instead of stopping
    uv run sidebyside.py --watch         # both screens, side by side
    uv run sidebyside.py --watch --play  # ...and you drive them, with one mouse
"""
import argparse
import collections
import json
import os
import select
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.join(HERE, "reconstruct", "popcorn-dev")

CODE = 0x1AC20
PLAY_LOOP = 0x1873                      # one level
PLAY_SESSION = 0x02F5                   # a whole game, with --from-session
BONUS_BODY = 0x4210                     # the end-of-level bonus, --from-bonus
# The ending animation, --sync-curtain: the curtain's pass and panel_finish's.
CURTAIN = (0x467F, 0x09D1, 0x4769)
ENDING = (0x596C, 0x59C0, 0x59E3)       # after level 49, --sync-ending
INTRO = (0x1EC4, 0x1EE0, 0x1F13)        # the level intro, --sync-intro

FRAME_END = 0x1C3F                      # `jmp 0x1a62`, the frame's close
# The opt-in second sync point, --sync-scroll. screen_scroll_up is called once
# per scrolled row by every screen that has a loop of its own, so taking it as
# a comparison point turns the end-level bonus from one indivisible step into
# fifty, and a difference gets a row number instead of a shrug.
SCROLL_UP = 0x4878
BALL_ENDGAME = 0x45A1                   # --sync-endgame, once a ball step
RESULTS_WAIT = 0x1037                   # --sync-results, once a wait pass

# Every extra sync point, and the kind the port tags it with - SYNC_SCROLL 1,
# SYNC_ENDGAME 2, SYNC_RESULTS 4, SYNC_CURTAIN 8, SYNC_ENDING 16,
# SYNC_INTRO 32, from game.h.
#
# This used to be `1 if off == SCROLL_UP else 2` over those two offsets alone,
# and every kind added afterwards stopped the emulator here while tagging
# nothing. The port tags all of them, so the check below saw a tag on one side
# and none on the other and reported the two as standing in different places -
# at every single one. --sync-curtain looked like the whole curtain differing
# and was really the driver not knowing the word for it.
SYNC_KIND = {SCROLL_UP: 1, BALL_ENDGAME: 2, RESULTS_WAIT: 4}
SYNC_KIND.update({o: 8 for o in CURTAIN})
SYNC_KIND.update({o: 16 for o in ENDING})
SYNC_KIND.update({o: 32 for o in INTRO})
SYNC_NAME = {1: "scroll", 2: "endgame", 4: "results",
             8: "curtain", 16: "ending", 32: "intro"}
#  - and NOT 0x1a62, its top: the serve wait jumps there too, at 0x1a58,
#    whenever the action button is held, so the top is hit more than once
#    a frame and the two sides end up compared at different points.
IMAGE_LEN = 0x208B0
CGA_SIZE = 0x4000
CGA_W, CGA_H = 320, 200
CGA_PLANE = 0x2000
CGA_STRIDE = 80
# --watch shows both screens at once. The gap is there so the eye can tell
# which picture is which without a caption, and it is part of the window's
# coordinates, so the mouse mapping has to subtract it.
WATCH_GAP = 8
PANES_W = CGA_W * 2 + WATCH_GAP
PADDLE_X = 0x2E54

# The brick behaviour table at 0x3044, by cell value - so a report can say
# which brick was hit rather than only that one was.
BRICKS = {0x28CB: 1, 0x2985: 2, 0x2A3F: 3, 0x3221: 4, 0x2A73: 5, 0x2AB4: 6,
          0x2AF5: 7, 0x2B36: 8, 0x2B9D: 9, 0x2C59: 10, 0x2D68: 11,
          0x2CCD: 16}
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
    ap.add_argument("--from-bonus", action="store_true",
                    help="the snapshot was taken at 1ac2:4210, the end-of-"
                         "level bonus, so resume there rather than in the play "
                         "loop. That screen needs a + capsule to reach and + "
                         "is 2 chances in 255 - injecting the capture is the "
                         "only way it is a regression rather than a wait. "
                         "Pair it with --sync-endgame, which is what compares "
                         "the ball inside it")
    ap.add_argument("--sync-intro", action="store_true",
                    help="also compare the level intro, 1ac2:1ec4, 1ac2:1ee0 "
                         "and 1ac2:1f13 a pass. It runs before the play loop "
                         "starts, so io_frame_sync has not begun and none of "
                         "it is compared by anything")
    ap.add_argument("--sync-ending", action="store_true",
                    help="also compare the sequence after the fiftieth level "
                         "is cleared, 1ac2:596c a pass. screen_all_levels_done "
                         "has no sync of its own, so without this the ending "
                         "is compared by nothing and the driver cannot tell "
                         "it from a hang")
    ap.add_argument("--sync-curtain", action="store_true",
                    help="also compare the ending animation - the curtain "
                         "that closes a level and the panel that follows it. "
                         "Without it that whole sequence has no sync point, so "
                         "it is compared by nothing and --watch shows it as a "
                         "jump: 1ac2:467f a pass and 1ac2:09d1 a pass")
    ap.add_argument("--sync-results", action="store_true",
                    help="also compare the results screen after a game over - "
                         "the two-player bar and the wait that follows it. "
                         "io_frame_sync lives in the play loop, so by the time "
                         "that screen is drawn there is nothing comparing the "
                         "two sides at all; this is 1ac2:1037, its wait")
    ap.add_argument("--sync-endgame", action="store_true",
                    help="also compare at every ball_after_endgame, which is "
                         "once per step of the end-level bonus's own ball "
                         "loop - the only part of that screen the scroll sync "
                         "does not reach")
    ap.add_argument("--sync-scroll", action="store_true",
                    help="also compare at every screen_scroll_up, so a screen "
                         "with a loop of its own - the end-level bonus, the "
                         "game over, the ending - stops being one indivisible "
                         "step. Without it the driver can say the two sides "
                         "differ afterwards and nothing about where")
    ap.add_argument("--snap-at", type=int, default=0, metavar="FRAME",
                    help="write a snapshot at this frame and stop. The number "
                         "is the one the difference report prints, which on a "
                         "resumed run counts from zero. A divergence is found "
                         "*at* a frame; verifying the routines behind it wants "
                         "the state a few frames before, and no level-start "
                         "snapshot is close enough")
    ap.add_argument("--snap-at-file", default="at.snap", metavar="FILE")
    ap.add_argument("--snapshots", metavar="DIR",
                    help="write a resumable snapshot at the start of every "
                         "level into DIR")
    ap.add_argument("--resume", metavar="FILE",
                    help="start from a snapshot instead of walking the menu")
    ap.add_argument("--from-session", action="store_true",
                    help="capture at play_session rather than play_loop, so "
                         "the comparison follows level transitions and lost "
                         "lives instead of ending with the level")
    ap.add_argument("--watch-write", type=lambda v: int(v, 0), default=-1,
                    metavar="ADDR",
                    help="report every write the emulator makes to this image "
                         "address, with the instruction that made it")
    ap.add_argument("--trace-from", type=int, default=-1, metavar="N",
                    help="from frame N, print one entity node and the PRNG on "
                         "both sides every frame")
    ap.add_argument("--trace-node", type=lambda v: int(v, 0), default=0x3154,
                    help="which node --trace-from follows")
    ap.add_argument("--inject", type=int, default=-1, metavar="N",
                    help="flip one byte of the port's image at frame N, to "
                         "prove the comparison can fail")
    ap.add_argument("--watch", action="store_true",
                    help="show both screens side by side in one window, the "
                         "emulator on the left and the port on the right "
                         "(needs a display)")
    ap.add_argument("--play", action="store_true",
                    help="drive both sides from your own mouse instead of "
                         "the bot. Implies --watch")
    ap.add_argument("--snap", metavar="FILE",
                    help="write the port's screen to FILE as a PNG every "
                         "--snap-every frames, for watching from elsewhere")
    ap.add_argument("--snap-every", type=int, default=250)
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--mask-keys", action="store_true",
                    help="exclude the three key-state bytes at 0x2d4c too. "
                         "Driven by the same input every frame the two sides "
                         "agree on them, so this is off by default now; a run "
                         "that drives the keyboard may still want it")
    ap.add_argument("--no-mask", action="store_true",
                    help="compare the stack below SP and the three key-state "
                         "bytes too. Both are excluded for reasons about the "
                         "harness rather than the port, and an exclusion "
                         "added to get past a real bug is easy to leave in "
                         "place after the bug is gone")
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
    # There is nowhere to read a mouse from without a window.
    if args.play:
        args.watch = True

    sys.path.insert(0, HERE)
    import unicorn
    from unicorn.x86_const import (
        UC_X86_REG_SP, UC_X86_REG_SS,
        UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
        UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_ES,
        UC_X86_REG_DS, UC_X86_REG_EFLAGS, UC_X86_REG_CS, UC_X86_REG_IP)
    from emulation import VgaDos, KEYMAP, GAME_CODE
    from emulation import UNPACKED
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

    def raw_ticks():
        """The counter itself, not what the PRNG folds it into.

        game_random adds the two words at 0040:006c and keeps sixteen bits, so
        a snapshot that stored the fold restored a *different* counter - one
        whose high word was zero. Everything seeded from it then walked a
        different sequence, and a snapshot taken beside a divergence did not
        reproduce it while one taken at the level start did, which is a
        confusing way to find out.
        """
        return struct.unpack("<I", m.uc.mem_read(0x46C, 4))[0]

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
            f.write(struct.pack("<I", raw_ticks()))
            img = raw_image()
            f.write(struct.pack("<I", len(img)) + img)
            v = snapshot_vram()
            f.write(struct.pack("<I", len(v)) + v)
            # The bot is state too. Without it a resume plays differently from
            # the first frame, and a divergence found in a long run cannot be
            # reached again from the snapshot written beside it - which is
            # exactly what happened to the one at frame 33,166.
            blob = json.dumps(bot.getstate()).encode()
            f.write(struct.pack("<I", len(blob)) + blob)
            # And **low memory**: the interrupt vector table and the BIOS data
            # area, neither of which is inside the load image. The game
            # installs its own INT 09h for play and takes it out again for the
            # menus, so a resume that boots fresh and overwrites only the image
            # has the BIOS handler where the running game had the game's.
            low = bytes(m.uc.mem_read(0, 0x500))
            f.write(b"LOWM" + struct.pack("<I", len(low)) + low)

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

    pending = {}
    for off, key, _ in parse_route(ROUTE_PLAY):
        pending.setdefault(off, collections.deque()).append(key)

    desynced = [False]
    start_at = (BONUS_BODY if args.from_bonus
                else PLAY_SESSION if args.from_session else PLAY_LOOP)
    captured = {}
    frame_hit = {}
    reentries = [0]
    resuming = [None]
    draws = []
    frames_done = [0]
    want_snap = [False]
    stop_now = [False]
    # A resumed run counts frames from zero - that is what the difference
    # report prints and so what --snap-at has to mean - but a snapshot's name
    # should carry on from where the one it resumed left off.
    base_frame = [0]
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
            want_snap[0] = True         # at the next frame close, not here
        if (args.snap_at and off == FRAME_END and captured
                and not resuming[0] and frames_done[0] + 1 >= args.snap_at):
            lv = m.uc.mem_read(base + LEVEL_NUMBER, 1)[0]
            write_snapshot(args.snap_at_file, lv,
                           base_frame[0] + frames_done[0])
            print(f"  snapshot at frame {frames_done[0]} -> "
                  f"{args.snap_at_file}", flush=True)
            stop_now[0] = True
            m.uc.emu_stop()
        if (args.snapshots and want_snap[0] and off == FRAME_END
                and not resuming[0]):
            want_snap[0] = False
            lv = m.uc.mem_read(base + LEVEL_NUMBER, 1)[0]
            path = os.path.join(args.snapshots,
                                f"level{lv:02d}_f"
                                f"{base_frame[0] + frames_done[0]:06d}.snap")
            write_snapshot(path, lv, base_frame[0] + frames_done[0])
            print(f"  snapshot: level {lv} at frame {frames_done[0]} "
                  f"-> {os.path.basename(path)}", flush=True)
        if captured and off in (0x0097, 0x1AD8, 0x1AF5, 0x1B04, 0x1B4D, 0x1C3F):
            hits[off] += 1
        if captured and off == 0x1FC1:          # field_backdrop
            draws.append((0x1fc1, 0x1fc1))
        # the brick handlers, by the cell value that reaches them
        k = BRICKS.get(off)
        if captured and k is not None:
            draws.append((0x8000 | k, 0x8000 | k))
        # play_session's decision points, so the trace can say what the
        # emulator did on a lost life rather than what it did not do.
        # play_loop's four exits, so a report can say *which* one ended the
        # level rather than only that something did: 0x1a9b the last ball
        # gone, 0x1aac the bricks gone, 0x1b2b a ball lost inside the walk,
        # 0x1b3d the last brick broken inside it.
        if captured and off in (0x0352, 0x036E, 0x034C, 0x0376,
                                0x1EB9, 0x0D2E, 0x0473, 0x0374,
                                0x1A9B, 0x1AAC, 0x1B2B, 0x1B3D):
            draws.append((off, 0))
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
        elif captured and (off == FRAME_END
                           or (args.sync_scroll and off == SCROLL_UP)
                           or (args.sync_endgame and off == BALL_ENDGAME)
                           or (args.sync_results and off == RESULTS_WAIT)
                           or (args.sync_curtain and off in CURTAIN)
                           or (args.sync_ending and off in ENDING)
                           or (args.sync_intro and off in INTRO)):
            # emu_stop() leaves IP *at* this instruction, so the next
            # emu_start runs it again and the hook fires a second time with
            # no work done in between. Counting those as frames compares the
            # port's frame N+1 against the emulator's frame N.
            #
            # It has to remember **which** instruction, not just that it
            # stopped: with a second sync point a real stop at the other
            # address arrives next and was being swallowed as if it were the
            # re-fire. That is what made --sync-scroll compare the emulator a
            # whole play-loop frame ahead of the port while both counted the
            # same number of sync points.
            if resuming[0] == off:
                resuming[0] = None
                return
            if off in SYNC_KIND:
                # Tagged here rather than where the offset is first seen: the
                # skip above runs before it, so tagging earlier reported a
                # scroll in the window *after* the one that stopped at it.
                draws.append((0x9100 | SYNC_KIND[off],
                              0))       # matches the port's tag
            frame_hit["img"] = snapshot_image()
            frame_hit["vram"] = snapshot_vram()
            resuming[0] = off
            uc.emu_stop()

    m.uc.hook_add(unicorn.UC_HOOK_CODE, on_code, None, code, code + 0x10000)

    if args.watch_write >= 0:
        # Which instruction touches a byte is a question the disassembly
        # answers badly - a cell is reached through a computed DI, so grepping
        # for its address finds nothing. Ask the machine instead.
        def on_write(uc, access, address, size, value, user):
            if not captured:
                return
            ip = uc.reg_read(UC_X86_REG_CS) * 16 + uc.reg_read(UC_X86_REG_IP)
            print(f"  write {address - base:#07x} = {value:#04x} "
                  f"by 1ac2:{ip - code:04x} at frame {frames_done[0]}",
                  flush=True)
        at = base + args.watch_write
        m.uc.hook_add(unicorn.UC_HOOK_MEM_WRITE, on_write, None, at, at)

    def run_a_bit():
        """Let the guest run until a hook stops it, or a chunk goes by."""
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.blocked_on_input = False
        m.uc.emu_start(addr, 0, count=20000)
        m.service_keyboard()

    if args.resume:
        lv, fr, regs, ticks, img, vram, extra, low = read_snapshot(args.resume)
        bot.setstate(extra)
        if low:
            m.uc.mem_write(0, low)
        m.uc.mem_write(base, img)
        m.uc.mem_write(0xB8000, vram)
        for reg, val in zip(REGS_ALL, regs):
            m.uc.reg_write(reg, val)
        m.uc.mem_write(0x46C, struct.pack("<I", ticks))
        captured["regs"] = list(regs[:10])
        captured["img"] = img
        captured["vram"] = vram
        captured["ticks"] = ticks
        # Where the snapshot actually stopped decides both of these, and it
        # is the last of REGS_ALL. A capture at the frame's close is restored
        # *on* the sync instruction, so the hook would fire once with no work
        # done - the port's frame 1 against the emulator's frame 0 - and one
        # hit has to be skipped, the same way the normal path skips one after
        # emu_stop.
        #
        # 1ac2:4210 is the exception and has to stay one: it is not a sync
        # point, so there is nothing to skip - and start_at is what the port
        # is *told it resumed at*, in the state file lockstep.c reads. Setting
        # it to the frame top here overrode --from-bonus and sent the port in
        # through play_loop instead of into the bonus, where it ran one frame
        # body the emulator never ran: one decrement of speed_step, of
        # speed_timer, and of every live entity's tick. Those five bytes are
        # what this route had been differing by, on every frame, for months.
        if regs[-1] == BONUS_BODY:
            start_at = BONUS_BODY
        else:
            start_at = FRAME_END
            resuming[0] = start_at
        base_frame[0] = fr
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

    # bufsize=0, because the watchdog below selects on the pipe: with a
    # BufferedReader the bytes can be sitting in Python's own buffer while the
    # file descriptor looks idle, and the watchdog fires on a port that has
    # already answered.
    port = subprocess.Popen([PORT, "--lockstep", state]
                            + (["--lockstep-sync-scroll"]
                               if args.sync_scroll else [])
                            + (["--lockstep-sync-endgame"]
                               if args.sync_endgame else [])
                            + (["--lockstep-sync-results"]
                               if args.sync_results else [])
                            + (["--lockstep-sync-curtain"]
                               if args.sync_curtain else [])
                            + (["--lockstep-sync-ending"]
                               if args.sync_ending else []),
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            bufsize=0)

    def read_exact(n, first_wait=180.0):
        """Read n bytes, but do not wait for them for ever.

        io_frame_sync is only called at the frame close in play_loop, so any
        screen with a loop of its own - the end-level bonus, the game-over
        sequence, the ending - emits nothing at all while it runs and this
        blocks on the pipe. That is indistinguishable from a hang, and it cost
        an hour of staring at a run that had in fact just finished the last
        level. Now it says which it was.
        """
        buf = b""
        while len(buf) < n:
            if not select.select([port.stdout], [], [],
                                 first_wait if not buf else 30.0)[0]:
                print(f"  the port has sent nothing for "
                      f"{first_wait:.0f}s - it is inside a screen that has no "
                      f"frame sync (the bonus, the game over, the ending), or "
                      f"it has stopped", flush=True)
                return None
            chunk = port.stdout.read(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def port_frame():
        """One frame, or None if the port went quiet or said something else."""
        def u32():
            b = read_exact(4, 30.0)
            return None if b is None else struct.unpack("<I", b)[0]
        head = read_exact(4)
        if head != b"PFRM":
            return None
        pn = u32()
        if pn is None:
            return None
        port_count[0] = pn
        n = u32()
        if n is None:
            return None
        img = read_exact(n, 30.0)
        v = u32()
        if img is None or v is None:
            return None
        vram = read_exact(v, 30.0)
        k = u32()
        if vram is None or k is None:
            return None
        if not k:
            return img, vram, []
        raw = read_exact(k * 2, 30.0)
        if raw is None:
            return None
        return img, vram, list(struct.unpack(f"<{k}H", raw))

    def port_go(mouse_x, buttons, ticks, stop_it=0):
        try:
            port.stdin.write(struct.pack("<HHIB3x", mouse_x & 0xFFFF,
                                         buttons & 0xFFFF,
                                         ticks & 0xFFFFFFFF, stop_it))
            port.stdin.flush()
        except BrokenPipeError:
            pass

    port_count = [0]

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
            pygame.display.set_caption(
                "Popcorn - emulator | port, in lockstep"
                + (" - your mouse drives both" if args.play else ""))
            view["screen"] = pygame.display.set_mode(
                (PANES_W * args.scale, CGA_H * args.scale))
        view["surf"] = pygame.Surface((CGA_W, CGA_H))
        view["panes"] = pygame.Surface((PANES_W, CGA_H))
        # CGA's interlace, precomputed: which vram byte feeds each cell.
        view["idx"] = np.array(
            [[(CGA_PLANE if y & 1 else 0) + (y >> 1) * CGA_STRIDE + x
              for x in range(CGA_STRIDE)] for y in range(CGA_H)])
        view["pal"] = np.array([(0, 0, 0), (0x55, 0xff, 0xff),
                                (0xff, 0x55, 0x55), (0xff, 0xff, 0xff)],
                               dtype=np.uint8)

    def paint(vram):
        """One side's video memory onto view["surf"], CGA interlace undone."""
        np, pygame = view["np"], view["pygame"]
        b = np.frombuffer(vram, dtype=np.uint8)[view["idx"]]
        px = np.stack([(b >> 6) & 3, (b >> 4) & 3, (b >> 2) & 3, b & 3],
                      axis=-1).reshape(CGA_H, CGA_W)
        pygame.surfarray.blit_array(view["surf"],
                                    view["pal"][px].transpose(1, 0, 2))

    def watch_draw(evram, pvram, n):
        pygame = view["pygame"]
        paint(pvram)
        if args.snap and n % args.snap_every == 0:
            # The snapshot is the *port*, which is what it always was: it
            # exists to be looked at from elsewhere, and the emulator beside
            # it would only halve the resolution of the thing being checked.
            big = pygame.transform.scale(
                view["surf"], (CGA_W * args.scale, CGA_H * args.scale))
            pygame.image.save(big, args.snap)
        if not args.watch:
            return 1
        view["panes"].fill((0x22, 0x22, 0x22))
        view["panes"].blit(view["surf"], (CGA_W + WATCH_GAP, 0))
        paint(evram)
        view["panes"].blit(view["surf"], (0, 0))
        pygame.transform.scale(view["panes"], view["screen"].get_size(),
                               view["screen"])
        pygame.display.flip()
        for e in pygame.event.get():
            if e.type == pygame.QUIT:
                return 0
        return 1

    def human_step():
        """Read the player's mouse once, and give it to the **emulator**.

        The invariant this whole tool rests on is that one number reaches
        both sides, so that a difference in the picture cannot be a
        difference in what the player did. That holds for a human exactly
        as it holds for the bot, provided the mouse is read once - so this
        writes the emulator's mouse and lets first_mouse() carry it across,
        rather than sending the port a second reading of the same hand.

        Either pane drives: the two show the same game, and being made to
        aim at the left one to steer the right one would be a puzzle rather
        than a feature.
        """
        pygame = view["pygame"]
        mx = pygame.mouse.get_pos()[0] / max(1, args.scale)
        if mx >= CGA_W + WATCH_GAP:
            mx -= CGA_W + WATCH_GAP
        mx = 0 if mx < 0 else CGA_W - 1 if mx > CGA_W - 1 else mx
        # The game's mouse is absolute and 640 wide - input_mouse is
        # `paddle = clamp(mouse x / 2)` - so a pixel of screen is two of it.
        m.mouse_pos = (int(mx) * 2, 100)
        m.mouse_btn = 1 if any(pygame.mouse.get_pressed()) else 0

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
        """Blank the bytes that are excluded from the comparison.

        Both exclusions are about the harness rather than the port, and both
        are worth re-testing now and then: the sound player was masked here
        for a real bug, the bug was fixed, and the mask outlived it by
        months. --no-mask takes them off.
        """
        if args.no_mask:
            return img
        b = bytearray(img)
        # The stack is **structural**: the port has no guest stack at all, so
        # the leftovers below SP are not a fact about either program. It stays
        # masked for ever.
        b[STACK_LO:STACK_HI] = bytes(STACK_HI - STACK_LO)
        # The three key-state bytes are not masked by default any more. They
        # were, because the INT 09h handler writes them asynchronously on one
        # side and the platform layer on the other - but driven by the same
        # input every frame the two agree, over twenty thousand frames with
        # nothing masked but the stack. --mask-keys puts it back for a run
        # that drives the keyboard, where they can still part company.
        if args.mask_keys:
            b[KEYS_LO:KEYS_HI] = bytes(KEYS_HI - KEYS_LO)
        return bytes(b)

    # The window has to exist before the first input when the player is the
    # one supplying it - there is nowhere to read a mouse from otherwise.
    if args.watch or args.snap:
        watch_open()

    # The port gets its first input before it runs a single instruction, for
    # the same reason: its serve wait reads the action button too.
    if args.play:
        human_step()
    else:
        bot.step()
    port_go(first_mouse(), getattr(m, "mouse_btn", 0), bios_ticks())

    differing, on_screen, compared = 0, 0, 0
    n = -1
    while args.frames == 0 or n + 1 < args.frames:
        if stop_now[0]:
            print("stopped at the requested frame")
            return 0
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
            ebd = sum(1 for a, _ in draws if a == 0x1fc1)
            marks = "".join(
                n for o, n in ((0x0352, "R"), (0x036E, "G"),
                               (0x0473, "o"), (0x0D2E, "p"),
                               (0x0374, "j"), (0x034C, "I"),
                               (0x1EB9, "L"), (0x0376, "D"))
                if any(a == o for a, _ in draws))
            pbd = sum(1 for d in pdraws if d == 0x1fc1)
            print("  f%-5d %-4s bkdrop %3d|%-3d  node %s | %s   prng %02x%02x | %02x%02x" % (
                n, marks or '-', ebd, pbd,
                " ".join(f"{ee[nd + k]:02x}" for k in range(14)),
                " ".join(f"{pp[nd + k]:02x}" for k in range(14)),
                ee[0x33D3], ee[0x33D2], pp[0x33D3], pp[0x33D2]), flush=True)

        if desynced[0]:
            continue                        # driving, not comparing

        if n == args.inject:                # prove the check can fail
            pimg = bytearray(pimg)
            pimg[PADDLE_X] ^= 0x01
            pimg = bytes(pimg)
            print(f"  injected a one-bit change at frame {n}")

        # Are the two sides standing in the same *kind* of place? Comparing
        # the counts cannot answer that - every window consumes exactly one
        # port frame, so those always agree, which made the check look like a
        # safeguard while being unable to fail. The tag says which sync point
        # each side stopped at, and that can disagree.
        if any((args.sync_scroll, args.sync_endgame, args.sync_results,
                args.sync_curtain, args.sync_ending, args.sync_intro)):
            def where(tags):
                k = [t & 0xff for t in tags if t & 0xff00 == 0x9100]
                return SYNC_NAME.get(k[0], "?") if k else "a frame close"
            emu_at = where([t for t, _ in draws])
            port_at = where(pdraws)
            if emu_at != port_at:
                print(f"\n*** out of step at comparison {n}: the emulator "
                      f"stopped at {emu_at} and the port at {port_at}. "
                      f"Everything compared from here is two different "
                      f"moments, not two different results.")
                if not args.keep_going:
                    return 2
                # --keep-going means the run is being *watched*, and the
                # transition out of a screen is the interesting part - it is
                # also exactly where the two part company. Carry on driving
                # both, and stop comparing: the warning above says why, and a
                # flood of meaningless diffs would bury it.
                desynced[0] = True

        a, b = mask(frame_hit["img"]), mask(pimg)
        ev = frame_hit["vram"]
        if a == b and ev == pvram:
            img_bad = vram_bad = []
        else:
            img_bad = [i for i in range(IMAGE_LEN) if a[i] != b[i]]
            vram_bad = [i for i in range(CGA_SIZE) if ev[i] != pvram[i]]

        if img_bad or vram_bad:
            differing += 1
            on_screen += 1 if vram_bad else 0
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
                if len(seen) >= int(os.environ.get("SBSLIM", "20")):
                    print(f"    ... and {len(img_bad) - int(os.environ.get("SBSLIM", "20"))} more image bytes")
                    break
            if draws or pdraws:
                def tag(a, dl):
                    # Tags live at 0x8000 and above; every return address in
                    # this code segment is below 0x5e90, so they cannot be
                    # confused. Masking on 0x1000 did confuse them, and
                    # reported the spawn gate at 0x1c2c as "brick44".
                    if a == 0x1fc1:
                        return "backdrop"
                    if a & 0xff00 == 0x9100:
                        return "sync:" + SYNC_NAME.get(a & 0xff, "?")
                    if a >= 0x9000:
                        return ("exit:no-balls", "exit:no-bricks",
                                "exit:ball-lost",
                                "exit:last-brick")[a & 3]
                    if a >= 0x8000:
                        return f"brick{a & 0xff}"
                    return f"{a:#06x}/{dl}"
                print("    draws this frame - emulator: " +
                      (", ".join(tag(a, dl) for a, dl in draws) or "none"))
                print("                          port: " +
                      (", ".join(tag(d, "") for d in pdraws) or "none"))
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

        if (args.watch or args.snap) and not watch_draw(
                frame_hit["vram"], pvram, n):
            print(f"\nwindow closed at frame {n}")
            break
        frames_done[0] = n
        if n and n % 250 == 0:
            print(f"  {n} frames, still identical "
                  f"(level {frame_hit['img'][LEVEL_NUMBER]})", flush=True)

        # The bot reads the emulator - the reference - and both are told the
        # same thing. So does the player: one mouse, read once.
        if args.play:
            human_step()
        else:
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
        # Memory and screen are worth separating in the tally, not only in
        # each frame's report. A handover that leaves a countdown one tick
        # out differs in five bytes for ever and draws the same picture
        # every frame, and "200 differed" reads as a broken port when what
        # it means is a broken join. The screen is what a player can see;
        # the image is what the two programs believe.
        where = ("none of them on screen" if not on_screen
                 else f"{on_screen} of them on screen")
        print(f"\n{compared} frames compared, {differing} differed - {where}")
        return 1
    print(f"\n{compared} frames compared, identical throughout")
    return 0


if __name__ == "__main__":
    sys.exit(main())
