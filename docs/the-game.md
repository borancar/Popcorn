# The game, and the binary it is

What Popcorn is, and what kind of program it turned out to be. Everything
below was read out of `popcorn.exe` or its readme.

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
loads `POPTAB.PPC` - the format is in
[docs/level-format.md](level-format.md). `POPSPEED.EXE` sets the game
speed for machines faster than an 8 MHz 8086, and does less than its name
suggests: [docs/utilities.md](utilities.md).

## What the binary is

**Hand-written 16-bit x86 assembly.** Not compiled. The evidence:

- **zero** `push bp; mov bp,sp` sequences in 23,696 bytes of code
- no C runtime at all — the entry point copies the PSP command tail and starts
  working
- one code segment, one flat data area, `DS = 0` **almost** throughout, data
  addressed as absolute offsets (`mov byte ptr [0x2d4f], al`). The exception is
  the ending: `screen_all_levels_done` sets `DS = 0xc46` and leaves it there,
  so `ending_blob`, `ending_blobs` and `ending_walk` reach `0x28d9`, `0x289d`
  and `0x2823` **through that segment**. Reading them as plain image offsets
  is right everywhere else in the program and wrong there, which is exactly
  the kind of mistake a convention invites - it took a register dump to see
  it, because the code looks identical either way
- arguments in registers, threaded across calls (`dl` as a row counter, `bx`
  as a width, `si`/`di` as cursors)
- flags poked into the code segment itself (`cs:[0x84]`, the sound-enable bit,
  written by the INT 09h handler)

That makes the port easier than a compiled one: there are no compiler idioms to
reverse, and every instruction is a decision someone made.

The last point is why the port reads as it does. There are no compiler idioms
to undo, so a routine can be written as the thing it does rather than as the
registers it shuffles - and where that is not honest, it is written literally
and says so.
