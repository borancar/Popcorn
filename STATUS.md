# Status

Where the port has got to, what is proven, and what is next.
Facts about the program live in [CLAUDE.md](CLAUDE.md).

Updated 2026-08-22.

## Done

### The executable is recovered and the recovery is proven

`popcorn.exe` is Microsoft EXEPACK. `unpack_popcorn.py` runs the stub under
Unicorn and takes what it leaves in memory, so the result is the original image
by construction rather than by reimplementing the format.

`validate.py` round-trips it: loading the emitted EXE at a segment and applying
its relocation table reproduces the stub's own output **byte for byte**, 133,296
bytes, with the entry point, stack and 35 relocations all as the stub would have
set them. The relocation count is agreed on by two independent readings — the
stub's own table, and a diff of two unpacks at different load segments.

### The game runs, and the CGA output is right

`emulation.py` reaches the title screen, the animated main menu, the player-name
boxes and a playing level, with the colours the game is meant to have.
Keyboard works on both paths: the BIOS INT 16h buffer in menus, and IRQ 1 with
scan codes at port 0x60 while the game's own INT 09h handler is installed.

### The game paces itself on instruction throughput, and the emulator now does too

Popcorn programs PIT channel 0 **never**. The only ports it writes in a whole
session are 0x42, 0x43 and 0x61, and those are the PC speaker. Its pacing comes
from the busy-wait at `0x164c` — `push cx; mov cx,N; loop $; pop cx` with N
patched by POPSPEED — and from waiting on port 0x3da bit 3 for vertical retrace.
So an emulator that runs as fast as it can plays the game as fast as it can.
`--ips` holds the guest to a fixed instruction rate, defaulting to 800,000,
which is roughly the 8 MHz 8086 the readme says the default speed is written
for. The retrace bit also moved from 70 Hz to CGA's 60.

### It plays itself

`autoplay.py` walks the menu and then keeps the paddle under the ball, for
unattended play-throughs and screenshots. It survives indefinitely on the
built-in levels, reaches multi-ball and tracks several balls at once, and
restarts itself if it ever does lose a game.

It drives the **mouse**, not the keyboard, because the game's mouse routine at
`0x1654` is absolute and takes effect on the next frame — `paddle = clamp(mouse
x / 2)` — while the keyboard routine at `0x16d2` moves one pixel per repeat
tick, with an accelerator at `0x2d4b` that only builds while a single direction
is held. A bot that has to reverse loses that acceleration and arrives late.
That delay is the game's, not the emulator's.

Two things about the ball structure cost a debugging round each, and both are
worth knowing before the C port touches this code:

- **`+0x18/+0x19` is not the ball's position.** It is the anchor: where the
  current straight segment began. It does not move again until the next bounce,
  and the Bresenham accumulators at `+0x1a/+0x1b` count away from it. The live
  position, the one that matches the drawn sprite to the pixel, is
  **`+0x00/+0x01`**.
- **The slope pair is stored (dy, dx)**, not (dx, dy). Both branches of the
  stepper at `0x27d7` come out as `x_offset / y_offset = [+0x17] / [+0x16]`.
  Reading them the other way round makes a predicted landing point wrong by the
  square of the slope, which presents as a paddle that jerks at each bounce and
  catches the ball only when the geometry happens to agree.

### Eight routines are transcribed, and all eight are proven

`verify.py` stops the emulator at a routine's entry, captures the machine, lets
the **original** body run to its return, captures again, then runs the C on the
first capture and diffs against the second. The comparison is between the C and
the original *on the same call inside one run*, so it needs no determinism to
mean anything — the host clock and the game's RNG cannot make it flaky.

```
  ok   draw_char           (0x0c64): 25 calls, identical
  ok   input_keyboard      (0x1712): 25 calls, identical
  ok   draw_paddle         (0x221a): 25 calls, identical
  ok   blit_xor            (0x2281): 25 calls, identical
  ok   paddle_row_offsets  (0x22de): 25 calls, identical
  ok   ball_step           (0x27d7): 25 calls, identical
  ok   save_screen         (0x5099):  1 call,  identical
  ok   restore_screen      (0x50bc):  1 call,  identical
```

It reports "NOT REACHED, so unproven" separately from "agreed", because zero
mismatches over a state that never reaches the routine reads exactly like a
pass. `input_keyboard` was in that category until `--keyboard` was added: the
bot plays through the mouse, so `0x16d2` never ran.

The first run of this caught four things, and three were real:

- **`draw_paddle` was missing `sub word [0x1487], 0x1e0`** — taken only when
  the paddle actually moves. `[0x1487]` is a countdown the play loop reloads
  from `[0x1489]`; most likely the level time bonus, charged for moving.
- **`save_screen` copies 16,000 bytes, not the 16,384 of the aperture** — two
  `rep movsw` of `0xfa0` words, which is the 200 visible scan lines at 80 bytes
  for every other one. The 192 bytes of padding at the end of each half are
  neither saved nor restored, and the two halves land *adjacent* in the buffer
  rather than 0x2000 apart. A `memcpy` of the whole aperture is wrong twice
  over.
- **`input_keyboard`'s equal case is not a no-op.** With neither key held it
  resets the acceleration to 5 and clamps the paddle to the right-hand limit;
  with *both* held it moves in the direction of whichever key was pressed most
  recently, which the INT 09h handler records at `[0x2d4a]`.

The fourth was the harness's own: the stack is scratch, not state, and
comparing it flags every routine that pushes anything. It is excluded, and the
note says why.

### The built-in level table is decoded

Fifty levels of 176 bytes at image `0xc46c`, which main copies to `0x2f10` one
at a time. The first byte of each is the **brick count** - checked against the
non-zero cells of the first four levels and exact every time - and the
remaining 175 bytes are the cells.

### The C port draws the menu, and it is the emulator's screen exactly

`make -C reconstruct && ./reconstruct/popcorn` reads your own `POPCORN.EXE`,
unpacks it, runs the four opening animations and leaves you on the main menu
with the function keys live.

`compare_screen.py` settles what "correct" means here: it runs both to the same
point, dumps the 0xb8000 aperture from each and diffs them.

```
visible screen: 15878 of 16000 bytes identical (99.24%)
  10 of 200 scan lines differ
```

The 122 bytes that differ are the two animated elements that are still stubs —
the scrolling banner across the character's belly and the bouncing kernel under
the menu. Everything static is byte-for-byte the original: the lettering, the
logo, the credits, the character, the arrow.

Getting there took four bugs, and the eye would have passed three of them:

- **`movsw` under a set direction flag copies the word *at* `si`** and steps
  afterwards. Reading at `si-1`/`si-2` instead shifted the whole title by two
  bytes, which drew its cyan background and none of the lettering.
- **Two of the four logo passes run forwards.** There is a `cld` at `0x5565`
  between them, and the interlace step reverses with it. Running all four
  backwards drew the character as streaks across the middle of the screen.
- **`intro_curtain` does not just draw bars.** Its second half grows the title
  into the corner, and between the two sits a colour remap that reads two
  source bits and emits two: a leading 0 emits `00` and *discards the bit
  after it*. That maps colour 1 to 0 — it strips the cyan out of the leading
  edge, which is what makes the lettering fade in rather than snap on.
- The player-name boxes, the level loader and the play loop are stubs, so F1
  starts nothing yet.

### The code segment is mapped

`analyze.py` follows control flow from the entry point and the INT 09h handler
and reaches **122 routines, 14,798 of 23,696 bytes (62.4%)**. What it does not
reach is either data between routines or reached through a pointer, and
`--gaps` lists it.

## Open

### Reaching a particular screen

Input is scripted by **code offset** rather than wall clock (`@13d2:return`
fires the first time execution reaches `0x13d2`; several at one offset fire on
successive arrivals). That is reproducible, where a timed script tuned on one
run missed on the next. What is still missing is snapshots — saving and
restoring the whole machine — which is what makes a test *start* at a screen
rather than play to it. shift+F10 writes a screenshot and a VRAM dump, useful as
a rendering reference but unable to resume.

### Not yet modelled

- **PC-speaker sound.** Ports 0x42 and 0x61 are understood (`sound_tick` at
  `0x0097`) but nothing generates audio yet.
- **The disk reads at startup.** The program issues INT 13h AX=0002 and INT 25h
  AX=0002 before anything else. Both are refused and the game carries on, so
  they are not load-bearing, but what they are reading is not yet known — the
  first guess is where `POPSPEED.EXE` stores its value.
- **`popspeed.exe` and `popgen.exe`.** Neither is examined. `popspeed` patches
  the `mov cx,N` immediate in the delay at `0x164c`; where it puts N is the
  same question as the disk reads above.
- **The `.PPC` level format.** `poptab.ppc` is 8,630 bytes and is read whole by
  the loader at `0x08c8`. Not decoded yet. The levels being ported are the ones
  **baked into the executable**, which is what running with no command tail
  uses; `.PPC` files are the level editor's output and come later.
- **The `.HSC` high-score format.** ASCII, 180 bytes, one line per entry.

### Not started

- `native.py` — routines hooked at their entry points and reimplemented in
  Python. Less obviously needed now than it was in Ducks: `verify.py` checks
  the **C** against the original directly, which is what `native.py` existed to
  make possible, so the Python middle layer may never be worth writing.
- **The play path is half done.** `play_loop` (`0x1873`) and `play_session`
  (`0x02f5`) are transcribed, and F1 now enters a level with a working paddle.
  What is still empty is everything they call into: `level_draw` (`0x1c4f`,
  the playfield and the brick reveal), `ball_collide` (`0x2827`), `ball_draw`
  (`0x2881`), `panel_draw` (`0x0b0b`), the eight entity handlers, and the
  end-of-level and end-of-game screens. Until `ball_collide` is real the ball
  cannot be lost and no brick can be hit, so a level neither ends nor
  progresses.
- **The other screens.** F5 (define keys), F6 (hall of fame), F8 (palette),
  F10, and the demo are stubs. `reconstruct/stubs.c` is the list, and
  `POPCORN_TRACE_STUBS=1` prints each one the first time it is reached, so
  "that screen is blank" and "that routine is missing" are the same
  observation.

## Next

1. Keep transcribing outwards from the play loop at `0x1873`, verifying each
   routine as it lands. The order that gets the port running soonest is: the
   sprite blitters the entity handlers use, the level loader, then the play
   loop itself.
2. Decode the built-in level table.
3. Snapshots, so a verification run can start at a screen rather than play to
   it — the 60-second runs are dominated by getting into a level.
4. The menus, which are a large fraction of the code and none of it is
   transcribed.

## Deferred

- **A full per-file inventory in `CLAUDE.md`.** It currently documents only the
  tools that exist. Once the game is fully reversed, every file in the
  repository gets an entry saying what it is and why it is there.
