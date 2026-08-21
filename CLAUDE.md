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
| `0xc460+0x3df0` | 32,000-byte backup of the CGA screen |

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
| `0x5630` | the screen blit: waits on 0x3da bit 3, then `rep movsb` per row |

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
| `unpack_popcorn.py` | EXEPACK recovery by running the stub under Unicorn |
| `validate.py` | round-trips the result against the stub's own output |
| `trace_dos.py` | headless DOS/BIOS shim; **read-only** on the host filesystem |
| `emulation.py` | CGA + SDL window on top of it — the reference implementation |
| `analyze.py` | recursive-descent map of the code segment |
| `tools_dis.py` | disassemble a range; `load_image()` is the shared loader |

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

## Conventions

- Addresses are **image offsets** unless written `seg:off`. Every reconstructed
  routine carries the offset it was read from, so any line can be checked back
  against the binary.
- Where a name or a type is a guess, say so.
- The C port is **structured C that reads as a game**, not transliterated
  register-shuffling — checked against the emulator rather than assumed. If a
  routine genuinely cannot be written honestly as structured C, write it
  literally and say why.
