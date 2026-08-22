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

`reconstruct/exepack.c` decodes it again in C, and the port reads the player's
own `POPCORN.EXE` at startup. No game data is embedded and none is committed.

### The game runs under emulation, and the CGA output is right

`emulation.py` reaches the title screen, the animated main menu, the
player-name boxes, a playing level and the hall of fame, with the colours the
game is meant to have. Keyboard works on both paths: the BIOS INT 16h buffer in
menus, and IRQ 1 with scan codes at port 0x60 while the game's own INT 09h
handler is installed.

INT 10h AH=0Ch/0Dh — one pixel per BIOS call — is implemented for modes 4, 5
and 6, XOR included. It had been missing, and since the menu's bouncing kernels
are drawn entirely through it, six hundred thousand calls a minute were being
dropped on the floor. That is worth stating plainly because of *how* it was
found: the port and the emulator disagreed, an independent model of the
instructions agreed with the port exactly, and so the hole had to be on the
emulator's side. Verification catches the checker as well as the checked.

### The game paces itself on instruction throughput, and the emulator does too

Popcorn programs PIT channel 0 **never**. The only ports it writes in a whole
session are 0x42, 0x43 and 0x61, and those are the PC speaker. Its pacing comes
from the busy-wait at `0x164c` — `push cx; mov cx,N; loop $; pop cx` with N
patched by POPSPEED — and from waiting on port 0x3da bit 3 for vertical retrace.
So an emulator that runs as fast as it can plays the game as fast as it can.
`--ips` holds the guest to a fixed instruction rate, defaulting to 800,000,
which is roughly the 8 MHz 8086 the readme says the default speed is written
for. The retrace bit also moved from 70 Hz to CGA's 60.

POPSPEED stores its setting in the **offset half of interrupt vector 0x68**,
which is why the startup disk reads looked load-bearing and were not.

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

Two things about the ball structure cost a debugging round each:

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

### Every reachable routine is transcribed

**179 of 179, all 24,062 bytes.** `port_coverage.py` measures it by the image
offset each routine carries, not by counting functions: a routine counts only
when its `1ac2:xxxx` header appears somewhere that is not `stubs.c`, so a stub
renamed to look finished does not move the number.

`reconstruct/stubs.c` is down to `entity_unknown`, which is a safety net for a
handler address that is not in any table. It should never fire.

The last one in was `0d2e`, where a player's turn ends. Each player's whole
state lives in a 0x11b-byte record — lives, level, score, their copy of the
cells, and at `+0xd2` a count followed by copies of the live entities. Saving
the entity list is what lets a player come back to a level with the capsules
still falling.

### The transcription is checked against the original, not against the screen

`verify.py` captures the machine at a routine's entry, lets the **original**
run to its return, captures again, then runs the C on the first capture and
diffs the image, the video memory and, where the routine has one, its return
value. A routine that compiles proves nothing and one that looks right on
screen proves very little more; a blitter can be wrong in ways that still draw
something plausible.

`reconstruct/verify.c` dispatches 142 routines. Which registers a routine takes
its arguments in is part of what is being asserted — getting that wrong shows
up as a mismatch, which is the point.

The harness had to be sharpened four times, each time because it was agreeing
for a reason that was not evidence:

1. **It counted calls, not work.** Twenty-five agreements on a routine whose
   common path is an early return. It now caps on calls that *changed
   something*, and says so when a routine's whole sample was early returns.
2. **The PRNG is seeded from the BIOS tick count.** Without carrying
   `0040:006c` across from the emulator, every routine that consults it
   diverged for a reason that was not a bug — the C crumbled a brick where the
   original removed it, and the only difference was the seed.
3. **Memory comparison alone passes a pure function.** `game_random` only bumps
   a counter by a constant, so it agreed whatever it computed. Routines may now
   report a **return value** and have that compared too.
4. **The stack is not evidence.** Excluded (`0x1AA20`–`0x1AC20`), because
   leftovers below SP differ for reasons that have nothing to do with the
   routine.

Two failures that stood for a while are now settled, and neither was what it
looked like:

- `panel_reveal` (`0911`) draws its corner pieces with `rep movsb`, and `si` is
  set **once**, before the loop. They are two 21-byte sprites read straight
  through, three bytes a row — not one row drawn seven times. The disputed
  bytes at vram `0x50` were the sprite's third row.
- `menu_particles_tick` (`53c2`) was the emulator's missing pixel call,
  described above. The port was right all along.

## Open

### Verification coverage is bounded by what a run reaches

Three routes - the mouse play route, the keyboard one, and sitting in the menu
- verify **71 routines byte-identical with nothing failing**, 70 of them on a
call that actually changed something. `verify.py --menu` and `--keyboard` exist
because the attract demo and the keyboard input path are not reachable from a
route that starts a game with the mouse.

Fourteen dispatched routines are reached by none of the three: `draw_run`,
`demo_start`, `draw_paddle_raw`, `brick_11`, six entity handlers around
`0x365e`-`0x37e0`, `cells_restore`, and the menu arrow's two halves. They need
game states a bot does not play into.

The honest limit is that **a routine no run reaches is unproven**, and
`verify.py` prints that list rather than quietly omitting it. What would fix it
properly is **snapshots** - saving and restoring the whole machine - so a check
can *start* at a screen instead of playing to it. Input is currently scripted by
code offset (`@13d2:return` fires the first time execution reaches `0x13d2`),
which is reproducible where a timed script tuned on one run missed on the next,
but it still has to play the game to get there.

A few routines cannot be checked this way at all and are excluded on purpose:
the ones that never return normally (`play_session` leaves by longjmp), the
ones that are DOS or hardware I/O (`hsc_save`, `drive_check`,
`read_speed_setting`), and `screen_define_keys`, which switches to text mode
01h - the port has no text renderer.

Two kinds of byte are excluded from the comparison, both because they are not a
function of the routine being checked: the **stack** below SP, and the three
**key-state bytes** at `0x2d4c`-`0x2d4e` that the INT 09h handler maintains.
The original takes interrupts while a sampled call runs and the C takes none,
so `draw_paddle_shifted` - which never mentions those bytes - differed on one
call in eleven because a key went down inside it. They are blanked at
comparison time only: `laser_fire` reads `0x2d4c` to decide whether to fire, and
blanking it in the state handed to the C made the port hold its fire and the
original shoot.

### Not yet modelled

- **PC-speaker sound.** Ports 0x42 and 0x61 are understood (`sound_tick` at
  `0x0097`) but nothing generates audio yet.
- **The `.PPC` level format.** `poptab.ppc` is 8,630 bytes and is read whole by
  the loader at `0x08c8`, which is transcribed; the format its contents are in
  is not decoded. The levels being ported are the ones **baked into the
  executable**, which is what running with no command tail uses.
- **`popspeed.exe` and `popgen.exe`.** Neither is examined.

### Not started

- `native.py` — routines hooked at their entry points and reimplemented in
  Python. Less obviously needed than it was in Ducks: `verify.py` checks the
  **C** against the original directly, which is what `native.py` existed to make
  possible, so the Python middle layer may never be worth writing.

## Next

1. Widen verification coverage: longer and differently-routed runs, and then
   snapshots, so the screens a bot does not reach can be checked too.
2. Sound.
3. The `.PPC` format, which is what makes the level editor's output playable.

## Deferred

- **A full per-file inventory in `CLAUDE.md`.** It currently documents only the
  tools that exist. Now that the transcription is complete this is the next
  documentation job.
