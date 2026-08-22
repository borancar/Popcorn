# Popcorn — porting notes

Everything needed to pick this up cold. Progress and what is next live in
[STATUS.md](STATUS.md); this file is the facts about the program.

## The goal

Port **Popcorn** (Christophe Lacaze / Frédérick Raynal, LACRAL software, 1988)
from its DOS binary to C on SDL, the way [`../Ducks/`](../Ducks/) was done:
reverse-engineer from the disassembly, use an emulator as ground truth, and
check the port against it.

Two artefacts, and they check each other:

- `emulation.py` — the game running on an emulated 8086 with an SDL window.
  This is the **reference**, not the deliverable. It is what "correct" means.
- a C reconstruction on SDL3 — the **deliverable**. Not started yet.

`native.py` (routines hooked at their entry points and reimplemented in Python,
each checked against the code it replaces) is the bridge between them, as in
Ducks. Not written yet.

## The game

An Arkanoid clone. CGA only, keyboard or Microsoft-compatible mouse. French.
`popcorn.doc` is the original readme; it documents the menu:

| key | |
| --- | --- |
| F1 | play |
| F2 | demo |
| F3 | mouse control |
| F4 | keyboard control |
| F5 | redefine keys |
| F6 | high-score table |
| F8 | pick a colour palette |
| F9 | sound on/off |
| F10 | "touche spéciale pour employés" |
| Esc | menu → DOS; in game → pause |

Level sets are `.PPC` files made with the shipped `POPGEN.EXE`; `POPCORN POPTAB`
loads `POPTAB.PPC`. `POPSPEED.EXE` sets the game speed (0-30000, default 110,
lower is faster) for machines faster than an 8 MHz 8086.

Shipped files, in `popcorn/` (never committed):
`popcorn.exe`, `popcorn.doc`, `popcorn.hsc` (high scores), `popspeed.exe`,
`popgen.exe`, `poptab.ppc`, `ltf.ppc`, and two batch files.

## What the binary is

**Hand-written 16-bit x86 assembly.** Not compiled. The evidence:

- **zero** `push bp; mov bp,sp` sequences in 23,696 bytes of code
- no C runtime at all — the entry point copies the PSP command tail and starts
  working
- one code segment, one flat data area, `DS = 0` throughout, data addressed as
  absolute offsets (`mov byte ptr [0x2d4f], al`)
- arguments in registers, threaded across calls (`dl` as a row counter, `bx`
  as a width, `si`/`di` as cursors)
- flags poked into the code segment itself (`cs:[0x84]`, the sound-enable bit,
  written by the INT 09h handler)

That makes the port easier than a compiled one: there are no compiler idioms to
reverse, and every instruction is a decision someone made.

### It is EXEPACK-compressed

`popcorn.exe` is 103,848 bytes on disk and expands to 133,296. The MZ header
carries **zero** relocations and an entry point 16 bytes from the end of the
file — both belong to the unpacker stub. `unpack_popcorn.py` recovers it:

```sh
venv/bin/python unpack_popcorn.py     # -> popcorn.unpacked.exe
venv/bin/python validate.py           # round-trips it against the stub
```

EXEPACK header (16-byte variant, no `skip_len`): `real_cs:ip = 1ac2:0113`,
`real_ss:sp = 1aa2:0200`, `dest_len = 0x208b` paragraphs, `exepack_size =
0x178`. 35 relocations, agreed on by two independent readings — the stub's own
table, and a diff of two unpacks at different load segments.

**The unpack relies on the 8086 wrapping addresses at 1 MB.** The stub walks its
pointers downwards and renormalises with `or si,0xfff0`, which drives the
segment register below zero; on real hardware the address wraps, in Unicorn's
flat memory it escapes to 0x10eea1 and the stub prints "Packed file is corrupt".
Loading at segment 0x2000 instead of the 0x110 DOS would pick keeps the segment
non-negative and sidesteps it. The image is normalised back to segment 0 before
being written out.

## Layout of the unpacked image

Linear offsets into `popcorn.unpacked.exe`'s load image, which is the address
convention every note and every reconstructed routine here uses.

| range | what |
| --- | --- |
| `0x00000`-`0x1ac20` | data: sprites, fonts, level tables, strings, buffers |
| `0x1ac20`-`0x208b0` | **the code**, one segment `0x1ac2`, 23,696 bytes |

`DS = 0` for the whole program, so a data reference `[0x2d4f]` is image offset
`0x2d4f`, and a code address `1ac2:03e3` is image offset `0x1b003`.

### Data addresses identified so far

| offset | what |
| --- | --- |
| `0x13a0` | PSP command tail, copied there at startup |
| `0x1405` | saved `SP` for the return-to-menu longjmp |
| `0x1428` | level filename being built (`<tail>.PPC`) |
| `0x13e9` | which player-name box is being edited, ASCII `'1'`.. |
| `0x2d41` | saved BIOS INT 09h vector (offset, then segment at `0x2d43`) |
| `0x2d45` | current screen handler pointer |
| `0x2d47` | selected input handler: `0x16d2` = keyboard, `0x1654` = mouse |
| `0x2d49` | last make scan code seen by the INT 09h handler |
| `0x2d4a` | last direction: 0 = left, 1 = right |
| `0x2d4c` | action key held |
| `0x2d4d` | right key held |
| `0x2d4e` | left key held |
| `0x2d4f` | **left** key scan code (default `0x24`, K) |
| `0x2d50` | **right** key scan code (default `0x25`, L) |
| `0x2d51` | **action** key scan code (default `0x39`, Space) |
| `0x344f` | player-name table, `0x11b` bytes per player |
| `0x3f08` | players entered so far |
| `0x2d40` | paddle repeat counter, counts down to the next allowed step |
| `0x2d4b` | paddle repeat divider; decremented while a key is held, so the paddle accelerates |
| `0x2e54` | **paddle x**, the left edge in pixels |
| `0x2d3e` | lowest paddle position (8) |
| `0x2d3f` | highest paddle position (172) |
| `0x2ea1` | **ball pool**: four entries of `0x1e` bytes |
| `0x3138` | head node of the entity list; its `+0x0c` link is `0x3144` |
| `0x3142` | previous-node cursor, for unlinking |
| `0x3144` | first entity link; `0xffff` terminates the chain |
| `0x3146` | entity node pool, stride `0x0e` |
| `0x313a` | "remove me" flag an entity handler sets |
| `0x33d2` | PRNG state, advanced by `0x5ec5` per call |
| `0x3164` | ten words the PRNG folds in |
| `0x9020` | **font**: 40 glyphs of 8x12, 24 bytes each |
| `0x2f10` | the level being played: an 8-byte header then 12x14 cells |
| `0x3044` | brick behaviour table, **thirty** words indexed by cell value |
| `0x3080` | what each hit animated brick became, indexed by the new cell value |
| `0x3134`, `0x3135` | the animation script's counter and its reload |
| `0x3136` | the animation script pointer, into the block at segment `0x14a1` |
| `0x13c9` | lives |
| `0x13ca` | offset of the current level in the table |
| `0x13cc` | level number, 0-0x31 |
| `0x13cd` | the score, as eight ASCII digits |
| `0x13d5` | the player's name, 12 characters |
| `0x1487` | **the frame delay**, reloaded from `0x1489` every frame |
| `0x1485`, `0x1486` | how often the ball is allowed to step, and the limit |
| `0x2d0d` | table of four paddle sprite bases |
| `0x2e73` | clear when the last ball is lost |
| `0x4903` | the paddle sprites: four sets of four pre-shifted 7x44 images |
| `0xc46c` | **the level table**: fifty records of 176 bytes |
| `0x10250` | 32,000-byte backup of the CGA screen (`0xc46:0x3df0`) |

### Code addresses identified so far

Segment-relative (add `0x1ac20` for the image offset).

| offset | what |
| --- | --- |
| `0x0085` | `speaker_on` — PIT ch2 to mode 3, gate and data bits on |
| `0x0090` | `speaker_off` |
| `0x0097` | `sound_tick` — steps the current tune, writes PIT ch2 |
| `0x0106` | `flush_keys` — drain the BIOS INT 16h buffer |
| `0x0113` | **entry point**; startup, then the main menu loop at `0x0206` |
| `0x03b0` | `install_int09` — save the BIOS vector, install `0x03e3` |
| `0x03d1` | `restore_int09` |
| `0x03e3` | the INT 09h handler (see below) |
| `0x02d4` | F1: the play path |
| `0x10de` | the player-name boxes |
| `0x13b8` | one name field, via INT 21h AH=07h |
| `0x164c` | `delay` — `push cx; mov cx,N; loop $; pop cx`, N patched by POPSPEED |
| `0x1654` | mouse input handler |
| `0x16d2` | keyboard input handler |
| `0x5099` | `save_screen` — 0xb800 both halves to `0xc46:0x3df0` |
| `0x50bc` | `restore_screen` |
| `0x0c64` | `draw_char` — one 8x12 glyph to `ES:DI`, stepping the interlace |
| `0x1873` | **the play loop**: serve, entity walk, ball stepping, collision |
| `0x169f` | `input_mouse` tail: `paddle = clamp(mouse x / 2)`, buttons = action |
| `0x172f` | `input_keyboard` tail: one pixel per repeat tick |
| `0x27d7` | `ball_step` — the Bresenham stepper |
| `0x3257` | unlink an entity from the list |
| `0x40c0` | `random` — BIOS ticks, ten words at `0x3164`, and an LCG at `0x33d2`; returns `AH = value % DL` |
| `0x1c4f` | the level intro: the border, the lives, and a figure walking the paddle row |
| `0x1e50` | one 12x7 sprite, shifted to a pixel x and XORed in at the paddle row |
| `0x2281` | `blit_xor` - seven rows of eleven bytes, XORed |
| `0x22de` | `paddle_row_offsets` - seven CGA offsets from an x |
| `0x2f5` | `play_session` - a whole game, level by level |
| `0x5680` | `read_speed_setting` - POPSPEED's value, out of **interrupt vector 0x68** |
| `0x14b3` | `build_shifted_sprites` - generates three of every four sprite phases at startup |
| `0x5630` | the screen blit: waits on 0x3da bit 3, then `rep movsb` per row |
| `0x2ccd` | `brick_animated` - cells 16-21, the pieces of a running picture |
| `0x3abf` | the entity that keeps one of those pieces animating |
| `0x3bac` | `draw_anim_cell` - eight rows of four bytes, copied not XORed |

### The INT 09h handler, `0x03e3`

It does **not** chain to the BIOS, so the INT 16h buffer stops filling while it
is installed. The game therefore installs it for play and takes it out again for
the menus, and anything feeding keys in has to follow that. It:

- reads port 0x60, acknowledges via port 0x61 (set bit 7, restore)
- sets `ah` = 1 for a make, 0 for a break
- notes the direction at `0x2d4a` if the code matches the left or right key
- toggles the sound-enable flag `cs:[0x84]` on scan code `0xc3` (F9 break)
- stores the make code at `0x2d49`
- `repne scasb` over the three configured keys and stores the make/break flag
  into `0x2d4c + cx` — so left lands at `0x2d4e`, right at `0x2d4d`, action at
  `0x2d4c`
- EOI to port 0x20, `iret`

## Video

CGA **mode 05h**, set once with INT 10h AX=0005 and never changed. 320x200,
four colours, two bits per pixel, most significant pair leftmost.

- Memory at `0xb8000`, **interlaced**: even scan lines from offset 0, odd from
  offset `0x2000`, 80 bytes to a row either way. The game's own row-stepping
  idiom is visible all over the code:
  `cmp di,0x2000; jb +; sub di,0x1fb0; jmp ++; +: add di,0x2000`.
- Mode 05h sets the colour-burst-kill bit in the mode-control register, which on
  an RGB monitor selects the third, often-forgotten four-colour palette:
  **background / cyan / red / white**. That is what the game is drawn in.
- The game never writes 0x3d8 itself, so the BIOS defaults for mode 05h are what
  matter. F8 cycles the colour-select register, 0x3d9.
- It waits on port 0x3da bit 3 (vertical retrace) around its blits.

## Sound

PC speaker: PIT channel 2 (port 0x42) with the gate at port 0x61 bits 0-1.
`sound_tick` at `0x0097` walks a table of (divisor, duration) word pairs; the
tune pointers are at `cs:0xf8` and the enable flag at `cs:0x84`. Not yet
modelled — the emulator is silent.

## The tools

```sh
python -m venv venv
venv/bin/pip install capstone unicorn pygame-ce numpy

venv/bin/python unpack_popcorn.py         # recover the plain EXE
venv/bin/python validate.py               # prove the recovery
venv/bin/python emulation.py --scale 3    # play it
venv/bin/python analyze.py                # map the code segment
venv/bin/python tools_dis.py 0x1ad33 0x80 --seg 0x1ac2
```

| file | what |
| --- | --- |
| `CLAUDE.md` | this file: what is known about the program, and the conventions |
| `STATUS.md` | where the port has got to, what is proven, what is next |
| **Recovering the executable** ||
| `unpack_popcorn.py` | EXEPACK recovery by running the stub under Unicorn at segment 0x2000, which is the workaround for the stub's reliance on the 8086's 1 MB wrap |
| `validate.py` | proves the recovery: round-trips the emitted EXE against the stub's own output, and checks the C decoder agrees |
| **Emulating it** ||
| `trace_dos.py` | headless DOS/BIOS shim — INT 21h, 16h, 33h, the IVT, the PSP. **Read-only** on the host filesystem: writes are satisfied from an in-memory overlay |
| `emulation.py` | `VgaDos` on top of it: CGA modes 4/5/6, ports 0x3d8/0x3d9, retrace on 0x3da, IRQ 1 keyboard, INT 10h pixels, and the SDL window. The reference the port is measured against |
| `sb.py`, `xms.py` | Sound Blaster and XMS models inherited from Ducks. Popcorn uses neither — it is CGA and PC speaker — but `emulation.py` wires them in and they cost nothing |
| **Reading the code** ||
| `analyze.py` | recursive-descent map of the code segment, seeded with the entry point and every handler table the game dispatches through. `--listing` dumps each reachable routine |
| `tools_dis.py` | disassemble any range; `load_image()` is the loader everything else shares |
| `coverage.py` | records which bytes actually execute, across several menu routes, into `coverage.bin` |
| `dump_data.py` | extracts a data structure and renders it back out — the 8x12 font at `0x9020` is the worked example |
| **Checking the port** ||
| `verify.py` | the differential harness: captures the machine at a routine's entry, lets the **original** run to its return, runs the C on the same capture, diffs image, video memory and return value |
| `port_coverage.py` | measures transcription by image offset, counting a routine only when its `1ac2:xxxx` header appears outside `stubs.c` |
| `compare_screen.py` | diffs the port's `0xb8000` against the emulator's, byte for byte |
| `autoplay.py` | walks the menu and then keeps the paddle under the ball, for unattended runs. Drives the **mouse**, because the game's mouse input is absolute and lands on the next frame |
| **The port** ||
| `reconstruct/Makefile` | builds `reconstruct/popcorn` against SDL3 |
| `reconstruct/main.c` | argument handling, the load, and the call into `game_main` |
| `reconstruct/exepack.c` | the EXEPACK decoder in C, byte-identical to the Python one. The port reads the player's own `POPCORN.EXE` at startup |
| `reconstruct/game.h` | types, the named image offsets, and the backend interface |
| `reconstruct/game.c` | the transcribed routines — all 179 of them — each carrying the `1ac2:xxxx` offset it was read from |
| `reconstruct/sdl_io.c` | the platform layer: window, presentation, keyboard, mouse, and the retrace and delay hooks the game paces itself on |
| `reconstruct/stubs.c` | what is not transcribed. Down to `entity_unknown`, a safety net for a handler address that is in no table |
| `reconstruct/verify.c` | the other half of `verify.py`: loads a captured state, calls one routine, writes back what it produced |
| **Not in the repository** ||
| `popcorn/` | the game itself, excluded by directory and again by file type. Never committed |
| `popcorn.unpacked.exe` | derived from it, and therefore just as copyrighted. Regenerated, not committed |
| `debug/`, `*.png` | screenshots and VRAM dumps from `shift+F10` |
| `venv/`, `coverage.bin` | build and run products |

### Running the game

```sh
venv/bin/python emulation.py --scale 3
venv/bin/python emulation.py --scale 3 --cmdline poptab     # the shipped levels
```

F1-F10 go to the game. The emulator's own controls sit behind **Shift**:
shift+F9 pause, shift+F10 capture to `debug/`, shift+F12 quit.

For an unattended run, `--keys` scripts input against wall clock and
`--shots N --shot-every S` writes PNGs headlessly:

```sh
venv/bin/python emulation.py --shots 4 --shot-every 4 --shot-dir debug \
    --keys 11:f1,14:b,15:o,16:b,18:return --cmdline poptab
```

Note that `--keys` times are **wall clock**, and the emulator's speed varies with
what the game is doing, so a script tuned on one run can miss on another.

### Layering, and where changes go

Same rule as Ducks: `trace_dos.py` → `emulation.py` → `native.py`. New behaviour
goes in the **top** layer. `trace_dos.py`'s value is that it cannot modify the
game directory — it serves reads from the real files and satisfies writes from
an in-memory overlay — and weakening that to add a feature destroys the
guarantee.

Large files are edited by one-shot anchored scripts (`edit_*.py`, git-ignored)
that assert each anchor occurs exactly once, do every replacement, then write the
file back once, so a failed anchor leaves the file untouched.

## The game is never in the repository

`popcorn/` is git-ignored by directory name and again by file type
(`*.exe`, `*.ppc`, `*.hsc`, `*.doc`, `*.bat`). `popcorn.unpacked.exe` is derived
from it and therefore just as copyrighted; it is regenerated, not committed.
`POPCORN_GAME_DIR` moves the game directory elsewhere.

The program is stated in its own readme to be public domain
("Ce programme fait parti du domaine public"), but that is the authors' word
about their own distribution, not a licence grant we can re-publish under, so
nothing of it is redistributed here.

## The entity system

The play loop at `0x1873` walks a **linked list** and calls each node's handler:

```
1b4d  mov word [0x3142], 0x3138   ; the previous-node cursor, at the head
1b53  mov bx, [0x3144]            ; the first link
1b57  cmp bx, 0xffff / je done
1b5d  push bx / call word ptr [bx] / pop bx
1b61  cmp byte [0x313a], 0        ; did the handler ask to be removed?
1b68  mov [0x3142], bx / mov bx, [bx+0xc] / jmp 1b57
1b71  mov cx, [bx+0xc] / call 0x3257 / mov bx, cx / jmp 1b57
```

So a node is `+0x00` its per-frame handler and `+0x0c` the next link, the pool
is at `0x3146` with stride `0x0e`, and `0xffff` ends the chain. **Nothing that
follows control flow can reach a handler**, which is why static reachability
stopped at 62.4%; walking the list while the game played found eight of them
(`0x3273`, `0x3386`, `0x3561`, `0x3717`, `0x390d`, `0x39fa`, `0x3aee`,
`0x3b2a`) and took it to 76.9%.

The node layout, as far as `0x39fa` (a ball's handler) reads it:

| offset | what |
| --- | --- |
| `+0x00` | the per-frame handler — **and it is rewritten in place**, so a node is a small state machine |
| `+0x04`, `+0x05` | two bytes passed to the draw call as `cl` and `al`; position, most likely |
| `+0x06` | a pointer to a pointer to the sprite |
| `+0x08` | flags; the low nibble is the kind |
| `+0x0c` | the next link |

`0x39fa` is worth reading whole, because it independently confirms the ball
structure: on a bounce it sets the anchor from the live position with one
`mov ax,[di] / mov [di+0x18],ax`, zeroes the accumulators at `+0x1a`, flips
both direction flags, and picks a fresh slope with two `random(7) + 1` into
`+0x16` and `+0x17`. It also writes `cs:[0xf4] = 6` to start a sound, and
replaces its own handler with `0x3aee`. `[0x33d4]` carries the collision result
the step produced.

## The ball structure

Four entries of `0x1e` bytes at `0x2ea1`, stepped by `ball_step` at `0x27d7`.

| offset | what |
| --- | --- |
| `+0x00`, `+0x01` | **the live position**, in pixels |
| `+0x14`, `+0x15` | direction flags; non-zero negates that axis (`+0x15` set = moving up) |
| `+0x16`, `+0x17` | the slope, stored **(dy, dx)** |
| `+0x18`, `+0x19` | the anchor: where the current straight segment began |
| `+0x1a`, `+0x1b` | Bresenham accumulators, counting away from the anchor |
| `+0x1c` | state: 0 idle, 1-2 in play |

Two traps, each of which cost a debugging round:

- **`+0x18`/`+0x19` is not the position.** It is the anchor, and it does not
  move again until the next bounce. The live position is `+0x00`/`+0x01`, which
  matches the drawn sprite to the pixel.
- **The slope pair is stored (dy, dx).** Both branches of the stepper come out
  as `x_offset / y_offset = [+0x17] / [+0x16]`. Reading it the other way round
  makes a predicted landing point wrong by the square of the slope.

## The level format

Fifty levels of 176 bytes at image `0xc46c` - the block the program reaches as
segment `0xc46`. `play_session` copies one at a time to `0x2f10`, and the play
loop watches the first byte of that copy to know when the level is cleared.

| offset | what |
| --- | --- |
| `+0x00` | **brick count** - checked against the non-zero cells of four levels and exact every time |
| `+0x01` | a second count, used by `0x36fb` |
| `+0x02` | a short list `0x36fb` walks, `[+0x01]` entries long |
| `+0x08` | the cells: **12 columns by 14 rows**, one byte each |

### The animated bricks

Cell values 16 to 21 are not bricks but the six pieces of one picture, and nine
of the fifty levels use them - 7, 9, 20, 23, 30, 34, 39, 42 and 45, always all
six together. Hitting one runs `0x2ccd`, which **adds eight** to the cell
rather than clearing it, draws the piece from the level's animation script, and
leaves an entity running `0x3abf` behind; that entity redraws the piece every
time the script steps, so the picture goes on moving after it has been broken.
Hitting the marked cell again lands on table entries 24 to 29, which all point
back at the solid handler: it bounces and nothing more.

`0x3abf` is reachable only through an entity node - **nothing calls it** - so
anything following control flow will count its bytes as the tail of whatever
routine precedes it.

The geometry is not a guess. At twelve wide the first level reads as four bands
of two solid rows alternating between cell values 2 and 1, which is exactly
what the game draws; at any other width it is diagonal nonsense.

`POPGEN.EXE` writes `.PPC` files of the same shape - `poptab.ppc` is 8,630
bytes against the built-in table's 8,800 - but the loader at `0x08c8` has not
been read yet, and the port uses the built-in table.

## The font

`draw_char` at `0x0c64` maps a character to a glyph index — `' '` to 0, `'-'`
to `0x0b`, `':'` to `0x26`, `0xff` to `0x27`, `'0'`-`'9'` to `al - 0x2f`,
`'A'`-`'Z'` to `al - 0x35` — multiplies by 24 and indexes a table at `0x9020`.
Each glyph is 12 rows of one word: **8x12 at two bits per pixel**. The
destination steps the CGA interlace and backs `DI` up two bytes each row to
stay in one column.

Glyph 0, what a space maps to, is **not blank**: it is a solid block of colour
2, which is how the game paints the red bars its headings sit on.
`dump_data.py font` renders the sheet, which is the check — a wrong stride is
obvious at a glance and invisible in a hex dump. This is the score-panel font;
the menu uses a second, larger one that has not been located yet.

## Conventions

- Addresses are **image offsets** unless written `seg:off`. Every reconstructed
  routine carries the offset it was read from, so any line can be checked back
  against the binary.
- Where a name or a type is a guess, say so.
- **Integer types are always `stdint`**: `uint8_t`, `uint16_t`, `uint32_t`,
  `int16_t`, `int32_t`. Never `unsigned`, `unsigned char`, `short` or `long`.
  This is a port of 16-bit assembly, where every value has a width the
  original depended on - `int16_t` says "this truncation is the `imul`'s" in a
  way `short` does not, and a bare `int` says nothing at all. `char` stays
  `char` for actual strings. `reconstruct/tostdint.py` did the conversion and
  is comment-aware, because "the compare is unsigned" is prose.
- The C port is **structured C that reads as a game**, not transliterated
  register-shuffling — checked against the emulator rather than assumed. If a
  routine genuinely cannot be written honestly as structured C, write it
  literally and say why.
