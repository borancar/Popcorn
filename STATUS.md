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

**185 of 185, all 25,230 bytes.** `port_coverage.py` measures it by the image
offset each routine carries, not by counting functions: a routine counts only
when its `1ac2:xxxx` header appears somewhere that is not `stubs.c`, so a stub
renamed to look finished does not move the number.

`reconstruct/stubs.c` is down to `entity_unknown`, which is a safety net for a
handler address that is not in any table. It should never fire.

The last three in were the **animated bricks**, and they are worth the
paragraph because of how they hid. The brick table at `0x3044` is thirty words
long; its entries 16 to 21 all point at `0x2ccd`, which is not an ordinary
brick handler. It does not clear the cell - it **adds eight** to it, leaving
the low nibble alone - draws the piece out of the level's animation script, and
allocates an entity running `0x3abf`, which redraws that piece every time the
script steps. Six cells make one picture, and nine of the fifty levels use it;
the picture goes on moving after you have broken it.

`0x3abf` was invisible to the recursive-descent map because **nothing calls
it**: it is only ever stored into an entity node's handler slot, so its bytes
were being counted as the tail of `entity_bonus`. Seeding it explicitly is what
took the count from 179 to 185.

Before that, `0d2e`, where a player's turn ends. Each player's whole
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

`reconstruct/verify.c` dispatches 145 routines. Which registers a routine takes
its arguments in is part of what is being asserted — getting that wrong shows
up as a mismatch, which is the point.

`verify.py --resume` takes one of `sidebyside.py`'s snapshots - image, video
memory, all fourteen registers and the BIOS tick the PRNG seeds from - and
starts there instead of walking the menu. That is what the "snapshots would fix
this properly" note under **Open** asked for, and it arrived because it had to:
the three animated-brick routines only run on nine of the fifty levels, the
first of which is about ten minutes of emulation away from the menu. Resumed at
level 7 they verify in two minutes, ten calls each, all identical.

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

### Two million frames come out byte for byte

`sidebyside.py` plays the emulator and the port together on the same driven
input and compares the whole image and the whole screen after every frame.
With `--from-session` it follows a whole game rather than one level, and it
now runs past **2,000,000 frames** without a byte differing, through fourteen
levels and the end-level bonus. That screen alone accounted for six of the
bugs found on 2026-08-23; before them the two sides parted company on its
ninth frame, and before the bot could aim, no run ever got past level 10.

`--snapshots DIR` writes a resumable state at the start of every level and
`--resume FILE` starts from one, so a divergence hours in is reached in a
couple of minutes rather than replayed. Almost nothing since the harness was
built has been found without it - with one caveat recorded below.

**And the sound player is in the comparison now.** `cs:[0xf4]`-`[0xf7]` - the
request, the note timer and the tune pointer - used to be masked, because one
side raised a request on a frame the other did not. Run without `--no-sound`,
the two sides agree on those four bytes for **250,000 frames**. The **key-state bytes** at `0x2d4c` came off with it: driven by the same input
every frame the two sides agree on those as well, and 150,000 frames run clean
with nothing masked but the stack. `--mask-keys` puts it back for a run that
drives the keyboard, where they can still part company.

What stays masked is the **stack below SP**, and that one is structural rather
than stale: the port has no guest stack at all, so the leftovers there are not
a fact about either program. Unmasked, every byte that differs in twenty
thousand frames is in `0x1aa8e`-`0x1aa9f` and nowhere else. `--no-sound` stays, because
narrowing a comparison is still how a divergence gets isolated; nothing needs
it.

**And the levels a bot never reached are compared now too.** `sweep_levels.sh`
pokes the level *before* the one wanted and clears it, so `play_session` loads
the wanted one normally, and the comparison starts from that level's own
beginning. Twenty-two levels across the back half of the game - 11, 12, 13,
14, 15, 17, 19, 20, 22, 24, 26, 28, 31, 33, 36, 38, 41, 43, 46, 47, 48 and 49
- each run four thousand frames identical.

That is worth recording as a result rather than as an absence. Before it,
every frame-for-frame number this file has ever quoted covered levels 0 to 10,
because that is as far as a bot that could only return the ball ever got. The
deep levels use cells and combinations the early ones do not - 49 is 168 cells
of value 11 and nothing else - and the whole lesson of 2026-08-23 is that
untested code is where the bugs are.

The instruction counts say those frames are real rather than empty: the sound
tick, the ball's step gate and the entity walk each run once a frame and the
ball steps on two in three of them, exactly what the `[0x1486] = 3` gate
predicts, held over the whole run.

### What is still open

**`verify.py` cannot check the end-level bonus at all**, and the reason is
structural rather than an input problem - which took most of a day to see.
`1ac2:2da0` throws four words off the stack and the ending's own `ret` lands
in `play_session`, so the routine never returns to the address the harness
read off the stack at entry. "Run the original to its return address" therefore
runs it on through the level change and beyond, and compares that against a C
function that stopped at the end of the screen. It is out of the dispatch now,
with the reason written where the case used to be.

`entity_paddle_fx` has the same limit on the one call in sixty that fires the
bonus, and stays dispatched: the other fifty-nine are worth having, and the
report says one call in sixty rather than pretending to a clean sweep.

The screen is checked by `sidebyside.py` instead, which does not care where a
routine returns to.

**A snapshot resume is not equivalent to the run it came from.** The level 5
divergence that was open for much of 2026-08-23 did not reproduce from a
snapshot taken *one frame before it*, while two full runs from the menu
stopped at the same frame with the same bytes. It turned out the resumed run
took the bonus's **other ending** and agreed by luck - so the reproduction was
not evidence, and reading it as one nearly closed a bug that was still there.
Low memory is captured now (the vector table and BIOS data area, neither of
which is inside the load image) and it was not the cause. What is left to
suspect: the emulator's own attributes - retrace phase, key and scan queues,
CGA registers - and the port's C stack, which a resume rebuilds from
`play_session`'s `goto retry` rather than restoring.

### Coverage, as last measured

`verify_all.py --summary FILE``verify_all.py --chase --summary FILE` writes this; it is a number that was
measured rather than remembered, and re-running it is how it should be updated.

**Use `--chase`.** A routine whose caller is being sampled is never sampled
itself - the harness will not re-enter it - so a plain pass reports as
unchecked a great many routines it ran straight past. The chase re-runs each
route with `--only` set to whatever is still unreached, and it is worth about
twenty routines: 141 proven and 11 unreached with it, against 123 and 29
without on a *larger* set of routes.

<!-- generated by verify_all.py --summary -->
| | routines | |
| --- | ---: | --- |
| **transcribed** | | 185 of 185 reachable routines transcribed, 25230 of 25230 bytes (100.0%) |
| **proven** | 146 | reached, did work, agreed on every route that reached it |
| shallow | 3 | agreed, but every call was an early return - not proof |
| differing | 1 | entity_paddle_fx |
| unreached | 8 | no route runs them |
| **dispatched** | 158 | what verify.c can check |

Routes in this union: play, menu, keyboard, menu.snap +@0206:f8, menu.snap +@0206:f10, menu.snap +@0206:f6, menu.snap +@0206:f2, menu.snap +@0206:^l,@0206:^a,@0206:^c,@0206:^r,@0206:^a,@0206:^l,@0206:space,@0206:s,@0206:o,@0206:f,@0206:t,@0206:w,@0206:a,@0206:r,@0206:e,@0206:return, cap.snap, level10_f000000.snap, level49_f000000.snap, particles.snap, tall.snap, marks.snap, vlife.snap, play (chase), menu (chase), keyboard (chase), menu.snap +@0206:f8 (chase), menu.snap +@0206:f10 (chase), menu.snap +@0206:f6 (chase), menu.snap +@0206:f2 (chase), menu.snap +@0206:^l,@0206:^a,@0206:^c,@0206:^r,@0206:^a,@0206:^l,@0206:space,@0206:s,@0206:o,@0206:f,@0206:t,@0206:w,@0206:a,@0206:r,@0206:e,@0206:return (chase), cap.snap (chase), level10_f000000.snap (chase), level49_f000000.snap (chase), particles.snap (chase), tall.snap (chase), marks.snap (chase), vlife.snap (chase).

Two of those figures matter more than the headline. **Shallow** is a routine
every call to which was an early return: it agreed, and that agreement is not
proof. Some of them cannot leave that column by being run more: `border_step`
and `hsc_bubble` change **no memory at all** - their whole answer is the DI
they return - and `flush_keys` and `restore_int09` are deliberate no-ops in
the port, because the platform layer owns the keyboard. The first two are
fixed by comparing the register, which `RETURNS` now does for AH, AX, DX, DI
and the carry. The second two never can be, and saying so is better than
leaving them looking like work someone forgot. **Unreached** is the honest limit - a routine no route runs is not
checked at all, and every bug found on 2026-08-23 was in that column the day
before. Proven went from 82 to 146 in one session purely by reaching further,
without anyone reading a line of assembly looking for mistakes.

### Verification coverage is bounded by what a run reaches

A ten-minute play route verifies **82 routines byte-identical with nothing
failing**. Every failure that stood in this list for most of a session turned
out to be the harness rather than the port:

- **1ac2:1a4f and 1ac2:1a6f are not routines.** 0x1a4f is a call site,
  `call word ptr [0x2d45]`, and 0x1a6f is an inline block inside play_loop.
  The harness reads [SP] as a return address, which for these is play_loop's
  own frame, lets the "original" run to somewhere arbitrary, and blames the C
  for the difference. Both are out of the dispatch now.
- **bonus_steer and bonus_script were called with zeros.** entity_bonus hands
  the capsule's x and y in CL and AL; the harness passed 0 and 0, so the
  routine worked on a capsule at the origin, took a different branch, and drew
  twice from the PRNG where the original drew nothing.

A check that cannot fail is worth nothing, and a check that fails for its own
reasons is worse - it spends attention on the wrong thing. Both kinds were
here. `verify.py --menu` and `--keyboard` exist
because the attract demo and the keyboard input path are not reachable from a
route that starts a game with the mouse.

Fourteen dispatched routines are reached by none of the three: `draw_run`,
`demo_start`, `draw_paddle_raw`, `brick_11`, six entity handlers around
`0x365e`-`0x37e0`, `cells_restore`, and the menu arrow's two halves. They need
game states a bot does not play into.

The honest limit is that **a routine no run reaches is unproven**, and
`verify.py` prints that list rather than quietly omitting it. Snapshots are the
answer to it and now exist: `verify.py --resume` starts a check *at* a screen
instead of playing to it, which is how the animated bricks were checked at all.
Applying it to the rest of the unreached list is the outstanding job - it needs
snapshots taken at the states a bot does not play into, not just at level
starts. Input is otherwise scripted by code offset (`@13d2:return` fires the
first time execution reaches `0x13d2`), which is reproducible where a timed
script tuned on one run missed on the next.

A few routines cannot be checked this way at all and are excluded on purpose:
the ones that never return normally (`play_session` leaves by longjmp), the
ones that are DOS or hardware I/O (`hsc_save`, `drive_check`,
`read_speed_setting`), and `screen_define_keys`, which switches to text mode
01h - the port has no text renderer.

Two kinds of byte are excluded from the comparison, both because they are not a
function of the routine being checked: the **stack** below SP, and the three
**key-state bytes** at `0x2d4c`-`0x2d4e` that the INT 09h handler maintains.
The second was re-tested on 2026-08-23 rather than taken on trust, and it
earns its place: `--no-mask-keys` on the keyboard route produces three
failures - `panel_draw`, `blit_xor` and `brick 1`, each differing in exactly
one of those three bytes and nothing else - which is a key going down inside a
sampled call, not a transcription. The same mask in `sidebyside.py` did *not*
earn its place and has been dropped there.
The original takes interrupts while a sampled call runs and the C takes none,
so `draw_paddle_shifted` - which never mentions those bytes - differed on one
call in eleven because a key went down inside it. They are blanked at
comparison time only: `laser_fire` reads `0x2d4c` to decide whether to fire, and
blanking it in the state handed to the C made the port hold its fire and the
original shoot.

### Not yet modelled

- **PC-speaker sound in the emulator.** `emulation.py` is still silent:
  `sound_tick` at `0x0097` is understood and the ports are modelled, but
  nothing turns them into audio. The **port** does make sound - see below.
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

1. **The end-level bonus, still.** Five transcription errors and one
   structural one have come out of it, and the two sides now play the whole
   screen together and into the next level: **9,000 frames identical** from
   , against 9 at the start of the day. Two things are still open on
   it, and they are different questions:

   - A full run from the menu diverges at frame 120,291 on level 5, with the
     **port** a level ahead this time rather than behind - and it does **not
     reproduce**: resuming from that level's own snapshot, 6,753 frames
     earlier, runs 9,000 frames clean - and so does a resume from **one frame
     before it**. The full run itself is deterministic: a second one from the
     menu stops at the same frame with the same bytes. So the snapshots do not
     capture everything, and it is not accumulation over a long run - it is
     something the resume gets wrong immediately. Low memory has been ruled
     out (the vector table and BIOS data area are captured now, and it changes
     nothing), which leaves the emulator's own attributes - the retrace phase,
     the key and scan queues, the CGA registers - and the port's C stack,
     which a resume rebuilds from `play_session`'s `goto retry` rather than
     restoring. That is a limit on the harness rather than a fact about the
     port. Worth knowing before trusting a
     reproduction that comes back clean: it is evidence about the resumed run,
     not about the original one.
   -  still disagrees when it compares the whole screen as a
     **single call**. That call reads thousands of inputs, and the harness can
     pin one pointer and one keypress, not a stream of them; the lockstep,
     where both sides genuinely get the same input every frame, is clean. The
     two are not the same claim and the smaller number is not the better one.

   The carry the bonus returns on is worth knowing about: 
   branches on it at , and **nothing sets it deliberately**. There
   is no  or  on either exit from  - the flag is
   whatever the last arithmetic left, which on the ordinary path is the
    in  and so zero, a completed level. That is why
   the C says so outright, and why it is worth writing down that the original
   is relying on an accident here.

2. **Two routines nothing builds a state for.** `score_before` needs a
   **two-player** game, which the bot never plays; `copy_string_text` is in
   `screen_define_keys`, which switches to text mode 01h and is excluded on
   purpose. Both are honest gaps rather than oversights.
3. **Sound in the emulator.** The port plays it: `io_sound` records the PIT
   divisor and `io_present` tops the stream up every frame while the note
   lasts. That top-up is the part worth keeping - `sound_tick` only calls
   `io_sound` when the note **changes**, since it returns early at `1ac2:00a5`
   while one is being held, so queueing a fixed buffer played the first thirty
   milliseconds of every note and then silence. A note of ten ticks is about a
   sixth of a second. `emulation.py` is still silent.
4. **The `.PPC` format**, which is what makes the level editor's output
   playable.

## Deferred

- **Levels 11 and beyond.** The bot has held level 10 for a quarter of a
  million frames without clearing it, so the levels after it are untested by
  the side-by-side. Reaching them wants a bot that aims rather than one that
  survives, or a snapshot taken with the level number written by hand.
