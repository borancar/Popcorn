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

### Four hundred thousand frames come out byte for byte

`sidebyside.py` plays the emulator and the port together on the same driven
input and compares the whole image and the whole screen after every frame.
With `--from-session` it follows a whole game rather than one level, and it now
runs past **390,000 frames** without a single byte differing - not a byte of
the 133,296-byte image or the 16,384-byte screen, sound player included,
nothing masked but the stack and the three key-state bytes. That is around two
hours of continuous play, through eleven levels, and the run had not stopped
when this was written.

`--snapshots DIR` writes a resumable state at the start of every level and
`--resume FILE` starts from one, so a divergence two hours in is reached in a
couple of minutes rather than replayed. Nothing since the harness was built has
been found without it.

Five bugs came out along the way, each found by the harness rather than by
reading:

- **sound_tick read its tunes out of the sprite data.** The tune pointers are
  offsets into the code segment, because DS is the code segment for the
  length of the routine. Reading them as image offsets played whatever bitmap
  happened to be at 0xa.
- **entity_bonus** mishandled the no-move steer, bounced the wrong ball when
  more than one was out, and returned instead of falling through into the
  capsule being consumed.
- **bonus_move_down looked a whole row out** - `add al, 0xa`, ten pixels
  below the capsule, not two - and was missing its ending, where a capsule
  reaching the paddle row is handed to the script at 0x8320 rather than being
  blocked.
- **The animated bricks were not transcribed at all**, because the routine
  that runs them is reached only through an entity node. Level 7 is where the
  first of them is, and the port passed through cells the original turned into
  a running picture.
- **The brick table has thirty entries, not twenty-two.** Entries 24 to 29 -
  an animated brick that has already been hit - all point back at the solid
  handler, so hitting one again bounces the ball and does nothing else. The
  port had no case for those values and fell through, so the ball passed where
  the original bounced.

The last of those is the one worth remembering, because the harness reported it
as a **disagreement about which brick** and both sides were right. The
emulator's tag names the routine the dispatch reaches; the port's named the
cell value; and 4, 12 and 24 to 29 all share one routine. Two bytes differed -
a ball's bounce counter and a sound timer - for a difference of naming on top
of a real bug underneath it. The port now canonicalises its tag, which also
removes the same false report for cell 12.

The table length was itself the second thing to be measured wrong the same way.
`analyze.py --tables` audits every dispatch table and every word the code calls
through, and it missed this one because the length was **hand-written**. A
count you supply is not a count you checked; the fix was to read entries until
they stop being code addresses. It now reports "every dispatch target is
mapped" over thirty entries rather than twenty-two.

Earlier, two fixes to the harness rather than the port, both of which made it
*look* like the port was wrong:

- **1ac2:1a62 is not once a frame.** It is the top of the frame, but the serve
  wait reaches it too, at 0x1a58, whenever the action button is held - and a
  bot holds it permanently. The sync point is 1ac2:1c3f, the `jmp` that closes
  the frame, which is the one instruction both paths converge on.
- **`emu_stop()` leaves IP on the instruction it stopped at**, so the next
  `emu_start` ran it again and the hook fired a second time with no work in
  between. Four "frames" over one real one, comparing the port's frame N+1
  against the emulator's frame N.

The instruction counts say those frames are real rather than empty: the sound
tick, the ball's step gate and the entity walk each run once a frame and the
ball steps on two in three of them, exactly what the `[0x1486] = 3` gate
predicts, held over the whole run.

What is still set aside is the sound player's state at cs:[0xf4]-[0xf7] - the
request, the note timer and the tune pointer. One side raises a sound request
on a frame the other does not; because the request is both set and consumed
inside a single frame, that block is the only place it shows, and with sound
off none of it reaches the screen. Masking the timer alone simply moved the
first difference onto the pointer, which is the same divergence a byte later.

### The screens a bot never reached were the ones with the bugs in

Nothing had ever run the `+` capsule's end-of-level screen. `verify_all`
listed `bonus_end_level` unreached, and the frame comparison only got there
once the bot started collecting capsules deliberately. It had **five**
transcription errors in it, and the routines it calls had two more:

- The wall closing in copied its rows the wrong way and never advanced:
  `rep movsw` at `1ac2:4282` has SI on the band's row and DI one row below,
  and the C's `di = cga_prev_row(cga_next_row(di))` is the identity.
- The banner has two fixed rows before the level's cells, not one.
- Each level row is three banner rows - the cells twice, then a blank - and
  `si` walks **up** the level, not down.
- Three whole blocks were missing after the cells: seven more fixed rows, the
  funnel, and the mouth opening at `1ac2:44bd` whose two masks go on
  **crossed**, because the word is loaded little-endian and the mask written
  the other way round.
- `screen_scroll_up` read `mov cx, 7` - the `rep movsw` **word count** - as a
  row count and the `0x6f` row count as a pass count, so it scrolled seven
  rows a hundred and eleven times and drew the paddle a hundred and eleven
  times, where the original scrolls a hundred and eleven rows once.
- `ball_after_endgame`'s chamber test was inverted: `1ac2:45da` bounces the
  ball when x is **outside** `0x60..0x6b`, and the C bounced it when it was
  inside - in the gap, which is where it is supposed to go down and on. So
  the ball bounced off the opening and fell through the funnel wall.

Three of those were only findable after the harness stopped agreeing for
reasons that were not evidence, which is the recurring lesson here:

- **Return values were compared for three routines and none by flag.** Every
  routine that answers in the **carry** - `ball_redraw`, `ball_on_paddle`, and
  `play_loop` reads `jae` after each - could return the opposite decision and
  still be reported identical.
- **The pointer was never carried across.** `game_input` reads the mouse, and
  every between-level screen calls it through `input_and_draw_paddle`, so the
  C put the paddle wherever the checking process's pointer happened to be.
  `input_and_draw_paddle` differed on exactly half its calls and
  `screen_scroll_up` inherited it. The state file carries the emulator's
  pointer now, the same way it already carried the BIOS tick count.
- **`bonus_end_level` has two entry points**, `1ac2:2da0` and `1ac2:4210`, and
  the C merged them - so the harness, which enters at the second, was
  comparing a version that also tears the play loop down against one that
  does not.

### The port has finished the last level

On 2026-08-23 the C port played **level 49, the last of the fifty, to
completion**: 168 bricks of cell 11 and nothing else, from 158 down to 0, and
then `screen_all_levels_done` at `1ac2:5940` - the one place the game reaches
after the fiftieth level is cleared. The port says so itself now, because that
screen has no frame sync and a lockstep driver blocked while it runs cannot
tell "finished" from "hung".

Two honest qualifications. The port was **placed** at level 49 rather than
walking there: a snapshot poked onto level 48 and cleared, so `play_session`
loads level 49 the way it normally would. And it cannot yet walk there - it
stalls on level 10, whose row of cell 3 hardens into indestructible 4s and
walls the top of the field off, so a bot that survives indefinitely still
never clears it.

Getting there found the bug that would have stopped it. `brick_11` at
`1ac2:2d68` scores, sounds, turns the cell to 0x0c and draws, and **never
touches the ball** - no `inc [di+0x1d]`, no bp anywhere in it. The C routed it
through `brick_common`, which zeroes the bounce counter, so the
every-0x23-bounces slope shuffle fired at a different moment. On any other
level that is one brick in a mixed field; on level 49 it is every brick. The
two sides disagreed on the level's 189th frame and ran fifty-one thousand
frames together once it was fixed.

### What is still open on the end-level bonus

`bonus_end_level` is the last routine that differs, and the two that differ
with it - `entity_paddle_fx` and `screen_game_over` - only do so because they
call it. Five transcription errors have come out of it and it still differs.

`--sync-scroll` was built to get further: `io_frame_sync` is only reached at
the frame close in `play_loop`, so a screen with a loop of its own is one
indivisible step to the driver, which can then say the two sides differ
*afterwards* and nothing about where. Taking `screen_scroll_up` as a second
comparison point turns the bonus into fifty steps instead of one, and the
difference it reports drops from 2,889 screen bytes to 284.

Those 284 **are** a real difference, and it took two fixes to be able to say
so. The `resuming` flag that skips the hook's re-fire after `emu_stop`
remembered only *that* the emulator had stopped, not *where*, so with a second
sync point a genuine stop at the other address was swallowed as if it were the
re-fire. And the first desync check written for this compared the port's sync
count against the driver's window count - which always agree, because every
window consumes exactly one port frame. It looked like a safeguard and could
not fail, which is the trap this file keeps recording.

What can fail is comparing the *kind* of point each side stopped at, which the
tag now carries.

A third sync point, `--sync-endgame` on `ball_after_endgame`, reaches the one
part of the bonus the scroll sync does not: its own ball loop, which is
otherwise a single indivisible move. It found the two bugs five earlier passes
over this screen had missed, and they were both about where the ball starts
and stops:

- `1ac2:4518` **sets** the ball's position to `(0x70, 0xb4)` - the live pair at
  `+0` and the drawn pair at `+2` - and copies eight bytes of sprite record in
  from `0x48fb`. The C kept whatever position the *level's* ball had ended on,
  so the bonus's ball started wherever the last one died rather than at the top
  of the funnel.
- `1ac2:4738` is `cmp al,0x6c / jb no-bounce`, so the funnel's right wall
  catches the ball **at** `0x6c`. The C had `x > 0x6c` and it slipped through on
  the exact pixel.

The bonus now runs to completion and the game advances from level 7 to level 8:
**2,059 aligned comparisons, against 9 before**. What is left is smaller and the
same shape - the port's ball takes a little longer to reach a chamber, so the
emulator finishes the screen a window sooner and the port is one level
transition behind. `verify.py` also still reports a difference when it compares
the whole screen as a single call, which is a different question from whether
the screen plays correctly, and neither is claimed as the other.

One lesson worth keeping: **turn one instrument on at a time**. With the scroll
sync also enabled the same difference surfaced a hundred comparisons later and
looked like something else entirely.

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
| **proven** | 137 | reached, did work, agreed on every route that reached it |
| shallow | 8 | agreed, but every call was an early return - not proof |
| differing | 3 | bonus_end_level, bonus_end_level_body, entity_paddle_fx |
| unreached | 12 | no route runs them |
| **dispatched** | 160 | what verify.c can check |

Routes in this union: play, menu, keyboard, menu.snap +@0206:f8, menu.snap +@0206:f10, menu.snap +@0206:f6, menu.snap +@0206:f2, cap.snap, level10_f000000.snap, level49_f000000.snap, particles.snap, tall.snap, marks.snap, play (chase), menu (chase), keyboard (chase), menu.snap +@0206:f8 (chase), menu.snap +@0206:f10 (chase), menu.snap +@0206:f6 (chase), menu.snap +@0206:f2 (chase), cap.snap (chase), level10_f000000.snap (chase), level49_f000000.snap (chase), particles.snap (chase), tall.snap (chase), marks.snap (chase).

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
before. Proven went from 82 to 141 in one session purely by reaching further,
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
     earlier, runs 9,000 frames clean. So the snapshots do not capture
     everything a long run depends on, and that is a limit on the harness
     rather than a fact about the port. Worth knowing before trusting a
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
