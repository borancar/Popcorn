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
  Python, each byte-checked against the code it replaces.
- The C reconstruction on SDL3.

## Next

1. Snapshots, so a test can start at a screen rather than play to it.
2. Name the rest of the code map, working outwards from the play loop at
   `0x1873` — which is where the game actually is.
3. Decode the built-in level table.
4. Start `native.py` with the drawing primitives, since those are what the C
   port needs to agree with first.

## Deferred

- **A full per-file inventory in `CLAUDE.md`.** It currently documents only the
  tools that exist. Once the game is fully reversed, every file in the
  repository gets an entry saying what it is and why it is there.
