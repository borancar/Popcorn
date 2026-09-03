# Status

Where the port has got to, what is proven, and what is next.
Facts about the program live in [docs/](docs/README.md); how to work on the
port lives in [CLAUDE.md](CLAUDE.md).

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
took the reachable-routine count from 179 to 185.

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
ninth frame, and before the bot could aim, no run ever got past level 11.

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

**BP on the way out of the bonus, and a flag standing in for it.** The two
endings run the same curtain and the same `call 0x2034` inside it, but that
call is `push ds / mov ds, bp` first and BP is the data segment on only one
path: `1ac2:4636` on the floor ending is `mov bp, ds`, and the chamber ending
enters at `1ac2:4794` and never loads it. So the transition after a completed
level is black and after a lost one shows the level behind it. The port
carries a `cells` flag matched to that observation - what BP actually holds on
the chamber path is **not established**.

It cannot be staged: poking the ball into the upper chamber gives the chamber
ending the *floor* ending's registers, so the emulator draws the bricks and
the comparison answers a different question. What settles it is a register
dump at `1ac2:46ce` on a chamber ending the game played into - the technique
that found `DS = 0xc46` in the ending - which needs the bot through the funnel
for real. A brief flash of the level before the curtain, reported from
watching, is unresolved for the same reason.

**Two sync points are not trusted yet.** `--sync-intro` reports a large
difference from a capture at `1ac2:1c3f`, and the pictures are different
*moments*: the driver can ask the port to start at `play_loop` or at
`play_session` and a frame-close capture is neither, so `play_session` runs
its prologue and resets the score, the lives and the level. And the step check
only covered two of the six sync flags until recently, so the ending's "600
frames identical" was an unverified pairing - the bug it found is real, the
clean number after it is not yet worth what it looked like.


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

With that fixed, a resume tracks: from the level 4 snapshot of a full run, the
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
| **transcribed** | | 182 of 182 routines the port is meant to have, 24926 of 24926 bytes (100.0%) |
| **proven** | 159 | reached, did work, agreed on every route that reached it |
| shallow | 2 | agreed, but every call was an early return - not proof |
| differing | 1 | entity_paddle_fx |
| unreached | 3 | no route runs them |
| **dispatched** | 165 | what verify.c can check |

Routes in this union: play, menu, keyboard, level.snap, menu.snap +@0206:f8, menu.snap +@0206:f6, menu.snap +@0206:f10, menu.snap +@0206:f2, menu.snap +@0206:^l,@0206:^a,@0206:^c,@0206:^r,@0206:^a,@0206:^l,@0206:space,@0206:s,@0206:o,@0206:f,@0206:t,@0206:w,@0206:a,@0206:r,@0206:e,@0206:return, menu.snap +@0206:f2,@1785:p,@1785:o,@1785:p, level04.snap, bonus.snap, level11.snap +@1a62:escape,@1a62:space, level11.snap, level50.snap, capsule.snap, tall.snap, marks.snap, vlife.snap, lastball.snap, ending.snap, particles.snap, twoplayer.snap, twoplayer_go.snap, cleared.snap, holes.snap, play (chase), menu (chase), keyboard (chase), level.snap (chase), menu.snap +@0206:f8 (chase), menu.snap +@0206:f6 (chase), menu.snap +@0206:f10 (chase), menu.snap +@0206:f2 (chase), menu.snap +@0206:^l,@0206:^a,@0206:^c,@0206:^r,@0206:^a,@0206:^l,@0206:space,@0206:s,@0206:o,@0206:f,@0206:t,@0206:w,@0206:a,@0206:r,@0206:e,@0206:return (chase), menu.snap +@0206:f2,@1785:p,@1785:o,@1785:p (chase), level04.snap (chase), bonus.snap (chase), level11.snap +@1a62:escape,@1a62:space (chase), level11.snap (chase), level50.snap (chase), capsule.snap (chase), tall.snap (chase), marks.snap (chase), vlife.snap (chase), lastball.snap (chase), ending.snap (chase), particles.snap (chase), twoplayer.snap (chase), twoplayer_go.snap (chase), cleared.snap (chase), holes.snap (chase).

**Six routines are not in the 159, and each has a reason:**

- `set_crtc` (`0x4b7a`) programs the 6845 through ports 0x3d0-0x3db and
  touches no memory at all. So it agrees on every call and can never "do
  work", and it will sit in the shallow column for ever. It stays dispatched
  because "no memory side effects" is worth confirming even when it is the
  whole of what there is to confirm. Only the boss-key route reaches it: F10
  is pressed on the **emulator**, and verify.py calls the C routine itself, so
  the port having no boss key does not stop it being checked.
- `install_int09` (`0x03d1`) is shallow for the same kind of reason: the
  handler is the platform layer's here, so the C is a no-op that returns at
  once. Every call agrees and none of them does anything.
- `entity_paddle_fx` (`0x3386`) differs on **one call in eleven**, the one that
  fires the end-level bonus. `bonus_end_level` does not return to its caller -
  it pops four words off the stack and jumps into its body, so the body's `ret`
  goes four frames up into `play_session` - and `verify.py`, waiting for a
  return address that is never reached, lets the original run on into the next
  level while the C, which longjmps out at the same point, has stopped. The
  image diff says exactly that: the level number and the lives advanced on one
  side only. It stays dispatched: "1 of 11" with a known cause is a better line
  than a clean sweep bought by dropping the case.

  One attempt at fixing it is worth recording, because it was half right.
  `0x2da0` pops four words and jumps to `0x4210`, so the address that body
  will eventually return to is readable when `0x2da0` is entered - the pops
  leave `SP` eight above where the CALL left it. Stopping the original there
  **does** stop the level advancing: `[0x13cc]` came out of the diff. But the
  lives came out one *lower* than the C's, which is `play_session`'s
  `dec [0x13c9]` at `1ac2:0363` having already run, and the score higher. So
  the original lands somewhere past that, and `0x4210` does not end in the
  plain `ret` the four pops imply - it is a long routine full of jumps. The
  landing point has to be found rather than assumed, and until it is this is
  not a fix. Reverted.
- `cell_hole_draw` (`0x4cc1`) is reported unreached, and it is not. From the
  cleared level the between-level screen that draws a hole is minutes of
  emulated time away, further than the sweep gives any one route; a
  three-hundred-second run finds twenty calls of it and they are identical.
  The fix is in the state rather than the clock - `make_snapshots.sh` now takes
  a second snapshot **at** `1ac2:05f8`, the screen itself, so the route starts
  where the work is.
- `screen_stash` (`0x4ba9`) and `screen_unstash` (`0x4c13`) are the pause, and
  nothing reaches them. `1ac2:1669` tests `ax == 0x11b` and calls the first,
  then blocks on `int 16h` until a key decides between carrying on and
  abandoning the game. The documented route - `@1a62:escape,@1a62:space` -
  does not fire, and the reason is worth chasing rather than guessing at: the
  game owns INT 09h while a level runs, its handler stores only the three
  configured keys, and Esc is not one of them - so the BIOS buffer INT 16h
  reads from never sees it. That is the same mechanism that hid the player
  name box. Whether the original has the same problem on real hardware is the
  question to settle first.
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
quantity only on level 50, which is 168 of them and nothing else.
`field_marks_wide` could not be sampled while `panel_finish` was being
sampled, because the harness will not re-enter a routine whose caller it is
already inside; asking for the callee on its own found it in one run.

### Verification coverage is bounded by what a run reaches

A ten-minute play route verifies **87 routines byte-identical with nothing
failing**, which is half of them: the rest are not reachable by playing. Every failure that stood in this list for most of a session turned
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

The routines the three base routes miss need game states a bot does not play
into - the ending, a capsule of a particular kind falling, a second player out
of lives, a field with a hole in it. Naming them here went stale as fast as it
was written, because each one that got a snapshot stopped being an example.
The table above is the count, and `verify.py` prints the current list at the
end of every run.

The honest limit is that **a routine no run reaches is unproven**, and
`verify.py` prints that list rather than quietly omitting it. Snapshots are the
answer to it and now exist: `verify.py --resume` starts a check *at* a screen
instead of playing to it, which is how the animated bricks were checked at all.
Applying it to the rest of the unreached list is the outstanding job - it needs
snapshots taken at the states a bot does not play into, not just at level
starts. Input is otherwise scripted by code offset (`@13d2:return` fires the
first time execution reaches `0x13d2`), which is reproducible where a timed
script tuned on one run missed on the next.

### The screens outside the play loop were compared by nothing

`io_frame_sync` is called from one place: the play loop's frame close at
`1ac2:1c3f`. Everything the game draws outside that loop - the level intros,
the game-over sequence, the two-player results screen, the hall of fame - has
never been in a frame-by-frame comparison, and `verify.py` cannot reach most
of it either because those routines return on a key or do not return at all.

That is where the bugs were. Three found in one sitting, all reported by
someone playing rather than by any check here:

- `1ac2:1053`, the hall of fame's own copy into the scratch `hsc_sort` reads,
  **not transcribed at all** - it is a jump target, so the byte map counted it
  as part of `screen_results` and the figure stayed at 100%.
- `name_field`'s centring shift ran the wrong way and blanked every name.
- the two-player CLASSEMENT screen was a sketch: the top bar, then both
  players one scan line apart, with no heading, no rule and no panel behind
  them. The original draws the panel with `draw_run(' ', n)` - glyph 0 is a
  solid block of colour 2, so a run of spaces *is* the red ground.

`--sync-results` closes the results screen: `1ac2:1037`, its wait, once a
pass. Two hundred comparisons, identical. The level intros and the hall of
fame are still uncompared, and the same trick would close them.

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

1. **The frame cursor is the last shape without a type.**
   `check_pointers.py` reported 72 `compound-offset` sites; it reports
   **7**. What went was everything that turned out to be a record or a table
   in disguise: `particle_t` took 50 of them, `fall_frame_t` and
   `global_table_w` most of the rest, and `score_add` as three words rather
   than six digits took six more. Each was the same discovery - the
   arithmetic was doing what a type should.

   What is left is one shape, five of the seven sites:

   ```c
   xor_sprite_16x7(x, y, global_ptr(global_w(cur_ptr - 2)));  /* the previous frame */
   xor_sprite_16x7(x, y, global_ptr(global_w(cur_ptr)));      /* and the current one */
   s->frame_ptr = global_w(s->frame_ptr + 2)                  /* a cursor stepping */
   ```

   A cursor into a list of the game's 16-bit pointers, walked by `+ 2` and
   dereferenced twice. `cur_ptr - 2` is *the entry before this one* - which
   is why every frame list starts at index 1, since `entity_crumble` erases
   what the cursor has just left - and nothing in the expression says so.

   The obstacle is that a `uint16_t *` into the overlay is not available:
   `global_t` is packed and the lists sit at odd offsets, so GCC will not
   hand out a word pointer to one. `global_table_w` is the shape that works
   around it for a table with a runtime base; a cursor wants the same
   treatment, with `- 2` becoming `[-1]` and the double indirection one call.

   The two remaining sites are neither: `global_ptr(is_two ? ... : ...)` is
   two named screens picked by a flag, and `eog_build_at - EOG_WIDTH` is a
   row step in a buffer. Both want fields, not a cursor type.

2. **The end-level bonus, still.** Five transcription errors and one
   structural one have come out of it, and the two sides now play the whole
   screen together and into the next level: **9,000 frames identical** from
   , against 9 at the start of the day. Two things are still open on
   it, and they are different questions:

   A third has been closed, and it was never the port. `--from-bonus` had
   been reporting five bytes differing on every frame - `speed_step`,
   `speed_timer` and the tick of every live entity, each one lower on the
   port. `--resume` set `start_at = FRAME_END` unconditionally, a hundred
   lines after `--from-bonus` had set it to `BONUS_BODY`, and `start_at` is
   the field `lockstep.c` reads to decide where to rejoin. So the port was
   told it had resumed at the frame top, went in through `play_loop`, and
   ran one frame body the emulator never ran - which decrements exactly
   those five counters and nothing else, which is why the screens were
   identical the whole time. `--from-bonus` was never taken at all. The
   resume now reads the snapshot's own stopping point out of its registers
   rather than assuming one, and the route is **3,000 frames identical**,
   through the bonus and on into the next level.

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

3. **One routine nothing builds a state for.**

   `copy_string_text` at `0x1642` is inside `screen_define_keys`, which
   switches to text mode 01h. Excluded on purpose: the port has no text
   renderer, and that is a separate job from transcribing.
4. **Sound.** Both play it now. The port records the PIT divisor in
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
5. **`.PPC` levels are playable now.** `reconstruct/popcorn --cmdline poptab`
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

- **Levels 12 and beyond.** The bot has held level 11 for a quarter of a
  million frames without clearing it, so the levels after it are untested by
  the side-by-side. Reaching them wants a bot that aims rather than one that
  survives, or a snapshot taken with the level number written by hand.

- **Screen positions written as bare offsets, which should be `cga_at`.**
  `di = 0x177e`, `d = 0x1cd9`, `di = 0x20f2`, `bp = 0x3ef2` and a few dozen
  more are positions on the screen, and `cga_at(x, y)` is what computes one:
  `0x20f2` is `cga_at(8, 7)`, `0xf2` the same x a scan line up, `8` is
  `cga_at(32, 0)`. Writing them that way says where they are instead of
  leaving the interlace to be decoded by hand.

  The work is **extracting the x and y**, not the substitution: each offset
  has to be decoded and checked against what the routine draws, and a few are
  not positions at all. It pairs with the decimal sweep below, since the x and
  y that come out are quantities.

  Counted: about fifty sites - 25 `di = 0x....`, 22 more locals initialised
  the same way, and 3 handed straight to `vram_setw`. **All of them**, not a
  selection: a screen offset written as a number is the thing being got rid
  of, so leaving some is leaving the reader to wonder which kind each one is.

  Two things to carry through, found while decoding
  `screen_high_scores`. The starting offsets convert cleanly - its `0x2142` is
  exactly `cga_at(8, 9)` and the `0x142` above it `cga_at(8, 8)`, the bar one
  scan line over the heading. But the **stepping** is a separate question:
  that routine advances by `HSC_LINE`, 0x1b0, whose comment says "DI already
  +0x30" - a stride that assumes the cursor has moved by the width just drawn,
  which is not `cga_next_row` and does not become it. And every step is masked
  `& 0xffff`, which is the original's 16-bit DI wrapping; whatever replaces
  the constant has to leave that alone.

  This matters beyond legibility. `eog_saved` lives at image offset 0, so
  every screen offset is also a valid image offset, and the two kinds of
  address are indistinguishable as bare numbers - `eog_screen_at` is a VRAM
  offset and `eog_build_at` an image one, initialised on consecutive lines,
  and only the disassembly separates them. The same coincidence made
  `ball_at(0)` look like a ball and put three of level 9's comparisons wrong.
  `cga_at` on one side and `img_ptr` on the other make the kinds visible.

- **Geometry in hex, which should be decimal.** Rows, columns, widths, pixel
  counts and scan lines are quantities, and a great many of them are still
  written as hex because that is how the disassembly spells every immediate.
  `for (dl = 0x15; dl > 0; dl--)` is twenty-one rows; `si += 0x34` is a
  fifty-two byte stride; `bx < 0x35` is fifty-three columns. Each reads as an
  address and none of them is one.

  The rule is in CLAUDE.md and new code follows it. Converting what is there
  is a sweep of its own, and it wants doing carefully rather than by regex:
  the same literal is a quantity in one line and an address in the next, and
  `0x34` being both a stride and a screen offset in intro_reveal is exactly
  the case a blind substitution would get wrong. Nothing in it changes
  behaviour, so the check is that the binary is identical before and after.

- **screen_stash and screen_unstash are reached by no route, and the reason
  is the boss key.** `1ac2:4ba9` puts the playfield aside and paints an
  overlay over it; `1ac2:4c13` puts it back. The comment at `1ac2:4ba9` says
  "used by the pause screen and by F10", and F10 is `employee_enter`, which
  the port deliberately does not have. So this is a **different pause** from
  the Esc one, and it wants implementing alongside the boss key rather than
  before it.

  Reaching them meanwhile needs a keyboard-mode capture: the Esc test lives
  in the keyboard input routine at `1ac2:16d2`, and every level snapshot runs
  on the mouse, so `--keys @1a62:escape` never gets to it. `make_snapshots.sh`
  builds a keyboard route for exactly this and the sweep has not been re-run
  since the snapshots were renamed.

  They share `0x1aef` with the intro curtain, the ending's band and the
  results sort, and those three are checked - so the union is not what is
  unproven here, the two routines are.

- **A keypress ticks the menu's animation, and in the port it does not.**
  Every key that falls through the menu's compare chain lands at `1ac2:02b4`,
  which steps the particles and the banner once before going back to the key
  wait at `1ac2:0206`. The port's tail only presents. So in the original the
  decoration advances by one step per keypress on top of its idle rate, and in
  the port it does not. Nothing measures this: no lockstep route covers the
  menu, which is the same blind spot that hid the F8 ordering until the
  compiler pointed at it. Wants a menu sync point in `sidebyside.py` before it
  is worth touching.

- **Last, and opt-in: let a keypress skip an animation.** The menu's
  decoration, the level intros, the curtains - a key would cut them short
  rather than being swallowed. This is a **deviation**, not a fidelity fix,
  and the two must not be confused: it goes behind a flag, it goes in after
  everything that makes the port match, and the default stays the original's
  behaviour. Noted here so it does not get done by accident while fixing the
  entry above, which is the opposite kind of change.

- **Stretch: stop needing the packed struct at all.** The image is the port's
  authoritative store, so `game_vars` has to land on the game's own addresses
  and most of its words sit at odd offsets. That forces
  `__attribute__((packed))` and the explicit padding, and it is where the
  little-endian assumption comes from.

  The **mechanism** for having both is easy and not the problem: one struct
  definition, with `#ifdef` around the padding fields and the attribute -
  packed and padded when it must overlay the image, plain and naturally
  aligned when it need not. The work is in earning the right to compile the
  second one.

  Counted rather than guessed, as of this writing:

  - **The `g_image + CONSTANT` idiom is gone from `game.c`.** Not only the
    literals: every `#define NAME 0x...` that stood for an address has become
    a field, across **three** overlays, because the program keeps variables in
    three places -

    | | | |
    | --- | --- | --- |
    | `game_vars` | `gv` | the data segment, at image 0 |
    | `code_vars` | `cv` | the code segment, at `0x1ac20` - `cs:[0x84]` and its neighbours, variables the assembly stored *inside instructions* |
    | `seg_c46_t` | `c46` | the level and ending block, at `0xc460` |

    149 `ENSURE_` assertions hold their offsets, and what a wrong padding size
    would once have hidden now fails the build - which it did, twice, while
    this was being done.

  - **What is left is `g_image + <variable>`**, five sites: `SEG_C46 +
    gv.level_src`, `CS_BASE + si` three times, `SEG_14A1 + si`. Those are the
    game's own 16-bit pointers and they are the real blocker, not the idiom.
  - **29 `img_off` and 21 `ball_at` calls bridge struct to offset**, and about
    forty of those fifty are *not* fundamental. `ball_draw(img_off(b->sprite))`
    wants a `const uint16_t *`; `ball_step(img_off(&gv.balls[i]))` wants a
    `ball_t *`. They are offsets because the signatures take offsets, and the
    signatures take offsets because `verify.c` dispatches routines by address
    with the original's register arguments. Free the dispatch and they become
    pointers.
  - **What is left is where the original stores an image address in its own
    data**: the entity list's `E_NEXT` links, a cell address parked in an
    entity slot and compared against one (`cell == img_w(slot)`), a ball's
    offset in a slot. Those 16-bit values are the game's, not the port's, and
    they are the real blocker - 92 `img_w(<var> + …)` and 223
    `g_image[<var> + …]` sites walk them.

  So the order is: give the entity list and its slots a representation that is
  not an image offset; free the verifier's dispatch from register-shaped
  signatures; then the `#ifdef` is a small change and the unpacked build
  compiles - the `ENSURE_` macros are what will say when it does.

  What it buys: no packing, no padding, no endianness assumption, and a port
  that builds on a big-endian or strict-alignment target, which also makes the
  WASM route simpler.

  What it costs: the C stops mirroring the original's memory. The addresses in
  the comments stay true, the *shape* does not, and this port's claim is that
  it can be read against the disassembly. That is the line between a
  transcription and a reimplementation.
