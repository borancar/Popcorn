#!/usr/bin/env python3
"""
Play Popcorn without a person at the keyboard.

The paddle is driven through the **mouse**, because in mouse mode the game's
own input routine at `0x1654` is a direct, absolute, one-frame mapping:

    mov ax,3 / int 33h        ; CX = x in the 640-wide virtual screen
    mov [0x2d4c], 0
    and bx, 3                 ; either button ...
    jz +                      ; ... is the action key
    mov [0x2d4c], 1
+   shr cx, 1                 ; paddle x = mouse x / 2
    clamp cl to [0x2d3e]..[0x2d3f]
    mov [0x2e54], cl

So setting the emulated pointer puts the paddle exactly where we want it, on
the next frame.  The keyboard route at `0x16d2` cannot do that: it moves the
paddle one pixel per repeat tick, with the divider at `0x2d4b` speeding up only
while a single direction is held, so a bot that has to reverse loses its
acceleration and arrives late.  That delay is a property of the game, not of
the emulator, which is why the fix is to use the input the game makes
absolute.

Everything the bot reads is equally plain:

    0x2e54   paddle left edge, in pixels
    0x2d3e   lowest paddle position (8)
    0x2d3f   highest paddle position (172)
    0x2ea1   ball array, four entries of 0x1e bytes

and each ball entry, read out of the Bresenham stepper at `0x27d7`:

    +0x00/01   the LIVE position, in pixels - this is where the ball is
    +0x14/15   direction flags: non-zero negates that axis
    +0x16/17   the slope - but as (dy, dx), not (dx, dy); see below
    +0x18/19   the anchor: where the current straight segment started
    +0x1a/1b   the Bresenham accumulators, counting away from the anchor
    +0x1c      state: 0 idle, 1-2 in play

`+0x18/19` is the trap here.  It looks like the position, and it is - but only
of the last bounce, and it does not move again until the next one.  Steering at
it means steering at where the ball *was* when it last hit something, which
tracks fine along the first segment after a serve and then sits still while the
ball crosses the screen.  Checked against the screen: `+0x00/01` matches the
drawn ball to the pixel, `+0x18/19` does not.

Usage:
    python autoplay.py --scale 3                 # play, with a window
    python autoplay.py --port --scale 3          # watch the **port** play
    python autoplay.py --demo                    # the game's own attract mode
    python autoplay.py --shots 8 --shot-every 5  # unattended capture
    python autoplay.py --no-bot                  # start a game, then hands off
    python autoplay.py --cmdline poptab          # the shipped level set
"""
import argparse
import collections
import os
import random
import time

import pygame
from unicorn import UcError, UC_HOOK_CODE
from unicorn.x86_const import *

from emulation import VgaDos, KEYMAP, GAME_CODE, IPS_8086_8MHZ, make_surface
from trace_dos import UNPACKED

# --------------------------------------------------------------- the addresses
# The three bytes the game's INT 09h handler maintains, and the only thing its
# keyboard input routine at 0x16d2 reads. Used only by --keyboard; the mouse
# path needs none of them.
KEY_ACTION = 0x2D4C
KEY_RIGHT = 0x2D4D
KEY_LEFT = 0x2D4E

PADDLE_X = 0x2E54          # left edge, pixels
PADDLE_MIN = 0x2D3E
PADDLE_MAX = 0x2D3F
BALLS = 0x2EA1
BALL_STRIDE = 0x1E
BALL_COUNT = 4
B_X, B_Y = 0x00, 0x01              # live position; see the note above
B_DIRX, B_DIRY = 0x14, 0x15
# The slope pair is stored (dy, dx), which is the opposite of how it reads.
# The stepper at 0x27d7 branches on which axis is major, and in both branches
# the offsets it produces come out as
#     x_offset / y_offset = [+0x17] / [+0x16]
# so +0x17 is the horizontal component. Taking them the other way round makes
# every predicted landing point wrong by the square of the slope - the paddle
# then sits somewhere plausible, jerks when the ball turns, and only catches it
# when the geometry happens to agree, which looks exactly like randomness.
B_DY, B_DX = 0x16, 0x17
B_ANCHOR_X, B_ANCHOR_Y = 0x18, 0x19
B_STATE = 0x1C

# The default paddle's width, and only the default: the table at 0x2d0d holds
# four (sprite, width) pairs and E morphs to kind 1, which is 39 wide. The bot
# reads the live width out of 0x2d3a and uses this only as a fallback.
PADDLE_W = 27              # 0x2d0d+2, and it matches the screen
PADDLE_Y = 186             # the row the paddle sits on
CEILING_Y = 6              # the top of the playfield, where a rising ball turns
WALL_L, WALL_R = 9, 195    # the ball's turning points, measured in play

# --------------------------------------------------------------- the capsules
# A falling capsule is an entity running 1ac2:3273, and the fields its catch
# test reads are +2 x, +3 y and +4 the kind. The window is `right = x + 0x0e`
# overlapping [paddle, paddle + width], for y from 0xb6 to 0xbe.
ENTITY_HEAD = 0x3144       # first link; 0xffff ends the chain
E_NEXT = 0x0C
CAPSULE = 0x3273           # entity_capsule's handler address
C_X, C_Y, C_KIND = 0x02, 0x03, 0x04
# Brick 10 - the red block with the white grid - takes the ball away and hangs
# it under a parachute, an entity running 1ac2:37e0 with +4 x, +5 y and +2 the
# ball it is carrying. It descends a pixel a frame to y = 0xb8 and then lets
# go: **upwards if the safety net is up, and otherwise the ball is lost**. The
# paddle cannot catch it on the way down and cannot catch what it drops, so a
# ball in state 4 is not something to steer at - which is why the bot used to
# follow a parachute across the screen and lose a different ball doing it.
#
# What *can* save it is hitting the carrier, which releases the ball early.
# The bot holds the action button permanently, so simply standing under a
# parachute fires the laser at it, if a laser has been collected.
PARACHUTE = 0x37E0
P_BALL, P_X, P_Y = 0x02, 0x04, 0x05
PARACHUTE_BOTTOM = 0xB8    # where it lets go
PARACHUTE_HALF = 8         # the sprite is 16 wide, so this is its centre

# ------------------------------------------------------------------ the laser
# Catching a capsule always morphs the paddle - entity_paddle_fx installs
# whatever 0x2d2d says that kind gives - and only L maps to the laser paddle.
# So while the laser is held, collecting anything at all throws it away, and
# the bot refuses every capsule but another L.
PADDLE_KIND_ADDR = 0x2D39
LASER_PADDLE = 2
LASER_CAPSULE = 3          # the L capsule, the one that keeps it
# shot_xor fires two dots, one under each end of the paddle: at x and at
# x + 0x13. Either can be aimed at a column.
SHOT_SPACING = 0x13
# The brick field, as draw_brick_row lays it out: eight pixels in from the
# left, twelve columns of sixteen pixels, fourteen rows.
LEVEL_CELLS = 0x2F18       # 0x2f10 + 8, past the level record's header
BRICK_LEFT_PX = 8
BRICK_W_PX = 16
BRICK_COLS = 12
BRICK_ROWS = 14
# Nothing breaks these, so a shot into a column whose lowest brick is one of
# them is wasted: 4 and 12 both dispatch to 0x3221, which only bounces, and
# 24-29 are animated bricks that have already been hit.
INDESTRUCTIBLE = frozenset((4, 12, 24, 25, 26, 27, 28, 29))
BALL_STEERABLE = (1, 2)    # state; 3 is brick 9's teleport, 4 the parachute
CATCH_Y = 0xB6             # the first row the paddle can take it on
CAPSULE_W = 0x0E           # the capsule's own width, from the same test
PADDLE_WIDTH = 0x2D3A      # the live width, which the paddle morphs change

# What each kind gives, read off the letter drawn on the capsule and matched
# against the effect table at 0x33bc. The letters are French: F is *filet*,
# the safety net, and V is *vie*, an extra life.
CAPSULE_LETTER = {0: "R", 1: "C", 2: "E", 3: "L", 4: "T", 5: "F",
                  6: "I", 7: "V", 8: "+", 9: "S", 10: "M"}
CAPSULE_EFFECT = {0: "100 points, but it cancels the net",
                  1: "catch", 2: "wider paddle",
                  3: "laser", 4: "multiball", 5: "net", 6: "reverse",
                  7: "extra life", 8: "end level", 9: "slower ball",
                  10: "stops the monsters"}
# Higher is chased first; below zero is never chased at all.
#
# L is top: the laser clears bricks on its own, and it is the only capsule
# that survives collecting another one. Then + for the level, F for the net,
# then V and E, which buy survival.
#
# R and S are refused. R is the trap of the set: it pays a hundred points and
# then **cancels the safety net** and the stopped-monsters state - 0x2daa
# clears [SAFETY_NET] and [EXTRA_ON] and wipes both indicator bars. Taking it
# undoes an F, which is the one capsule that gets a parachuted ball back.
#
# The paddle kind only goes back to 0 at a level start or a lost life
# (0x2d39 is written in play_prepare, play_teardown and the morph), so once
# the laser is held it is held until one of those - provided nothing else is
# collected, which is what the refusal below is for.
#
# 1ac2:31e8 is what S runs, and it decrements the ball's step gate at [0x1486]
# down to 2: the ball then moves on one frame in two instead of two in three.
# The comment here used to say "speed up", which had it exactly backwards.
CAPSULE_WANT = {3: 5, 8: 4, 5: 3, 7: 2, 2: 2,
                10: 1, 1: 1, 4: 1, 6: 0,
                0: -1, 9: -1}
# How much slack to keep between catching a capsule and being back under the
# ball. The mouse route is absolute - the paddle is wherever the pointer says
# on the very next frame - so this is margin against the prediction being
# wrong, not time spent travelling.
SAFETY_FRAMES = 40
# A few pixels of wander on the aim point, resampled every so often. Without
# it the bot returns the ball off the same part of the paddle every time and
# the two settle into a cycle that clears nothing - the paddle sitting still
# while the ball retraces one path. The offset is small enough not to cost a
# catch on a 28-pixel paddle, and it comes from a seeded generator so a run is
# still reproducible.
JITTER = 3
JITTER_HOLD = 24                # bot steps before a new offset is drawn
# With one ball left there is nothing to come back to, so the margin is the
# whole descent rather than a slice of it.
LAST_BALL_FRAMES = 140

# The menu route to a level. `@off` fires the first time execution reaches that
# offset in the game's code segment; several at one offset fire on successive
# arrivals, which is how a name is typed into the one input loop at 0x13d2.
#   0x0206  the main menu's INT 16h poll
#   0x13d2  the player-name input loop
# F3 selects mouse control, which is what makes the paddle absolute.
ROUTE_PLAY = ["@0206:f3", "@0206:f1",
              "@13d2:b", "@13d2:o", "@13d2:t", "@13d2:return", "@13d2:return"]
ROUTE_DEMO = ["@0206:f2"]
# The same route through F4 instead, which leaves the game on its keyboard
# input routine. Worse to play with - see the note above - but it is the only
# way to exercise 1ac2:16d2 at all, which verify.py needs.
ROUTE_PLAY_KEYS = ["@0206:f4"] + ROUTE_PLAY[1:]


class Bot:
    """Keep the paddle under the ball, through the mouse."""

    def __init__(self, m, keyboard=False, seed=0, jitter=JITTER):
        self.m = m
        self.base = m.load_seg * 16
        self.idle = 0
        self.rng = random.Random(seed)
        self.jitter = jitter
        self.wander = 0
        self.held = 0
        # Drive the three key-state bytes instead of the pointer. Only for
        # exercising the keyboard input routine: the paddle then moves one
        # pixel per repeat tick and the bot plays much worse.
        self.keyboard = keyboard

    def getstate(self):
        """Everything about the bot that a resume has to reproduce.

        Without this a snapshot does not replay: the bot starts fresh, makes a
        different decision on the very first frame, and the game goes another
        way - so a divergence found in a long run cannot be reached again from
        the snapshot written next to it. The wander is the whole of it, but
        the whole of it has to go.
        """
        return {"rng": self.rng.getstate(), "wander": self.wander,
                "held": self.held, "idle": self.idle}

    def setstate(self, st):
        if not st:
            return
        # JSON turns the generator's tuples into lists on the way back.
        v, keys, gauss = st["rng"]
        self.rng.setstate((v, tuple(keys), gauss))
        self.wander, self.held, self.idle = (st["wander"], st["held"],
                                             st["idle"])

    def wr(self, off, value):
        self.m.uc.mem_write(self.base + off, bytes([value & 0xFF]))

    def step_keys(self):
        """The same decision, expressed as keys held."""
        self.wr(KEY_ACTION, 1)
        live = self.balls()
        if not live:
            self.wr(KEY_LEFT, 0)
            self.wr(KEY_RIGHT, 0)
            return "serve"
        target = min(live, key=lambda b: (b[2] == 1, -b[1]))
        bx, by, dy_up = target[0], target[1], target[2]
        aim = bx if dy_up else self.predict(bx, by, target[4], target[5],
                                            target[3])
        lo, hi = self.rd(PADDLE_MIN)[0], self.rd(PADDLE_MAX)[0]
        px = self.rd(PADDLE_X)[0]
        want = max(lo, min(hi, aim - self.width() // 2))
        self.wr(KEY_RIGHT, 1 if want > px + 3 else 0)
        self.wr(KEY_LEFT, 1 if want < px - 3 else 0)
        return f"keys {px:3d}->{want:3d}"

    def rd(self, off, n=1):
        return bytes(self.m.uc.mem_read(self.base + off, n))

    def w(self, off):
        return int.from_bytes(self.rd(off, 2), "little")

    def entities(self, handler):
        """Every live node running this handler.

        Walking the list is the only way to see one: entities are not in a
        table, they are a chain of nodes whose handler word says what they
        are. This is the same walk the play loop does at 0x1b4d.
        """
        out = []
        bx = self.w(ENTITY_HEAD)
        for _ in range(64):             # the pool is smaller than this
            if bx == 0xFFFF:
                break
            if self.w(bx) == handler:
                out.append(bx)
            bx = self.w(bx + E_NEXT)
        return out

    def has_laser(self):
        return self.rd(PADDLE_KIND_ADDR)[0] == LASER_PADDLE

    def laser_columns(self):
        """Columns worth shooting at, as the pixel centre of each.

        A shot travels up and meets the **lowest** brick in its column, so a
        column whose lowest brick is indestructible swallows every shot for
        nothing. Only the lowest cell decides.
        """
        cells = self.rd(LEVEL_CELLS, BRICK_COLS * BRICK_ROWS)
        out = []
        for c in range(BRICK_COLS):
            for r in range(BRICK_ROWS - 1, -1, -1):
                v = cells[r * BRICK_COLS + c]
                if not v:
                    continue
                if v not in INDESTRUCTIBLE:
                    out.append(BRICK_LEFT_PX + c * BRICK_W_PX
                               + BRICK_W_PX // 2)
                break
        return out

    def parachutes(self):
        """Carriers on their way down, as (x, y)."""
        return [(self.rd(bx + P_X)[0], self.rd(bx + P_Y)[0])
                for bx in self.entities(PARACHUTE)]

    def capsules(self):
        """Every falling capsule, as (want, kind, x, y)."""
        out = []
        for bx in self.entities(CAPSULE):
            kind = self.rd(bx + C_KIND)[0]
            out.append((CAPSULE_WANT.get(kind, 0), kind,
                        self.rd(bx + C_X)[0], self.rd(bx + C_Y)[0]))
        return out

    def frames_to_paddle(self, b):
        """Roughly how many frames until this ball reaches the paddle row.

        The stepper advances one pixel along the major axis per step, so the
        vertical part of a step is dy/max(dx, dy), and the play loop only lets
        it step on two frames in three - the gate at [0x1485]/[0x1486].  Both
        are approximations; what they have to be right about is the *order* of
        "the ball is a long way off" and "the ball is nearly here".
        """
        x, y, dy_up, dx_neg, dy, dx = b
        major = max(dx, dy) or 1
        per_step = dy / major
        if per_step <= 0:
            return 9999
        drop = (y - CEILING_Y) + (PADDLE_Y - CEILING_Y) if dy_up \
            else PADDLE_Y - y
        if drop <= 0:
            return 0
        return int(drop / per_step * 1.5)

    def width(self):
        """The paddle's live width, which is not a constant.

        E morphs it to paddle kind 1, which is 39 pixels against the default
        27 - so aiming with the default centres a wide paddle six pixels off,
        every time, for as long as the capsule lasts.
        """
        return self.rd(PADDLE_WIDTH)[0] or PADDLE_W

    def capsule_aim(self, cx):
        """Where the paddle's left edge has to be to take a capsule at cx.

        Centre on centre: the capsule spans cx..cx+CAPSULE_W and the paddle
        spans px..px+width, so px = cx + (CAPSULE_W - width) / 2.
        """
        return cx + (CAPSULE_W - self.width()) // 2

    def balls(self):
        out = []
        for i in range(BALL_COUNT):
            b = self.rd(BALLS + i * BALL_STRIDE, BALL_STRIDE)
            if b[B_STATE] in BALL_STEERABLE:
                out.append((b[B_X], b[B_Y], b[B_DIRY], b[B_DIRX],
                            b[B_DX] or 1, b[B_DY] or 1))
        return out

    @staticmethod
    def predict(x, y, dx, dy, x_neg):
        """Where a descending ball reaches the paddle row, walls included.

        The stepper at 0x27d7 is Bresenham over (dx, dy), so the ball's net
        travel is dx horizontal for every dy vertical - exactly the ratio
        needed here.  A bounce is then a fold of the straight-line answer back
        into the playfield, which handles a shallow angle crossing more than
        once without a loop.
        """
        drop = PADDLE_Y - y
        if drop <= 0 or dy == 0:
            return x
        travel = drop * dx // dy
        end = x - travel if x_neg else x + travel
        span = WALL_R - WALL_L
        if span <= 0:
            return x
        k = (end - WALL_L) % (2 * span)
        return WALL_L + (k if k <= span else 2 * span - k)

    def step(self):
        """One decision. Returns a short string for the status line."""
        if self.keyboard:
            return self.step_keys()
        live = self.balls()
        lo, hi = self.rd(PADDLE_MIN)[0], self.rd(PADDLE_MAX)[0]
        px = self.rd(PADDLE_X)[0]

        # Everything the paddle has to be under, soonest first. A ball under
        # a parachute is not in `live` - its own x and y stop moving while the
        # carrier does - so it has to come in as the carrier's position or the
        # bot simply watches it fall, which is what it used to do.
        aims = []
        for b in live:
            aims.append((self.frames_to_paddle(b),
                         b[0] if b[2] else self.predict(b[0], b[1], b[4],
                                                        b[5], b[3]),
                         f"ball {b[0]},{b[1]}"))
        for cx, cy in self.parachutes():
            aims.append((max(0, PARACHUTE_BOTTOM - cy), cx + PARACHUTE_HALF,
                         f"parachute {cx},{cy}"))

        if not aims:
            # Between lives and between levels. Hold the button so the serve
            # goes out the moment the game asks for it, and leave the paddle
            # where the game put it.
            self.idle += 1
            self.m.mouse_btn = 1
            self.m.mouse_pos = (2 * px, 100)
            return f"serve {px}"
        self.idle = 0

        spare, aim, note = min(aims)

        # A capsule is worth going for only while everything else can spare
        # the paddle - not just the nearest one. Leaving to collect something
        # and losing a different ball on the way is not a trade. With one
        # thing left in the air the margin is its whole descent, because there
        # is nothing to come back to.
        margin = LAST_BALL_FRAMES if len(aims) == 1 else SAFETY_FRAMES
        if spare > margin:
            laser = self.has_laser()
            wanted = [c for c in self.capsules()
                      if c[0] > 0 and c[3] < CATCH_Y]
            # Holding the laser is worth more than anything a capsule gives,
            # and there is no way to take one without losing it: every kind
            # maps to some paddle through 0x2d2d, and only L maps back to the
            # laser. So while it is held the bot collects nothing else - not
            # even + or V.
            # ... but only while the laser can still do something. Level 10
            # is the case that proves it: a whole row of cell 3, which
            # brick_3 hardens into an unbreakable 4, walls the top of the
            # field off for good. Once every column ends in one, the laser
            # has nothing left to shoot and the only way out of the level is
            # a + capsule - so a bot that holds the laser and refuses
            # everything survives there for ever without clearing it, which
            # is what it did for three hundred and eighty thousand frames.
            shootable = self.laser_columns() if laser else []
            if laser and shootable:
                wanted = [c for c in wanted if c[1] == LASER_CAPSULE]
            if wanted:
                # Best first, and among equals the one that lands soonest.
                w, kind, cx, cy = max(wanted, key=lambda c: (c[0], c[3]))
                aim = self.capsule_aim(cx) + self.width() // 2
                note = (f"grab {CAPSULE_LETTER.get(kind, kind)} "
                        f"({CAPSULE_EFFECT.get(kind, '?')}) at {cx},{cy}, "
                        f"{spare}f spare")
            elif laser:
                # Nothing to collect and time to spare: put a shot into a
                # column that still has something breakable at the bottom of
                # it. The action button is held permanently, so standing in
                # the right place *is* firing.
                cols = shootable
                if cols:
                    near = min(cols, key=lambda c: abs(c - px))
                    # Either end of the paddle can do it; use whichever is
                    # less of a move from where the paddle already is.
                    left = near - SHOT_SPACING
                    aim = (near if abs(near - px) <= abs(left - px) else left) \
                        + self.width() // 2
                    note = f"laser at column {near}, {spare}f spare"
        # See JITTER: a few pixels of wander, held for a while so the aim is
        # steady across one approach rather than shaking every frame.
        if self.held <= 0:
            self.wander = self.rng.randint(-self.jitter, self.jitter)
            self.held = JITTER_HOLD
        self.held -= 1
        want = max(lo, min(hi, aim - self.width() // 2 + self.wander))
        # The game reads only CL after `shr cx,1`, so the pointer must stay
        # inside 0..510 for the paddle position to survive the truncation.
        self.m.mouse_pos = (min(510, 2 * want), 100)
        self.m.mouse_btn = 1
        return f"{px:3d}->{want:3d} {note}"


def parse_route(route):
    """Turn `@off:key` strings into (offset, key, release) triggers."""
    out = []
    for item in route:
        when, _, name = item.partition(":")
        release = name.startswith("-")
        name = name.lstrip("-")
        key = next((k for k in (getattr(pygame, f"K_{n}", None)
                                for n in (name.lower(), name.upper()))
                    if k is not None), None)
        if key is None or key not in KEYMAP:
            raise SystemExit(f"no scan code for {name!r}")
        if not when.startswith("@"):
            raise SystemExit(f"route entries must be @offset:key, got {item!r}")
        out.append((int(when[1:], 16), key, release))
    return out


class PortView:
    """What Bot needs of a machine, backed by the port's own image.

    The bot reads the game's memory to decide where the paddle goes. Against
    the emulator that memory is the emulator's; against the port it has to be
    the **port's**, or the bot is steering by one program while another plays.
    Bot only writes memory on the keyboard path, which this never uses.
    """

    load_seg = 0

    def __init__(self):
        self.img = b""
        self.uc = self
        self.mouse_pos = (200, 100)
        self.mouse_btn = 1

    def mem_read(self, addr, n):
        return self.img[addr:addr + n]

    def mem_write(self, addr, data):
        pass


def run_port(args):
    """Play the C port, alone, through the lockstep protocol.

    sidebyside.py runs the port and the emulator together and compares them
    every frame. This runs only the port, so what is on screen is the
    deliverable rather than the reference - the thing worth watching when the
    question is "is the port any good" rather than "do the two agree".
    """
    import struct
    import subprocess
    import sidebyside as SBS
    import snapshot as SNAP

    m = VgaDos(args.exe, max_insns=1 << 62, cmdline=args.cmdline)
    base = m.load_seg * 16
    code = base + GAME_CODE
    captured = {}

    start_at = SBS.PLAY_SESSION
    if args.resume:
        lv, fr, extra = SNAP.restore(m, args.resume)
        print(f"resumed {os.path.basename(args.resume)}: level {lv}")
        # A snapshot is already inside a level, so play_session will not come
        # round again - the handover point is the frame close instead, which
        # is what the snapshot was taken on. Same choice sidebyside.py makes.
        _lv, _fr, regs, ticks, img, vram, _x = SNAP.read(args.resume)
        captured["regs"] = list(regs[:10])
        captured["img"] = img
        captured["vram"] = vram
        captured["ticks"] = ticks
        start_at = SBS.FRAME_END

    pending = collections.defaultdict(collections.deque)
    if not args.resume:
        for off, key, _ in parse_route(ROUTE_PLAY):
            pending[off].append(key)

    def on_code(uc, address, size, user):
        off = address - code
        q = pending.get(off)
        if q:
            sc, asc = KEYMAP[q.popleft()]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)
        if off == start_at and not captured:
            captured["regs"] = [uc.reg_read(r) & 0xFFFF for r in (
                UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
                UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_ES,
                UC_X86_REG_DS, UC_X86_REG_EFLAGS)]
            captured["img"] = bytes(uc.mem_read(base, SBS.IMAGE_LEN))
            captured["vram"] = bytes(uc.mem_read(0xB8000, SBS.CGA_SIZE))
            lo, hi = struct.unpack("<HH", uc.mem_read(0x46C, 4))
            captured["ticks"] = (lo + hi) & 0xFFFF
            uc.emu_stop()

    m.uc.hook_add(UC_HOOK_CODE, on_code, None, code, code + 0x10000)

    screen = None
    if not args.headless:
        pygame.init()
        pygame.display.set_caption("Popcorn - the port, playing itself")
        screen = pygame.display.set_mode((SBS.CGA_W * args.scale,
                                          SBS.CGA_H * args.scale))

    def show():
        """Whatever is in the machine's video memory, on the window.

        During the handover that is the emulator walking the menu; afterwards
        the port's own screen is written into the same place. Watching the
        first is the only thing to look at while it happens - the alternative
        is half a minute of nothing.
        """
        if screen is None:
            return True
        surf = make_surface(m)
        pygame.transform.scale(surf.convert(screen), screen.get_size(), screen)
        pygame.display.flip()
        for ev in pygame.event.get():
            if ev.type == pygame.QUIT or (ev.type == pygame.KEYDOWN
                                          and ev.key == pygame.K_ESCAPE):
                return False
        return True

    if not captured:
        print("walking the menu to hand the port a starting state...")
    addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
    ticks_shown = 0
    while not captured and m._elapsed() < 120:
        m.blocked_on_input = False
        m.uc.emu_start(addr, 0, count=20000)
        if m.finished:
            break
        m.service_keyboard()
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        ticks_shown += 1
        if ticks_shown % 40 == 0 and not show():
            return 0
    if not captured:
        raise SystemExit("never reached play_session")
    print("  handed over; the port is playing now")

    state = os.path.join(os.environ.get("TMPDIR", "/tmp"),
                         "popcorn_autoplay.pvs")
    with open(state, "wb") as f:
        f.write(b"PVS2" + struct.pack("<I", start_at))
        f.write(struct.pack("<10H", *captured["regs"]))
        f.write(struct.pack("<I", captured["ticks"]))
        f.write(struct.pack("<I", len(captured["img"])) + captured["img"])
        f.write(struct.pack("<I", SBS.CGA_SIZE) + captured["vram"])

    port = subprocess.Popen([SBS.PORT, "--lockstep", state],
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
        if read_exact(4) != b"PFRM":
            return None
        read_exact(4)
        n, = struct.unpack("<I", read_exact(4))
        img = read_exact(n)
        v, = struct.unpack("<I", read_exact(4))
        vram = read_exact(v)
        k, = struct.unpack("<I", read_exact(4))
        if k:
            read_exact(k * 2)
        return img, vram

    view = PortView()
    bot = Bot(view)
    clock = pygame.time.Clock()

    frames = 0
    prev_level = [0]
    start = time.time()
    # The port reads a command **before** it runs a frame, so the first one
    # has to go out before anything is read back. Reading first deadlocks the
    # pair: the port waiting for input, this waiting for a frame, and a window
    # that never gets past black.
    port.stdin.write(struct.pack("<HHIB3x", view.mouse_pos[0] & 0xFFFF,
                                 view.mouse_btn & 0xFFFF,
                                 captured["ticks"], 0))
    port.stdin.flush()
    while True:
        pf = port_frame()
        if pf is None:
            print(f"the port stopped after {frames} frames")
            break
        view.img, vram = pf
        frames += 1

        # The port's screen, decoded by the emulator's own CGA renderer: write
        # its video memory into the idle machine and let make_surface do the
        # interlace and the palette rather than repeating both here.
        m.uc.mem_write(0xB8000, vram)
        if not show():
            port.kill()
            return 0
        if screen is not None:
            clock.tick(60)

        note = bot.step()
        # Level 49 cleared takes [0x13cc] to 0x32, which play_session resets
        # to 0 before running screen_all_levels_done - so the level number
        # falling back to 0 from a high one is the whole game finished, and
        # it is worth saying out loud rather than scrolling past.
        lv = view.img[0x13cc]
        if lv < prev_level[0] and prev_level[0] >= 0x30:
            print(f"  [port] *** the last level is finished, at frame "
                  f"{frames} ***", flush=True)
        if lv != prev_level[0]:
            print(f"  [port] level {prev_level[0]} -> {lv} at frame {frames}",
                  flush=True)
            prev_level[0] = lv
        if args.status_every and frames % (args.status_every * 40) == 0:
            print(f"  [port] {frames:6d} frames  level "
                  f"{view.img[0x13cc]:2d}  lives {view.img[0x13c9]:2d}  "
                  f"bricks {view.img[0x2f10]:3d}  {note}", flush=True)
        # Nothing here has to match anything, so the tick the PRNG stirs in
        # just has to advance the way the BIOS one would: 18.2 a second.
        ticks = int((time.time() - start) * 18.2) & 0xFFFF
        try:
            port.stdin.write(struct.pack("<HHIB3x",
                                         view.mouse_pos[0] & 0xFFFF,
                                         view.mouse_btn & 0xFFFF, ticks, 0))
            port.stdin.flush()
        except BrokenPipeError:
            break
        if args.run_seconds and time.time() - start > args.run_seconds:
            port.kill()
            break
    return 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=UNPACKED)
    ap.add_argument("--cmdline", default="",
                    help="DOS command tail naming a level file, as in "
                         "POPCORN POPTAB. The default is no tail, which is the "
                         "table baked into the executable - the levels the game "
                         "ships with, and the ones being ported")
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--demo", action="store_true",
                    help="run the game's own attract mode (F2) instead")
    ap.add_argument("--no-bot", action="store_true",
                    help="reach the level, then leave the paddle alone")
    ap.add_argument("--chunk", type=int, default=20_000)
    ap.add_argument("--ips", type=int, default=IPS_8086_8MHZ)
    ap.add_argument("--shots", type=int, default=0)
    ap.add_argument("--shot-every", type=float, default=5.0)
    ap.add_argument("--shot-dir", default="debug")
    ap.add_argument("--port", action="store_true",
                    help="drive the **C port** instead of the emulator, and "
                         "show its screen. The emulator still boots far enough "
                         "to hand over a starting state - the lockstep "
                         "protocol begins at the play loop and the port has no "
                         "way to reach it on its own - and then stops")
    ap.add_argument("--resume", metavar="FILE",
                    help="with --port, start from a snapshot.py snapshot "
                         "rather than walking the menu")
    ap.add_argument("--headless", action="store_true")
    ap.add_argument("--run-seconds", type=float, default=0.0)
    ap.add_argument("--status-every", type=float, default=5.0)
    ap.add_argument("--idle-restart", type=float, default=12.0,
                    help="seconds without a ball in play before the menu route "
                         "is walked again; 0 never restarts")
    args = ap.parse_args()

    if args.port:
        return run_port(args)

    if args.headless:
        os.environ["SDL_VIDEODRIVER"] = "dummy"
    if args.shots:
        os.makedirs(args.shot_dir, exist_ok=True)

    pygame.init()
    m = VgaDos(args.exe, max_insns=1 << 62, cmdline=args.cmdline)
    bot = Bot(m)

    triggers = parse_route(ROUTE_DEMO if args.demo else ROUTE_PLAY)
    pending = {}
    code_base = m.load_seg * 16 + GAME_CODE
    started = [False]

    def arm_route():
        """(Re)install the menu route, so a finished game starts another."""
        pending.clear()
        for off, key, release in triggers:
            pending.setdefault(off, collections.deque()).append((key, release))
        started[0] = False

    def on_code(uc, address, size, user):
        q = pending.get(address - code_base)
        if q:
            key, release = q.popleft()
            sc, asc = KEYMAP[key]
            m.press_key(sc, asc, down=not release)
            if not release:
                m.press_key(sc, asc, down=False)
            print(f"  [route] scan {sc:#04x}")
            if not any(pending.values()):
                started[0] = True

    arm_route()
    m.uc.hook_add(UC_HOOK_CODE, on_code, None, code_base, code_base + 0x10000)

    screen = pygame.display.set_mode((320 * args.scale, 200 * args.scale))
    pygame.display.set_caption("Popcorn - autoplay")
    clock = pygame.time.Clock()

    addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
    running, frames, shots, note = True, 0, 0, ""
    next_shot, next_status = args.shot_every, args.status_every
    budget_t = time.perf_counter()
    last_ball = None
    restarts = 0

    while running:
        m.blocked_on_input = False
        try:
            m.uc.emu_start(addr, 0, count=args.chunk)
        except UcError as e:
            print(f"  [cpu] {e} at {m._reg(UC_X86_REG_CS):04x}:"
                  f"{m._reg(UC_X86_REG_IP):04x}")
            break
        if m.finished:
            print(f"  [dos] program exited: {m.finished}")
            break
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.service_keyboard()
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)

        if started[0] and not args.demo and not args.no_bot:
            note = bot.step()
            # Watchdog. Losing the last life goes to the hall of fame and then
            # back to the menu, where nothing the bot writes has any effect. So
            # if no ball has been in play for a while, walk the route again -
            # otherwise an unattended run stops being a play-through the first
            # time it dies.
            if bot.balls():
                last_ball = m._elapsed()
            elif last_ball is None:
                last_ball = m._elapsed()
            elif (args.idle_restart
                    and m._elapsed() - last_ball > args.idle_restart):
                restarts += 1
                print(f"  [route] no ball for {args.idle_restart:.0f}s - "
                      f"walking the menu again (restart {restarts})")
                arm_route()
                last_ball = None
                m.mouse_btn = 0

        if args.ips and not m.blocked_on_input:
            budget_t += args.chunk / args.ips
            spare = budget_t - time.perf_counter()
            if spare > 0.0005:
                time.sleep(spare)
            elif spare < -0.25:
                budget_t = time.perf_counter()
        else:
            budget_t = time.perf_counter()

        frames += 1
        # make_surface() hands back an 8-bit paletted surface; the display is
        # 32-bit, and pygame will not scale between formats.
        surf = make_surface(m)
        pygame.transform.scale(surf.convert(screen), screen.get_size(), screen)
        pygame.display.flip()
        clock.tick(120)

        for ev in pygame.event.get():
            if ev.type == pygame.QUIT:
                running = False
            elif ev.type == pygame.KEYDOWN and ev.key == pygame.K_ESCAPE:
                running = False

        el = m._elapsed()
        if args.shots and el >= next_shot:
            next_shot += args.shot_every
            shots += 1
            path = os.path.join(args.shot_dir, f"play{shots:02d}.png")
            pygame.image.save(surf, path)
            print(f"  [shot] {path}  t={el:5.1f}s  {note}")
            if shots >= args.shots:
                running = False
        if el >= next_status:
            next_status += args.status_every
            print(f"  [play] t={el:5.1f}s frames={frames} "
                  f"balls={len(bot.balls())} {note}")
        if args.run_seconds and el >= args.run_seconds:
            running = False

    print(f"stopped after {frames} display updates, {m._elapsed():.1f}s, "
          f"{restarts} restarts")


if __name__ == "__main__":
    main()
