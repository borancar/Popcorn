# Popcorn — porting notes

Everything needed to pick this up cold. Progress and what is next live in
[STATUS.md](STATUS.md); this file is the facts about the program.

## Where the facts are

This file is how to *work on* the port: what the binary is, the tools, the
conventions, and the traps that change what you do. What the program **is** -
its addresses, its data formats, its structures - lives in `docs/`, because it
is reference rather than working context and it was crowding this file out.

| | |
| --- | --- |
| [docs/memory-map.md](docs/memory-map.md) | the load image, every data and code address identified, the INT 09h handler, and how the EXEPACK recovery works |
| [docs/level-format.md](docs/level-format.md) | the 176-byte level record, the `.PPC` file, the cell values, the animated bricks |
| [docs/entities.md](docs/entities.md) | the entity chain, the ball structure, the capsules, the parachute, and a slip in the original's brick collision |
| [docs/video-and-sound.md](docs/video-and-sound.md) | CGA mode 05h and its palette, the 8x12 font, the PC speaker |
| [docs/utilities.md](docs/utilities.md) | POPSPEED, POPGEN and its level editor, and the cheat typed at the menu |
| [docs/the-game.md](docs/the-game.md) | what Popcorn is, its menu keys, and what kind of program the binary turned out to be |
| [reconstruct/popcorn.doc](reconstruct/popcorn.doc) | the authors' own readme - the primary source for the licence, the speed range and the menu keys |

Put a new fact about the program in `docs/`. Put a new fact about working on
it here.

## The goal

Port **Popcorn** (Christophe Lacaze / Frédérick Raynal, LACRAL software, 1988)
from its DOS binary to C on SDL: reverse-engineer from the disassembly, use an
emulator as ground truth, and check the port against it.

Two artefacts, and they check each other:

- `emulation.py` — the game running on an emulated 8086 with an SDL window.
  This is the **reference**, not the deliverable. It is what "correct" means.
- `reconstruct/` — the C on SDL3, the **deliverable**. Every reachable routine
  is transcribed; what is proven and what is not is [STATUS.md](STATUS.md)'s
  job to say, and it should be read before anything is claimed about it.

Nothing is finished because it looks right. A routine is done when the original
has been run against it and the two agreed - which is what `verify.py` and
`sidebyside.py` are for, and why most of the bugs found late were in code no
run had ever executed.

## The tools

```sh
# uv reads pyproject.toml, makes .venv, and pins every dependency in uv.lock.
uv sync

uv run unpack_popcorn.py         # recover the plain EXE
uv run validate.py               # prove the recovery
uv run emulation.py --scale 3    # play it
uv run analyze.py                # map the code segment
uv run tools_dis.py 0x1ad33 0x80 --seg 0x1ac2
```

| file | what |
| --- | --- |
| `CLAUDE.md` | this file: what is known about the program, and the conventions |
| `STATUS.md` | where the port has got to, what is proven, what is next |
| **Recovering the executable** ||
| `unpack_popcorn.py` | EXEPACK recovery by running the stub under Unicorn at segment 0x2000, which is the workaround for the stub's reliance on the 8086's 1 MB wrap |
| `validate.py` | proves the recovery: round-trips the emitted EXE against the stub's own output, and checks the C decoder agrees |
| **Emulating it** ||
| `pyproject.toml`, `uv.lock` | the dependencies, run with **`uv`**. `dos-emulator` is **pinned to a commit**: it is what every measurement here is taken against, so which version produced a number is part of the number. Moving the pin is deliberate — re-run `verify_routes.sh` after |
| `emulation.py` | **Popcorn's adapter onto the shared emulator.** The machine itself is `dos_emulator` (<https://github.com/borancar/dos_emulator>) — the DOS/BIOS shim, CGA, the IRQ 1 keyboard, the window. What is here is what is about *this game*: `GAME_DIR`, `UNPACKED`, `GAME_CODE = 0x1AC20`, and a command line that defaults to Popcorn so `python emulation.py --scale 3` still means what it always did. **Every tool here imports the emulator through this module**, so there is one place to look when the shared code moves |
| `drive.py` | drives **any** DOS program under the emulator by key, feeding the next one when the guest has drained the buffer and come back for more. `emulation.py --keys` times against the wall clock, which is right for the game and wrong for a program that sits waiting: the emulator's speed varies with what the guest is doing, so the same script reaches POPGEN's editor on one run and misses it on the next. `@tag` in the key list saves a screenshot, so a run documents itself, and `--dump` writes the guest's low 256K - press one key, dump, diff against a run that pressed nothing, and the byte that differs is what that key writes. That is how POPGEN's palette was measured |
| **Reading the code** ||
| `analyze.py` | recursive-descent map of the code segment, seeded with the entry point and every handler table the game dispatches through. `--listing` dumps each reachable routine |
| `tools_dis.py` | disassemble any range; `load_image()` is the loader everything else shares |
| `cycles.py` | what one play-loop frame costs the **original**, in 8086 cycles: hooks every instruction under the emulator and sums the iAPX 86/88 manual's table. This is how the port's frame rate is set - measuring the *port* only says how fast its own sleeps run, and adding up the delay loops misses everything they do not cover. It reports the mnemonics it could not cost, so the coverage of the estimate is visible rather than assumed |
| `coverage.py` | records which bytes actually execute, across several menu routes, into `coverage.bin` |
| `dump_data.py` | extracts a data structure and renders it back out — the 8x12 font at `0x9020` is the worked example |
| **Checking the port** ||
| `verify.py` | the differential harness: captures the machine at a routine's entry, lets the **original** run to its return, runs the C on the same capture, diffs image, video memory and return value |
| `port_coverage.py` | measures transcription by image offset, counting a routine only when its `1ac2:xxxx` header appears outside `stubs.c`. Also lists the functions the port **defines and never calls** - five, all accounted for - and the routines deliberately left as no-ops, which come out of both sides of the figure rather than counting as done or as remaining. `--verified` counts against `verify.c`'s dispatch: which transcribed routine has never once been run against the original. A routine transcribed and never wired up looks finished in the notes and has never run, which is how the `.PPC` loader sat complete and unreachable for months |
| `verify_all.py` | unions several `verify.py` routes - no single one reaches everything - and separates *proven* from *agreed but every call was an early return*. `--summary FILE` writes the coverage table STATUS.md carries |
| `snapshot.py` | capture and restore the whole machine, so a check can start *at* a screen instead of playing to it. `--at` stops on a routine, `--resume` chains, `--copy` and `--poke-str` edit a record. `--poke` lands when the file is **written**, so it seeds whatever resumes from it rather than steering the capture |
| `make_snapshots.sh` | builds the standard set of states the sweep runs from. Every one is reached by a rule - the routine only that state runs - not by a frame number someone watched go by |
| `verify_routes.sh` | the sweep itself: every route, unioned, `--chase`, and the summary written where STATUS.md can carry a measured number rather than a remembered one |
| `compare_screen.py` | diffs the port's `0xb8000` against the emulator's, byte for byte |
| `check_pointers.py` | the game keeps **16-bit pointers in its own data** - the chain's links, a cell parked in a slot, an animation cursor - and they go in through `global_off` and come out through `global_ptr`/`global_w`. This parses `game.c` with a C grammar and checks that: a field the game uses as a pointer is named `*_ptr`, a store into one goes through `global_off`, a read goes through a `_ptr` accessor, and what an accessor is handed is **simple**. That last rule is the one with teeth: `global_w(table + index * 2)` is pointer arithmetic without a pointer type, and the C that means it is `global_w(table[index])` with `table` typed. It also lists the fields already feeding an accessor without the suffix, which is the rename written down. Regex cannot do this - telling an argument from the expression inside it is the whole job |
| `sidebyside.py --sync-*` | `io_frame_sync` lives in the **play loop**, so everything outside it - the level intros, the results screen, the hall of fame - is compared by nothing at all. `--sync-scroll`, `--sync-endgame` and `--sync-results` add a sync point to one of those screens. Two real bugs lived on the results screen for months because no run could see it |
| `autoplay.py` | walks the menu and then plays: keeps the paddle under the ball, collects the capsules worth having, and catches a parachuted ball. Drives the **mouse**, because the game's mouse input is absolute and lands on the next frame. `--port` drives the **C port** instead, through the same lockstep protocol `sidebyside.py` uses, so the deliverable can be watched playing itself |
| **The port** ||
| `reconstruct/Makefile` | builds `reconstruct/popcorn` against SDL3 |
| `reconstruct/src/main.c` | **`popcorn`**: the game, with the command line the original has and no other - `popcorn` or `popcorn POPTAB`. That command line is part of what the port is, so nothing else is allowed to accumulate in it |
| `reconstruct/tools/devmain.c` | **`popcorn-dev`**: the same game with the flags that exist to *check* it - `--lockstep`, `--verify`, `--shot`, `--keys`, `--cmdline`, `--dump-image`. Every tool here runs this one |
| `reconstruct/tools/autoplay.c` | the bot, in C: `popcorn-dev --autoplay`. The same decisions `autoplay.py` makes, without the Python process, the emulator beside it and the pipe. It plays through `io_pin_mouse`, the door lockstep already uses, and reads the image without writing to it. It takes the paddle only - getting into a game is still F1 |
| `reconstruct/src/exepack.c` | the EXEPACK decoder in C, byte-identical to the Python one. The port reads the player's own `POPCORN.EXE` at startup |
| `reconstruct/src/game.h` | types, the three segment overlays, and the backend interface. The program keeps variables in three segments and there is a struct for each: **`gv`** (`game_vars`) is the data at image 0, **`cv`** (`code_vars`) is the code segment at `0x1ac20` - the `cs:[...]` bytes the assembly stored inside its own instructions - and **`c46`** (`seg_c46_t`) is the level and ending block at `0xc460`. Every field's offset is held by an `ENSURE_` assertion, so a wrong padding size fails the build instead of moving a field |
| `reconstruct/src/game.c` | the transcribed routines — all 181 of them — each carrying the `1ac2:xxxx` offset it was read from. Four more are here as no-ops with a comment saying why, and are counted as neither done nor outstanding |
| `reconstruct/src/sdl_io.c` | the platform layer: window, presentation, keyboard, mouse, and the retrace and delay hooks the game paces itself on |
| `reconstruct/src/stubs.c` | down to `entity_unknown`, a safety net for a handler address that is in no table. It was the to-do list and everything on it has landed; the two screens the port deliberately does not have are no-ops in `game.c`, not stubs here |
| `reconstruct/tools/verify.c` | the other half of `verify.py`: loads a captured state, calls one routine, writes back what it produced |
| **Not in the repository** ||
| `popcorn/` | the working copy the Python tools read, not committed. The committed copy is in `reconstruct/`, which is what the split repository publishes |
| `popcorn.unpacked.exe` | derived from it, and therefore just as copyrighted. Regenerated, not committed |
| `debug/`, `*.png` | screenshots and VRAM dumps from `shift+F10` |
| `venv/`, `coverage.bin` | build and run products |

### Reaching a screen to check it

Most of what was wrong in this port was in code no run had ever executed, so
the useful skill is getting a routine to run at all. Three things do it:

- **`snapshot.py --at OFFSET`** lands the machine on a routine and stops.
  `endgame_curtain` opens with 7,200 delay iterations, so a verify run from
  the bonus never reached `ending_column` in two minutes of emulated time; a
  snapshot taken *at* `0x538d` made it a sixty-second check.
- **`--poke`** fast-forwards rather than fakes: clearing the brick count at
  `0x2f10` is what the play loop watches for, so the game runs its own
  level-done path from there. Poking level 48 and clearing it is how the port
  came to play level 50.
- **`verify.py --keys`** presses from a resumed state. `screen_stash` and
  `screen_unstash` are the pause: `1ac2:1669` tests for `ax == 0x11b`, which
  is Esc, and nothing else calls them. `--keys @1a62:escape,@1a62:space`
  checks both in forty seconds.

- **Poke the odds, not just the state.** `extra_life` is 7/255 of a
  brick-2 capsule, which is why no run had ever reached it. Zeroing the
  cumulative weights at `0x33b1` up to V's entry makes `bonus_kind` return it
  every time, and since the poke is in the snapshot *both* sides see the same
  table - the comparison is as honest as any other, the rare path is just
  guaranteed. Four calls, identical.

- **`sweep_levels.sh`** compares any level without playing to it: it pokes
  the level *before* the one wanted and clears it, so `play_session` loads the
  wanted one normally. Most of the game had never been compared at all,
  because the bot could not get there.

  **Level 4 is in the standard set for a reason.** Its four-hit brick ends in
  the spinning 100 at `1ac2:366f`, whose counter at `[bx+2]` is a **byte** and
  whose `[bx+3]` holds whatever the recycled entity slot left there. Read that
  pair as a word and the counter never reaches zero, so the sprite is never
  taken off the screen - a divergence no other route in the sweep reaches, and
  one `verify.py` cannot see at all because it plays out across frames rather
  than inside a routine.

- **Two counters decide a two-player game**, and only one of them is obvious.
  `[0x3f08]` is how many players were entered and `[0x3f09]` how many are
  still in. `next_player` hands over to the next player while `[0x3f09]` is
  more than one and runs the hall of fame only when it reaches the last, so a
  hand-built two-player game over needs `0x3f08 = 2` **and** `0x3f09 = 1`.
  With both, `score_before` and `hsc_sort` run - and `score_before` has no
  other way in, since it exists to order two players' scores.

And **`verify_all.py --chase`**: a routine whose caller is being sampled is
never sampled itself, so a plain pass reports as unchecked a great many
routines it ran straight past.

### Running the game

```sh
uv run emulation.py --scale 3
uv run emulation.py --scale 3 --cmdline poptab     # the shipped levels
```

F1-F10 go to the game. The emulator's own controls sit behind **Shift**:
shift+F9 pause, shift+F10 capture to `debug/`, shift+F12 quit.

For an unattended run, `--keys` scripts input against wall clock and
`--shots N --shot-every S` writes PNGs headlessly:

```sh
uv run emulation.py --shots 4 --shot-every 4 --shot-dir debug \
    --keys 11:f1,14:b,15:o,16:b,18:return --cmdline poptab
```

Note that `--keys` times are **wall clock**, and the emulator's speed varies with
what the game is doing, so a script tuned on one run can miss on another.

### Layering, and where changes go

`dos_emulator` (DOS/BIOS shim → video, input, timing → window) → `emulation.py`
→ the tools. New behaviour goes in the **top** layer that can carry it.

The bottom layer's value is that it cannot modify the game directory — it serves
reads from the real files and satisfies writes from an in-memory overlay — and
weakening that to add a feature destroys the guarantee.

**Anything not about Popcorn belongs upstream, not here.** If a change would be
useful to another game, put it in `dos_emulator` and subclass or configure it
from `emulation.py`. If that means copying upstream code to change three lines,
upstream is missing an extension point: add the hook there. A forked copy stops
receiving fixes the moment it is made — and upstream has its own `CLAUDE.md`
saying not to break the projects that depend on it, of which this is one.

Large files are edited by one-shot anchored scripts (`edit_*.py`, git-ignored)
that assert each anchor occurs exactly once, do every replacement, then write the
file back once, so a failed anchor leaves the file untouched.

## Where the game lives

**In `reconstruct/`, and committed** - `popcorn.exe`, both `.ppc` sets,
`popgen.exe`, `popspeed.exe` and `popcorn.doc`. The authors put the program in
the public domain in `popcorn.doc`, asking only that it not be put to
commercial use without their prior agreement, so it travels with the port that
reads it and a fresh clone of the split repository plays.

`popcorn/` stays ignored: it is the working copy the Python tools read, and
two copies of the same bytes in one repository is one too many.
`POPCORN_GAME_DIR` moves it elsewhere for those tools.

`popcorn.hsc` is ignored wherever it appears - the game writes it, so it is a
save file rather than part of the game. `popcorn.unpacked.exe` is ignored too:
it is recovered from `popcorn.exe` in a second by `unpack_popcorn.py`.

The **port reads and writes everything relative to the current directory**, as
the original does - in DOS the game and its files *were* the current
directory. So copy `popcorn.exe` and the `.ppc` sets in beside the binary and
run it from there:

```sh
cp popcorn/popcorn.exe popcorn/*.ppc reconstruct/
cd reconstruct && ./popcorn LTF
```

`.gitignore` excludes them by file type as well as by directory, so copies
there are no more committable than the originals. `POPCORN_EXE` still points
at a POPCORN.EXE elsewhere, which is how the tools run from the repository
root.

The program is stated in its own readme to be public domain
("Ce programme fait parti du domaine public"), but that is the authors' word
about their own distribution, not a licence grant we can re-publish under, so
nothing of it is redistributed here.

## Two screens the port deliberately does not have

Both are understood and neither is transcribed. They are **no-ops with a
comment**, not gaps waiting to be filled, and they are out of `verify.c`'s
dispatch because comparing a no-op against the original reports a decision as
a difference.

- **F10, `employee_enter` at `1ac2:4ae0`.** The boss key: it stashes the
  screen, switches to text mode and paints a fake DOS prompt so the game can
  be hidden from whoever walks past. The port's F10 does nothing, and its
  caller skips `screen_restore` too - with nothing stashed there is nothing to
  put back.
- **F5, `screen_define_keys` at `1ac2:1581`**, with `read_new_key` (`0x1614`)
  and the prompt writer at `0x1642`. Redefining left, right and the key that
  launches the ball off the paddle, on a 40x25 text screen. The port keeps the
  defaults at `0x2d4f`-`0x2d51`, which are **J**, **K** and Space (0x24, 0x25, 0x39 - checked against the image, not the letters on a modern keyboard).

`0x1642` was called `copy_string_text` here, which described the instructions
it runs rather than the job it does. It writes one of those three prompts.

## Conventions

- **Levels are 0-based in the code and 1-based everywhere else.**
  `level_number` at `0x13cc` runs 0 to 49; the header bar draws it plus one.
  Snapshot files, the sweep scripts and the docs all use the **panel** number
  a player would say, zero-padded so `snapshots/` sorts: `L02.snap` through
  `L50.snap` - and there is no `L01`, because reaching a level means clearing
  the one before it. `sweep_levels.sh 15` gives you the level the panel calls
  15, which is record 14. See [docs/level-format.md](docs/level-format.md).

- Addresses are **image offsets** unless written `seg:off`. Every reconstructed
  routine carries the offset it was read from, so any line can be checked back
  against the binary.
- Where a name or a type is a guess, say so.
- **Geometry is decimal, addresses are hex.** A number that is a row, a
  column, a width, a pixel count or a scan line reads as a quantity and
  should be written as one: `reveal[7][1092]`, not `[7][0x444]`. Hex is for
  what the original means as an address or a bit pattern - an offset, a
  handler, a mask, a cell value. The same rule the padding sizes already
  follow. New code should be written this way; the existing constants are a
  sweep that has not been done - see STATUS.md.

- **Integer types are always `stdint`**: `uint8_t`, `uint16_t`, `uint32_t`,
  `int16_t`, `int32_t`. Never `unsigned`, `unsigned char`, `short` or `long`.
  This is a port of 16-bit assembly, where every value has a width the
  original depended on - `int16_t` says "this truncation is the `imul`'s" in a
  way `short` does not, and a bare `int` says nothing at all. `char` stays
  `char` for actual strings.
- The C port is **structured C that reads as a game**, not transliterated
  register-shuffling — checked against the emulator rather than assumed. If a
  routine genuinely cannot be written honestly as structured C, write it
  literally and say why.
