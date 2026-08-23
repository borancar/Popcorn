# Status

Where the port has got to, what is proven, and what is next.
Facts about the program live in [CLAUDE.md](CLAUDE.md).

Updated 2026-08-23.

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

**181 of 181, all 24,916 bytes.** `port_coverage.py` measures it by the image
offset each routine carries, not by counting functions: a routine counts only
when its `1ac2:xxxx` header appears somewhere that is not `stubs.c`, so a stub
renamed to look finished does not move the number.

Four routines are out of both sides of that figure, not counted as done and
not counted as remaining: the boss key and the redefine-keys screen, 314 bytes
between them. They are no-ops with a comment saying why. `port_coverage.py`
finds them by looking for a body with no statements in it rather than by
carrying a list, so emptying a routine and forgetting to say so is not
possible.

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

**And every level is compared now.** `sweep_levels.sh` pokes the level
*before* the one wanted and clears it, so `play_session` loads the wanted one
normally, and the comparison starts from that level's own beginning. **All
fifty of the built-in levels** have now been compared frame for frame - 0 to
10 by playing to them, 11 to 49 by sweeping - and every one is identical.

The shipped sets are swept the same way, from a snapshot of a `--cmdline
poptab` run so the table in the snapshot is POPTAB's. **All fifty of POPTAB's
levels** are compared as well, and thirteen spread across LTF's - so between
the built-in table and the two shipped ones, a hundred and thirteen levels
have been checked frame for frame, and every one is identical.

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

**A snapshot resume is equivalent to the run it came from - now.** It was not,
for most of 2026-08-23: the level 5 divergence did not reproduce from a
snapshot taken *one frame before it*, while two full runs from the menu stopped
at the same frame with the same bytes. The cause turned out to be the bug
itself. The bonus has two endings that return to different places, the port was
guessing which, and the resumed run happened to take the other one and agree.
So the clean reproduction was never evidence, and reading it as one nearly
closed a bug that was still there.

With that fixed, a resume tracks: from the level 3 snapshot of a full run, the
resumed run reaches level 4 at relative frame 39,131 where the original reached
it at 39,133 - two frames, which is the resume's own counting rather than its
behaviour. Low memory is captured too now (the vector table and the BIOS data
area, neither of which is inside the load image), which was a real gap even
though it was not this one.

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
| **proven** | 155 | reached, did work, agreed on every route that reached it |
| shallow | 1 | agreed, but every call was an early return - not proof |
| differing | 1 | entity_paddle_fx |
| unreached | 1 | no route runs them |
| **dispatched** | 158 | what verify.c can check |

Routes in this union: play, menu, keyboard, menu.snap +@0206:f8, menu.snap +@0206:f10, menu.snap +@0206:f6, menu.snap +@0206:f2, menu.snap +@0206:^l,@0206:^a,@0206:^c,@0206:^r,@0206:^a,@0206:^l,@0206:space,@0206:s,@0206:o,@0206:f,@0206:t,@0206:w,@0206:a,@0206:r,@0206:e,@0206:return, menu.snap +@0206:f2,@1785:p,@1785:o,@1785:p, level10_f000000.snap +@1a62:escape,@1a62:space, cap.snap, level10_f000000.snap, level49_f000000.snap, particles.snap, tall.snap, marks.snap, vlife.snap, ending.snap, twoplayer2.snap, play (chase), menu (chase), keyboard (chase), menu.snap +@0206:f8 (chase), menu.snap +@0206:f10 (chase), menu.snap +@0206:f6 (chase), menu.snap +@0206:f2 (chase), menu.snap +@0206:^l,@0206:^a,@0206:^c,@0206:^r,@0206:^a,@0206:^l,@0206:space,@0206:s,@0206:o,@0206:f,@0206:t,@0206:w,@0206:a,@0206:r,@0206:e,@0206:return (chase), menu.snap +@0206:f2,@1785:p,@1785:o,@1785:p (chase), level10_f000000.snap +@1a62:escape,@1a62:space (chase), cap.snap (chase), level10_f000000.snap (chase), level49_f000000.snap (chase), particles.snap (chase), tall.snap (chase), marks.snap (chase), vlife.snap (chase), ending.snap (chase), twoplayer2.snap (chase).

**Three routines are not in the 155, and each has a reason:**

- `install_int09` (`0x03d1`) is a deliberate no-op - the platform layer owns
  the keyboard - so it agrees on every call and can never "do work". It will
  sit in the shallow column for ever.
- `entity_paddle_fx` (`0x3386`) differs on **one call in sixty**, the one that
  fires the end-level bonus. `bonus_end_level` does not return to its caller,
  so `verify.py` runs the original past the routine and compares it against a C
  function that stopped. The other fifty-nine agree, and the screen itself is
  covered by four million frames of lockstep. It stays dispatched: "1 of 60"
  with a known cause is a better line than a clean sweep bought by dropping the
  case.
- **F10 and F5 are not transcribed**, by decision rather than omission. The
  boss key hides the game behind a fake DOS prompt and the redefine-keys screen
  runs in text mode; both are no-ops with a comment saying so, and both are out
  of the dispatch. `0x1642`, which writes the redefine screen's prompts, went
  with them - it had been named `copy_string_text` after the instructions it
  runs rather than the job it does.

Everything else was reached by finding the trigger: a cheat typed, a key
pressed during the demo, Esc for the pause, the odds table poked so the rarest
capsule falls, two players entered with one still in. Proven went from 82 to
155 in a day without anyone reading a transcription looking for mistakes.

**Ten routines had never been dispatched at all**, and asking about them found
a real bug. `cell_special` draws what a cell of `0x0c` shows - the hole brick
11 leaves. The column arrives in `cl`; the C read it back out of `di`, the
destination, using the formula for the *cell index*: `(di - 0x2f18) % 12` on a
vram offset. `di` steps four bytes a column, so the column it computed cycled
with period three and it copied the wrong four bytes. Putting the old line
back: 14 of 34 calls differ. With the column passed in: 20 of 20 identical.

Four million frames of lockstep had never caught it, and could not have. No
level in the built-in table contains a `0x0c` cell - the value only appears at
runtime, after a brick 11 breaks - and the routine that draws it at a level
intro reads the level *table*. It is `POPGEN`'s levels that can hold one, so
the path was unreachable in everything the port had ever played.

Reaching the other nine took states nothing had built. Brick 11 exists in
quantity only on level 49, which is 168 of them and nothing else.
`field_marks_wide` could not be sampled while `panel_finish` was being
sampled, because the harness will not re-enter a routine whose caller it is
already inside; asking for the callee on its own found it in one run.

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

### The nineteen that are not dispatched, and why

`port_coverage.py --verified` counts against `verify.c`'s dispatch table,
which is the honest question: *which transcribed routine has never once been
run against the original?* It used to count against `verify.py`'s `RETURNS`
dict, which only lists routines that return something, so it answered a
question nobody had asked and put the figure at 9 of 185.

162 of 181 can be asked for. The nineteen that cannot fall into five groups,
and none of them is "not got round to yet":

| | why |
| --- | --- |
| `game_main` `0x0113`, `play_loop` `0x1873`, `bonus_end_level` `0x2da0` | **never return normally.** `play_session` leaves by longjmp and `bonus_end_level` throws four words off the stack. The harness reads `[SP]` as a return address and lets the original run to it; for these that address is somebody else's frame |
| `level_load_file` `0x08c8`, `load_high_scores` `0x4d96`, `hsc_save` `0x4dbb`, `drive_check` `0x4dea`, `drive_writable` `0x4e04`, `next_player` `0x0d2e` | **DOS file I/O.** The original talks to INT 21h and the port opens the file itself |
| `install_int09` `0x03b0`, the INT 09h handler `0x03e3`, `input_mouse` `0x1654`, `input_keyboard` `0x16d2`, `read_speed_setting` `0x5680` | **the platform layer owns these.** The port's keyboard is SDL's, and `read_speed_setting` reads interrupt vector 0x68 |
| `screen_player_names` `0x10de`, `name_field` `0x13b8`, `screen_high_scores` `0x4e1a` | **return on a key.** The C reads the port's keyboard and the original the emulator's, so the two return at different moments and the difference would be the harness |
| `speaker_on` / `speaker_off` `0x0085`, `screen_all_levels_done` `0x5940` | port writes and a screen that changes `DS`. The speaker pair writes no memory at all, so a check would compare nothing and pass whatever they did |

A check that cannot fail is worth nothing, which is why the speaker pair is
left out rather than dispatched for the count.

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

- **Nothing, on sound.** Both sides play it as of 2026-08-23.
- **Nothing, on `.PPC`.** The shipped level sets load and play; see above.
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

2. **One routine nothing builds a state for.**

   `copy_string_text` at `0x1642` is inside `screen_define_keys`, which
   switches to text mode 01h. Excluded on purpose: the port has no text
   renderer, and that is a separate job from transcribing.
3. **Sound.** Both play it now. The port records the PIT divisor in
   `io_sound` and tops the stream up every frame while the note lasts;
   `emulation.py` tracks channel 2's divisor across its two `out 0x42` writes
   and the gate at port 0x61, and `speaker_update` loops a square wave for as
   long as the note is held. Ninety seconds of play produces 23 distinct
   tones, every divisor with a low byte of 1 - which is what `out 0x42,1`
   followed by the note byte gives. That top-up is the part worth keeping - `sound_tick` only calls
   `io_sound` when the note **changes**, since it returns early at `1ac2:00a5`
   while one is being held, so queueing a fixed buffer played the first thirty
   milliseconds of every note and then silence. A note of ten ticks is about a
   sixth of a second. `emulation.py` is still silent.
4. **`.PPC` levels are playable now.** `reconstruct/popcorn --cmdline poptab`
   loads `POPTAB.PPC` over the built-in table, the way `POPCORN POPTAB` does:
   0x21b6 bytes into the block reached as segment 0xc46, six bytes in, valid
   when the file's own six-byte header repeats. The loader was transcribed
   long ago and had simply never been called - the port had no way to name a
   file. Reading the PSP tail is the machine's job and is not transcribed, so
   `--cmdline` builds the name at 0x1428 the way 1ac2:0157 does.

   Both shipped sets work - `poptab` and `ltf`, 8,630 bytes each - and a
   name that is not there prints the original's own message and stops.
   `poptab` is fifty levels the port had never played and runs 170,000 frames
   identical against the emulator; `ltf` is another fifty and runs 20,000.

## Deferred

- **Levels 11 and beyond.** The bot has held level 10 for a quarter of a
  million frames without clearing it, so the levels after it are untested by
  the side-by-side. Reaching them wants a bot that aims rather than one that
  survives, or a snapshot taken with the level number written by hand.
