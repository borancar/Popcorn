# Popcorn — the game's own code, reconstructed

C reconstructed from the disassembly of **Popcorn** (Christophe Lacaze /
Frédérick Raynal, LACRAL software, 1988), a routine at a time, each carrying
the address in the original it was read from. It builds and plays:

```sh
make && ./popcorn
```

**The game is here**, because its authors put it in the public domain. From
`popcorn.doc`, in their own words:

> Ce programme fait parti du domaine public. Il ne doit servir à aucune fin
> commerciale sans accord préalable de […]

Public domain, then, with the one condition they attached: **not for
commercial use without their prior agreement.** That condition travels with
the files, and it is theirs rather than this repository's to waive.

So `popcorn.exe` is here, and the port needs it — every sprite, font, level
table and string the game has lives in the first 0x1ac20 bytes of that
executable, which the port unpacks at startup and reads its data from. Nothing
of the game is embedded in the C; it is read at run time from the file beside
it. The two shipped level sets are here too, and `POPGEN.EXE`, which made
them, and `POPSPEED.EXE`, which sets the game speed:

```sh
make && ./popcorn          # the fifty levels built into the executable
./popcorn POPTAB           # POPTAB.PPC instead, as POPGEN wrote it
./popcorn LTF              # LTF.PPC
```

`POPCORN_EXE` points at a copy kept somewhere else. Everything the game reads
and writes — the executable, the `.ppc` sets, and `popcorn.hsc` — is relative
to the current directory, as it was under DOS. `popcorn.hsc` is not in here:
the game writes it, so it is a save file rather than part of the game, and one
player's scores are nobody else's starting point.

Needs SDL3 and a C99 compiler. `make` prints what to install if it cannot find
SDL3.

## What this is

The original is **hand-written 16-bit x86 assembly** — not compiled. There are
zero `push bp; mov bp,sp` sequences in its 23,696 bytes of code, no C runtime,
one code segment and one flat data area, with arguments passed in registers and
threaded across calls. So there are no compiler idioms to reverse and every
instruction is a decision someone made, which is why this is written as
structured C that reads as a game rather than as transliterated
register-shuffling. Where a routine cannot honestly be written that way, it is
written literally and says why.

Every integer is a `stdint` one. `int16_t` says "this truncation is the
`imul`'s"; `short` only says "some signed type".

## How it is known to be right

It is not asserted, it is measured. The port runs beside an emulator running
the original, in lockstep, and the two are compared **byte for byte** — the
whole 133,296-byte image and the 16K of video memory, every frame. Separately,
each routine is captured at its entry, the original is run to its return, the C
is run on the same capture, and the results are diffed.

That machinery lives in the repository this was split from, along with the
disassembly notes: <https://github.com/borancar/Popcorn/tree/develop>.

A second binary, `popcorn-dev`, carries the flags that exist to *check* the
port — the lockstep protocol, single-routine verification, screenshots, a bot
that plays the game by itself. `popcorn` takes what the original takes and
nothing else, because that command line is part of what the port is.

## Files

**`src/` is the game. `tools/` is what exists to check it.**

| | |
| --- | --- |
| `src/main.c` | `popcorn` — the game, and its one optional argument |
| `src/game.c` | the transcription: every routine, each with its `1ac2:xxxx` |
| `src/game.h` | types, the named image offsets, the backend interface |
| `src/exepack.c` | the EXEPACK decoder that recovers the data at startup |
| `src/sdl_io.c` | window, presentation, keyboard, mouse, retrace, delay |
| `src/stubs.c` | what is not transcribed. One safety net is left in it |
| `tools/devmain.c` | `popcorn-dev` — the same game with the harness flags |
| `tools/autoplay.c` | the bot, for `popcorn-dev --autoplay` |
| `tools/lockstep.c`, `tools/verify.c` | the two halves of the checking, on this side |

Both binaries link both halves, because the game's own code calls into them:
`sdl_io.c` asks the bot whether it is driving, and `game.c` offers the extra
sync points the driver compares screens at. They cost nothing when nothing has
turned them on, and the alternative is `#ifdef`s through a transcription,
which would make it harder to read against the disassembly than it needs to
be.

## Licence

Two works, and they are not the same one.

**The game** — `popcorn.exe`, the `.ppc` sets, `popgen.exe`, `popspeed.exe`,
`popcorn.doc` — is Christophe Lacaze's and Frédérick Raynal's, placed by them
in the public domain in `popcorn.doc`, with the condition that it not be put
to commercial use without their prior agreement. Redistributing it here rests
on that declaration and carries that condition with it.

**The reconstruction** — the C, the Makefile, this README — is a separate work
written from the disassembly. Ask before assuming a licence for it.
